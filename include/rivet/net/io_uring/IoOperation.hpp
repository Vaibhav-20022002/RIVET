#pragma once

#include "rivet/memory/PacketRef.hpp"
#include <array>
#include <cstdint>
#include <sys/uio.h>

/// Forward declare liburing structures to avoid dragging in the header here
struct io_uring;
struct io_uring_sqe;

namespace rivet::memory {
template<typename T> class MemoryPool;
}

namespace rivet::net::io_uring {

using rivet::memory::MemoryPool;

/// Forward declarations to resolve circular dependencies
class EgressReactor;

/// Result record returned to application fiber for send operations
struct SendResult {
  int32_t  result;
  uint32_t bytes_transferred;
};

/// Result record returned to application fiber for receive operations
struct RecvResult {
  int32_t           result;
  uint32_t          bytes_transferred;
  memory::PacketRef packet;
};

/// Indicates whether an SQE was successfully pushed to the kernel or needs queuing
enum class ResubmitResult : uint8_t { Submitted, Deferred, Failed };

/// The core state machine representing a single asynchronous kernel I/O request.
struct IoOperation {
  enum class OpType : uint8_t { Send, SendZeroCopy, Recv, RecvExact };
  enum class State : uint8_t { InFlight, Partial, Backpressure, Completed, Failed, Eof };
  enum class CompletionKind : uint8_t { Io, Cancel };

  struct CompletionTag {
    IoOperation   *operation;
    CompletionKind kind;
  };

  /// Non-copyable, non-movable : Always allocated from a stable memory pool
  IoOperation() noexcept                      = default;
  IoOperation(const IoOperation &)            = delete;
  IoOperation &operator=(const IoOperation &) = delete;
  IoOperation(IoOperation &&)                 = delete;
  IoOperation &operator=(IoOperation &&)      = delete;

  OpType  type{OpType::Send};
  State   state{State::InFlight};
  int     fd{-1};
  int32_t result{0};

  struct iovec iov[2];
  int          iov_count{0};
  uint32_t     total_bytes{0};
  uint32_t     bytes_transferred{0};

  memory::PacketRef packet;
  /// Operation-owned header storage for '$' prefix, CSRCs & extensions
  std::array<uint8_t, 128> inline_hdr{};

  /// Use void* here as a placeholder for boost::fibers::context* to avoid pulling in the heavy
  /// boost headers in this core struct.
  void *waiter{nullptr};

  uint32_t pending_submissions{0};   //< Prepared I/O SQEs awaiting transfer CQEs
  uint32_t pending_notifications{0}; //< Tracks promised zero-copy buffer notifications (F_MORE)
  uint32_t pending_cancels{0};       //< Prepared cancellation SQEs awaiting their own CQEs

  bool         in_pending_queue{false}; //< True when enqueued in EgressReactor::pending_queue
  bool         in_cancel_queue{false};
  IoOperation *cancel_next{nullptr}; //< Intrusive cancellation queue link

  uint32_t dispatch_depth{0};        //< Active, possibly nested dispatch frames
  bool     cancelled{false};         //< True if explicitly cancelled by reator
  bool     transfer_done{false};     //< True when all requested bytes have been transferred
  bool     zc_notified{false};       //< True when all promised notifications have arrived
  bool     consumer_released{false}; //< True when application has taken the result

  CompletionTag io_completion{this, CompletionKind::Io};
  CompletionTag cancel_completion{this, CompletionKind::Cancel};

  [[nodiscard]] bool is_terminal() const noexcept {
    return state == State::Completed || state == State::Failed || state == State::Eof;
  }

  [[nodiscard]] bool is_queued() const noexcept { return in_pending_queue; }

  /// No queue, kernel request, consumer, or active dispatch may retain this record.
  [[nodiscard]] bool is_deallocatable() const noexcept {
    return is_terminal() && !is_queued() && !in_cancel_queue && pending_submissions == 0 &&
        pending_notifications == 0 && pending_cancels == 0 && consumer_released &&
        dispatch_depth == 0;
  }

  /// Attempt reclamation back to the reactor's memory pool (implemented in .cpp)
  void try_reclaim(MemoryPool<IoOperation> &op_pool) noexcept;

  /// Safely releases backing buffer only when terminal and all kernel DMA has ceased
  void try_release_buffer() noexcept {
    if (is_terminal() && !is_queued() && pending_submissions == 0 && pending_notifications == 0) {
      packet.reset();
    }
  }

  /// Resumes the waiting fiber (implemented in .cpp)
  void notify_waiter() noexcept;

  /// Explicit cancellation (implemented in .cpp)
  void cancel(MemoryPool<IoOperation> &op_pool, EgressReactor &reactor) noexcept;

  /// Called by application waiter to extract send results without releasing kernel-pinned payload
  [[nodiscard]] SendResult take_send_result(MemoryPool<IoOperation> &op_pool) noexcept {
    consumer_released = true;
    SendResult res{.result = result, .bytes_transferred = bytes_transferred};
    try_reclaim(op_pool);
    return res;
  }

  /// Called by application waiter to extract receive results, moving buffer ownership
  [[nodiscard]] RecvResult take_recv_result(MemoryPool<IoOperation> &op_pool) noexcept {
    consumer_released = true;
    RecvResult res{.result = result, .bytes_transferred = bytes_transferred, .packet = {}};

    /// Retain operation ownership of the buffer handle on failure/cancellation to prevent
    /// use-after-free while kernel completions are pending.
    if ((state == State::Completed || state == State::Eof) && pending_submissions == 0 &&
        pending_notifications == 0) {
      res.packet = std::move(packet);
    }

    try_reclaim(op_pool);
    return res;
  }

  /// **----- Kernel Preparation and Dispatch Methods (Implemented in .cpp) -----**
  ResubmitResult try_prep_sqe_recv(struct io_uring &ring) noexcept;
  ResubmitResult try_prep_sqe_send(struct io_uring &ring) noexcept;
  void           advance_iovecs(uint32_t transferred) noexcept;

  void on_cqe(int32_t          res,
      uint32_t                 flags,
      struct io_uring         &ring,
      MemoryPool<IoOperation> &op_pool,
      EgressReactor           *reactor) noexcept;

  [[nodiscard]] int on_cancel_cqe(int32_t res, MemoryPool<IoOperation> &op_pool) noexcept;

  /// RAII guard to prevent reentrant reclamation during completion or failure dispatch
  struct DispatchGuard {
    IoOperation             *op;
    MemoryPool<IoOperation> &pool;

    DispatchGuard(IoOperation *o, MemoryPool<IoOperation> &p) noexcept
        : op(o)
        , pool(p) {
      ++op->dispatch_depth;
    }
    DispatchGuard(const DispatchGuard &)            = delete;
    DispatchGuard &operator=(const DispatchGuard &) = delete;
    ~DispatchGuard() noexcept; //< Implemented in .cpp to call try_reclaim
  };
};

} //< namespace rivet::net::io_uring
