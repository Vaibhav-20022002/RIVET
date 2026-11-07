#pragma once

#include "../../memory/MemoryPool.hpp"
#include "IoOperation.hpp"
#include <cstddef>

#if defined(RIVET_HAS_IO_URING)
#include <cstdint>
#include <liburing.h>
#else
struct io_uring;
#endif

namespace rivet::net::io_uring {

using rivet::memory::MemoryPool;

/// EgressReactor manages the io_uring submission/completion backpressure queues and multi-domain
/// `IoOperation` reclamation for a single CPU core on Linux.
class EgressReactor {
public:
  static constexpr size_t PENDING_RESUBMIT_CAPACITY = 1024;
  static constexpr size_t CQE_BATCH_CAPACITY        = 256;

private:
  /// Fixed-capacity circular buffer of backpressured `IoOperation*`s awaiting SQE slots.
  IoOperation *pending_queue[PENDING_RESUBMIT_CAPACITY]{};
  size_t       pending_head{0};
  size_t       pending_tail{0};
  size_t       pending_count{0};

  /// Zero-allocation intrusive linked list of operations awaiting `io_uring_prep_cancel`.
  IoOperation *cancel_head{nullptr};
  IoOperation *cancel_tail{nullptr};
  size_t       cancel_count{0};

public:
  EgressReactor()                                 = default;
  EgressReactor(const EgressReactor &)            = delete;
  EgressReactor &operator=(const EgressReactor &) = delete;

  /// Enqueues an operation into the intrusive cancellation list (idempotent via `in_cancel_queue`).
  void enqueue_cancel(IoOperation *op) noexcept {
    if (!op || op->in_cancel_queue) {
      return;
    }
    op->cancelled       = true;
    op->in_cancel_queue = true;
    op->cancel_next     = nullptr;
    if (!cancel_tail) {
      cancel_head = op;
      cancel_tail = op;
    } else {
      cancel_tail->cancel_next = op;
      cancel_tail              = op;
    }
    ++cancel_count;
  }

  /// Parks a backpressured operation in the resubmission ring buffer (idempotent).
  [[nodiscard]] bool enqueue_pending_resubmit(IoOperation *op) noexcept {
    if (!op) {
      return false;
    }
    if (op->in_pending_queue) {
      return true;
    }
    if (pending_count >= PENDING_RESUBMIT_CAPACITY) {
      return false;
    }
    op->in_pending_queue        = true;
    op->state                   = IoOperation::State::Backpressure;
    pending_queue[pending_tail] = op;
    pending_tail                = (pending_tail + 1) % PENDING_RESUBMIT_CAPACITY;
    ++pending_count;
    return true;
  }

  /// Pops the next operation from the resubmission ring (used by drain loops & unit tests).
  [[nodiscard]] IoOperation *pop_pending_resubmit() noexcept {
    if (pending_count == 0) {
      return nullptr;
    }
    IoOperation *op             = pending_queue[pending_head];
    pending_queue[pending_head] = nullptr;
    pending_head                = (pending_head + 1) % PENDING_RESUBMIT_CAPACITY;
    --pending_count;
    if (op) {
      op->in_pending_queue = false;
    }
    return op;
  }

  /// Reclaims `op` back to `op_pool` strictly when `op->is_deallocatable()` holds across all domains.
  static bool try_reclaim(IoOperation *op, MemoryPool<IoOperation> &op_pool) noexcept {
    if (!op || !op->is_deallocatable()) {
      return false;
    }
    op->try_release_buffer();
    op_pool.deallocate(op);
    return true;
  }

  /// Pushes queued cancellation requests to the kernel SQ when `RIVET_HAS_IO_URING` is active.
  void drain_pending_cancels(struct io_uring &ring, MemoryPool<IoOperation> &op_pool) noexcept {
#if defined(RIVET_HAS_IO_URING)
    while (cancel_head) {
      IoOperation *op = cancel_head;
      if (op->pending_submissions == 0) {
        cancel_head = op->cancel_next;
        if (!cancel_head) {
          cancel_tail = nullptr;
        }
        op->cancel_next     = nullptr;
        op->in_cancel_queue = false;
        --cancel_count;
        (void)try_reclaim(op, op_pool);
        continue;
      }

      struct io_uring_sqe *sqe = ::io_uring_get_sqe(&ring);
      if (!sqe) {
        break; //< SQ saturated; retry next tick
      }
      cancel_head = op->cancel_next;
      if (!cancel_head) {
        cancel_tail = nullptr;
      }
      op->cancel_next     = nullptr;
      op->in_cancel_queue = false;
      --cancel_count;

      ::io_uring_prep_cancel(sqe, &op->io_completion, 0);
      ::io_uring_sqe_set_data(sqe, &op->cancel_completion);
      ++op->pending_cancels;
    }
#else
    (void)ring;
    while (cancel_head) {
      IoOperation *op     = cancel_head;
      cancel_head         = op->cancel_next;
      op->cancel_next     = nullptr;
      op->in_cancel_queue = false;
      --cancel_count;
      (void)try_reclaim(op, op_pool);
    }
    cancel_tail = nullptr;
#endif
  }

  /// Re-attempts SQE submission for backpressured send/recv operations.
  void drain_pending_resubmits(struct io_uring &ring, MemoryPool<IoOperation> &op_pool) noexcept {
    const size_t n = pending_count;
    for (size_t i = 0; i < n; ++i) {
      IoOperation *op = pop_pending_resubmit();
      if (!op) {
        break;
      }
      if (op->cancelled) {
        (void)try_reclaim(op, op_pool);
        continue;
      }
#if defined(RIVET_HAS_IO_URING)
      const ResubmitResult resub =
          (op->type == IoOperation::OpType::Recv || op->type == IoOperation::OpType::RecvExact)
          ? op->try_prep_sqe_recv(ring)
          : op->try_prep_sqe_send(ring);
      if (resub == ResubmitResult::Deferred) {
        (void)enqueue_pending_resubmit(op);
        break;
      }
#else
      (void)ring;
      (void)try_reclaim(op, op_pool);
#endif
    }
  }

#if defined(RIVET_HAS_IO_URING)
  /// Drains up to 256 CQEs in a single non-blocking batch.
  size_t drain_completions(struct io_uring &ring, MemoryPool<IoOperation> &op_pool) noexcept {
    struct io_uring_cqe *cqes[CQE_BATCH_CAPACITY]{};
    const unsigned       got = ::io_uring_peek_batch_cqe(&ring, cqes, CQE_BATCH_CAPACITY);
    for (unsigned i = 0; i < got; ++i) {
      auto *tag = static_cast<IoOperation::CompletionTag *>(::io_uring_cqe_get_data(cqes[i]));
      if (!tag || !tag->operation) {
        continue;
      }
      IoOperation   *op  = tag->operation;
      const int32_t  res = cqes[i]->res;
      const uint32_t fl  = cqes[i]->flags;
      if (tag->kind == IoOperation::CompletionKind::Cancel) {
        (void)op->on_cancel_cqe(res, op_pool);
      } else {
        op->on_cqe(res, fl, ring, op_pool, this);
      }
    }
    if (got > 0) {
      ::io_uring_cq_advance(&ring, got);
    }
    return got;
  }
#endif

  [[nodiscard]] size_t get_pending_count() const noexcept { return pending_count; }
  [[nodiscard]] size_t get_cancel_count() const noexcept { return cancel_count; }
};

} //< namespace rivet::net::io_uring
