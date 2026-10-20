#if defined(__linux__)

#include "IoOperation.hpp"

#include "EgressReactor.hpp"
#include "rivet/memory/MemoryPool.hpp"
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <liburing.h>
#include <sys/socket.h>

namespace rivet::net::io_uring {

/// **----- EgressReactor Queue Implementations -----**

void EgressReactor::enqueue_cancel(IoOperation *op) noexcept {
  assert(!op->in_cancel_queue && op->pending_cancels == 0);
  op->in_cancel_queue = true;
  op->cancel_next     = nullptr;

  /// O(1) insertion into the intrusive linked list.
  /// We use the embedded 'cancel_next' pointer inside IoOperation to avoid allocating memory for a
  /// queue node during an active teardown.
  if (cancel_tail) {
    cancel_tail->cancel_next = op;
  } else {
    cancel_head = op;
  }

  cancel_tail = op;
}

bool EgressReactor::enqueue_pending_resubmit(IoOperation *op) noexcept {
  if (pending_count >= PENDING_RESUBMIT_CAPACITY) {
    return false; //< Queue is full, unrecoverable backpressure
  }

  op->in_pending_queue        = true;
  pending_queue[pending_tail] = op;
  pending_tail                = (pending_tail + 1) % PENDING_RESUBMIT_CAPACITY;
  ++pending_count;
  return true;
}

void EgressReactor::drain_pending_cancels(struct io_uring &ring,
    MemoryPool<IoOperation>                               &op_pool) noexcept {
  while (cancel_head) {
    auto                      *op = cancel_head;
    IoOperation::DispatchGuard guard{op, op_pool};

    /// Only push a cancel SQE to the kernel if the original operation actually has pending
    /// submissions. If it doesn't, it already finished natively.
    if (op->pending_submissions > 0) {
      auto *sqe = io_uring_get_sqe(&ring);
      if (!sqe) {
        break; //< SQ is full again. Stop draining and try again next reactor tick
      }

      /// Instruct io_uring to find and abort the pending operation. We pass '&op->io_completion' to
      /// uniquely identify the target request.
      io_uring_prep_cancel(sqe, &op->io_completion, 0);
      io_uring_sqe_set_data(sqe, &op->cancel_completion);
      ++op->pending_cancels;
    }

    /// Remove from the intrusive list
    cancel_head = op->cancel_next;
    if (!cancel_head) {
      cancel_tail = nullptr;
    }

    op->cancel_next     = nullptr;
    op->in_cancel_queue = false;
    op->try_release_buffer();
  }
}
void EgressReactor::drain_pending_resubmits(struct io_uring &ring,
    MemoryPool<IoOperation>                                 &op_pool) noexcept {
  while (pending_count > 0) {
    auto                      *op = pending_queue[pending_head];
    IoOperation::DispatchGuard guard{op, op_pool};

    /// If this operation was explicitly cancelled while it was waiting in the backpressure queue,
    /// purge it immediately without submitting.
    if (op->is_terminal()) {
      pending_head = (pending_head + 1) % PENDING_RESUBMIT_CAPACITY;
      --pending_count;
      op->in_pending_queue = false;
      op->try_release_buffer();
      continue;
    }

    /// Re-attempt to acquire an SQE and format it
    ResubmitResult resub = (op->type == IoOperation::OpType::RecvExact)
        ? op->try_prep_sqe_recv(ring)
        : op->try_prep_sqe_send(ring);

    if (resub == ResubmitResult::Submitted) {
      pending_head = (pending_head + 1) % PENDING_RESUBMIT_CAPACITY;
      --pending_count;
      op->in_pending_queue = false;
      ++op->pending_submissions;
    } else if (resub == ResubmitResult::Deferred) {
      break; //< SQ is still saturated. Leave the entry at the head for the next loop
    } else {
      /// Failed (unrecoverable error preparing the SQE)
      pending_head = (pending_head + 1) % PENDING_RESUBMIT_CAPACITY;
      --pending_count;
      op->in_pending_queue = false;
      op->try_release_buffer();
      op->notify_waiter();
    }
  }
}

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

void IoOperation::notify_waiter() noexcept {
  ///\todo: Hook this up tp boost::fibers
}

void IoOperation::cancel(MemoryPool<IoOperation> &op_pool, EgressReactor &reactor) noexcept {
  DispatchGuard guard{this, op_pool};
  if (cancelled || is_terminal()) {
    return;
  }

  cancelled = true;
  state     = State::Failed;
  result    = -ECANCELED;

  /// If the kernel is currently processing this operation, we must actively cancel it
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

  /// -ENOENT means the kernel couldn't find the request (it already finished natively).
  /// -EALREADY means it was already in the process of being cancelled.
  /// Both are safe to ignore
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

  struct msghdr msg{};
  msg.msg_iov    = iov;
  msg.msg_iovlen = iov_count;

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

  struct msghdr msg{};
  msg.msg_iov    = iov;
  msg.msg_iovlen = iov_count;

  if (type == OpType::SendZeroCopy) {
    io_uring_prep_sendmsg_zc(sqe, fd, &msg, 0);
  } else {
    io_uring_prep_sendmsg(sqe, fd, &msg, 0);
  }

  io_uring_sqe_set_data(sqe, &io_completion);
  state = State::InFlight;
  return ResubmitResult::Submitted;
}

void IoOperation::advance_iovecs(uint32_t transferred) noexcept {
  /// Scans through the scatter-gather iovecs to correctly advance the memory pointers by the exact
  /// amount of bytes tranferred. This allows partial TCP sends to resume seamlessly without sending
  /// duplicate data.
  for (int i = 0; i < iov_count; ++i) {
    if (transferred >= iov[i].iov_len) {
      transferred -= iov[i].iov_len;
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

  /// 1. ZERO-COPY NOTIFICATION CQE

  /// This indicates the network card has physically finished reading the memory buffer, so it is
  /// finally safe to release our packet reference.
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

  /// 2. TRANSFER CQE
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

  /// 3. ASYNCHRONOUS ERROR PATH
  if (res < 0) [[unlikely]] {
    state = State::Failed;
    try_release_buffer();
    notify_waiter();
    return;
  }

  /// 4. RECEIVE PATH
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
      /// Partial exact receive: advance buffer and resubmit
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

  /// 5. Send Path
  bytes_transferred += static_cast<uint32_t>(res);

  if (bytes_transferred >= total_bytes) {
    transfer_done = true;
    state         = State::Completed;
    try_release_buffer();
    notify_waiter();
  } else {
    /// Partial send (usually TCP backpressure): advance buffer and resubmit
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

#endif //< defined(__linux__)