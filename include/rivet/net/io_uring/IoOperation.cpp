#if defined(__linux__) && defined(RIVET_HAS_IO_URING)

#include "IoOperation.hpp"

#include "EgressReactor.hpp"
#include "rivet/memory/MemoryPool.hpp"
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <liburing.h>
#include <sys/socket.h>

namespace rivet::net::io_uring {

/// **----- IoOperation Implementations -----**

IoOperation::DispatchGuard::~DispatchGuard() noexcept {
  --op->dispatch_depth;
  op->try_reclaim(pool);
}

void IoOperation::try_reclaim(MemoryPool<IoOperation> &op_pool) noexcept {
  if (is_deallocatable()) {
    op_pool.deallocate(this);
  }
}

void IoOperation::notify_waiter() noexcept {}

void IoOperation::cancel(MemoryPool<IoOperation> &op_pool, EgressReactor &reactor) noexcept {
  DispatchGuard guard{this, op_pool};
  if (cancelled || is_terminal()) {
    return;
  }

  cancelled = true;
  state     = State::Failed;
  result    = -ECANCELED;

  if (pending_submissions > 0) {
    reactor.enqueue_cancel(this);
  }

  try_release_buffer();
  notify_waiter();
}

int IoOperation::on_cancel_cqe(int32_t res, MemoryPool<IoOperation> &op_pool) noexcept {
  DispatchGuard guard{this, op_pool};
  assert(pending_cancels > 0);
  --pending_cancels;

  try_release_buffer();

  if (res == 0 || res == -ENOENT || res == -EALREADY) {
    return 0;
  }

  return res < 0 ? res : -EIO;
}

ResubmitResult IoOperation::try_prep_sqe_recv(struct io_uring &ring) noexcept {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    return ResubmitResult::Deferred;
  }

  /// Populate operation-owned `this->msg` so kernel SQE never points to a dead stack frame
  msg            = {};
  msg.msg_iov    = iov;
  msg.msg_iovlen = static_cast<size_t>(iov_count);

  io_uring_prep_recvmsg(sqe, fd, &msg, 0);
  io_uring_sqe_set_data(sqe, &io_completion);

  state = State::InFlight;
  return ResubmitResult::Submitted;
}

ResubmitResult IoOperation::try_prep_sqe_send(struct io_uring &ring) noexcept {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    return ResubmitResult::Deferred;
  }

  /// Populate operation-owned `this->msg` so kernel SQE never points to a dead stack frame
  msg            = {};
  msg.msg_iov    = iov;
  msg.msg_iovlen = static_cast<size_t>(iov_count);

  if (type == OpType::SendZeroCopy) {
    io_uring_prep_sendmsg_zc(sqe, fd, &msg, MSG_NOSIGNAL);
  } else {
    io_uring_prep_sendmsg(sqe, fd, &msg, MSG_NOSIGNAL);
  }

  io_uring_sqe_set_data(sqe, &io_completion);
  state = State::InFlight;
  return ResubmitResult::Submitted;
}

void IoOperation::advance_iovecs(uint32_t transferred) noexcept {
  for (int i = 0; i < iov_count; ++i) {
    if (transferred >= iov[i].iov_len) {
      transferred -= static_cast<uint32_t>(iov[i].iov_len);
      iov[i].iov_base = nullptr;
      iov[i].iov_len  = 0;
    } else {
      iov[i].iov_base = static_cast<char *>(iov[i].iov_base) + transferred;
      iov[i].iov_len -= transferred;
      break;
    }
  }
}

void IoOperation::on_cqe(int32_t res,
    uint32_t                     flags,
    struct io_uring             &ring,
    MemoryPool<IoOperation>     &op_pool,
    EgressReactor               *reactor) noexcept {
  IoOperation::DispatchGuard guard{this, op_pool};

  if (flags & IORING_CQE_F_NOTIF) {
    if (pending_notifications > 0) {
      --pending_notifications;
    }
    if (pending_notifications == 0) {
      zc_notified = true;
    }

    try_release_buffer();
    if (is_terminal()) notify_waiter();
    return;
  }

  if (pending_submissions > 0) {
    --pending_submissions;
  }
  if (flags & IORING_CQE_F_MORE) {
    ++pending_notifications;
  }

  if (cancelled) {
    try_release_buffer();
    return;
  }

  result = res;

  if (res < 0) [[unlikely]] {
    state = State::Failed;
    try_release_buffer();
    notify_waiter();
    return;
  }

  if (type == OpType::Recv || type == OpType::RecvExact) {
    if (res == 0) {
      state = State::Eof;
      try_release_buffer();
      notify_waiter();
      return;
    }

    bytes_transferred += static_cast<uint32_t>(res);

    if (type == OpType::Recv || bytes_transferred >= total_bytes) {
      transfer_done = true;
      state         = State::Completed;
      notify_waiter();
    } else {
      state = State::Partial;
      advance_iovecs(static_cast<uint32_t>(res));

      ResubmitResult resub = try_prep_sqe_recv(ring);
      if (resub == ResubmitResult::Submitted) {
        ++pending_submissions;
      } else if (resub == ResubmitResult::Deferred) {
        if (reactor && reactor->enqueue_pending_resubmit(this)) {
          state = State::Backpressure;
        } else {
          result = -ENOBUFS;
          state  = State::Failed;
          try_release_buffer();
          notify_waiter();
        }
      } else {
        state = State::Failed;
        try_release_buffer();
        notify_waiter();
      }
    }
    return;
  }

  bytes_transferred += static_cast<uint32_t>(res);

  if (bytes_transferred >= total_bytes) {
    transfer_done = true;
    state         = State::Completed;
    try_release_buffer();
    notify_waiter();
  } else {
    state = State::Partial;
    advance_iovecs(static_cast<uint32_t>(res));

    ResubmitResult resub = try_prep_sqe_send(ring);
    if (resub == ResubmitResult::Submitted) {
      ++pending_submissions;
    } else if (resub == ResubmitResult::Deferred) {
      if (reactor && reactor->enqueue_pending_resubmit(this)) {
        state = State::Backpressure;
      } else {
        result = -ENOBUFS;
        state  = State::Failed;
        try_release_buffer();
        notify_waiter();
      }
    } else {
      state = State::Failed;
      try_release_buffer();
      notify_waiter();
    }
  }
}

} //< namespace rivet::net::io_uring

#endif //< defined(__linux__) && defined(RIVET_HAS_IO_URING)
