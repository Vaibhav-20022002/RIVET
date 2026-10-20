#pragma once

#include "IoOperation.hpp"
#include <cstddef>

/// Forward declare liburing structures
struct io_uring;

namespace rivet::memory {
template<typename T> class MemoryPool;
}

namespace rivet::net::io_uring {

using rivet::memory::MemoryPool;

/// Forward declarations
struct IoOperation;

/// EgressReactor manages the io_uring event loop and handles backpressure queuing for a single CPU
/// core
class EgressReactor {

public:
  /// Maximum number of I/O operations that can be queued for resubmission
  /// when the io_uring submission queue (SQ) is temporarily full.
  static constexpr size_t PENDING_RESUBMIT_CAPACITY = 1024;

private:
  /// **----- Resubmission Ring Buffer -----**

  /// A fixed-size array acting as a circular buffer. This handles "backpressure".
  /// If the kernel's queue is full, we park the IoOperation here and retry it
  /// on the next event loop tick.
  IoOperation *pending_queue[PENDING_RESUBMIT_CAPACITY]{};
  size_t       pending_head{0};
  size_t       pending_tail{0};
  size_t       pending_count{0};

  /// **----- Intrusive Cancellation List ----**

  /// If a client disconnects, we need to cancel their pending operations.
  /// If the kernel SQ is full, we cannot submit the cancel request immediately.
  /// Rather than heap-allocating a queue for cancellations, we use a linked list
  /// pointer (`cancel_next`) embedded directly inside the `IoOperation` itself.
  IoOperation *cancel_head{nullptr};
  IoOperation *cancel_tail{nullptr};

public:
  EgressReactor()                                 = default;
  EgressReactor(const EgressReactor &)            = delete;
  EgressReactor &operator=(const EgressReactor &) = delete;

  /// **----- Queue Enqueue Methods -----**

  /// Adds a cancelled operation to the intrusive cancellation queue
  void enqueue_cancel(IoOperation *op) noexcept;

  /// Adds an operation to the backpressure ring buffer
  [[nodiscard]] bool enqueue_pending_resubmit(IoOperation *op) noexcept;

  /// **----- Queue Drain Methods ----**

  /// Attempts to push queued cancellation requests to the kernel
  void drain_pending_cancels(struct io_uring &ring, MemoryPool<IoOperation> &op_pool) noexcept;

  /// Attempts to push backpressured transfer requests to the kernel
  void drain_pending_resubmits(struct io_uring &ring, MemoryPool<IoOperation> &op_pool) noexcept;

  [[nodiscard]] size_t get_pending_count() const noexcept { return pending_count; }
};

} //< namespace rivet::net::io_uring