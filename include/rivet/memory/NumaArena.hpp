#pragma once

#include "PooledBuffer.hpp"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RIVET_HAS_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(RIVET_HAS_ASAN)
#define RIVET_HAS_ASAN 1
#endif

#if defined(RIVET_HAS_ASAN)
extern "C" {
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
}
#define RIVET_ASAN_POISON(addr, sz) __asan_poison_memory_region((addr), (sz))
#define RIVET_ASAN_UNPOISON(addr, sz) __asan_unpoison_memory_region((addr), (sz))
#else
#define RIVET_ASAN_POISON(addr, sz) ((void)0)
#define RIVET_ASAN_UNPOISON(addr, sz) ((void)0)
#endif

namespace rivet::memory {

/// Standard cacheline alignment
inline constexpr size_t CACHELINE_ALIGN = 64;

/// NumaArena is a NUMA-aware, zero-fragmentation, lock-free fixed-size slab allocator.
/// Uses an ABA-safe 64-bit tagged index Treiber stack plus an optional owner-thread local
/// magazine so single-writer ingest allocates with zero atomics and cross-reactor frees complete
/// without mutex contention.
class NumaArena {
public:
  static constexpr size_t   DEFAULT_SLAB_SIZE = 2048;
  static constexpr size_t   DEFAULT_CAPACITY  = 4096;
  static constexpr uint32_t kInvalidIndex     = 0xFFFFFFFFU;

private:
  size_t   slab_size_{DEFAULT_SLAB_SIZE};
  size_t   capacity_{0};
  uint8_t *raw_memory_{nullptr};
  bool     valid_{false};

  std::unique_ptr<PooledBuffer[]> descriptors_;
  std::unique_ptr<uint32_t[]>     next_free_idx_;

  /// Packed 64-bit Treiber head: [63..32: ABA generation tag | 31..0: descriptor index]
  alignas(64) std::atomic<uint64_t> free_head_{pack_head(0, kInvalidIndex)};

  /// Optional owner-thread magazine (zero-atomic fast path for IngestReactor Thread 0)
  std::atomic<bool>     has_owner_thread_{false};
  std::thread::id       owner_tid_{};
  std::vector<uint32_t> owner_magazine_;

  /// Telemetry counters
  alignas(64) std::atomic<size_t> active_allocations_{0};
  std::atomic<uint64_t> exhaustion_count_{0};

  [[nodiscard]] static constexpr uint64_t pack_head(uint32_t tag, uint32_t idx) noexcept {
    return (static_cast<uint64_t>(tag) << 32U) | static_cast<uint64_t>(idx);
  }

  [[nodiscard]] static constexpr uint32_t unpack_tag(uint64_t head) noexcept {
    return static_cast<uint32_t>(head >> 32U);
  }

  [[nodiscard]] static constexpr uint32_t unpack_idx(uint64_t head) noexcept {
    return static_cast<uint32_t>(head & 0xFFFFFFFFULL);
  }

  void push_shared_index(uint32_t idx) noexcept {
    uint64_t old_head = free_head_.load(std::memory_order_relaxed);
    for (;;) {
      next_free_idx_[idx]     = unpack_idx(old_head);
      const uint64_t new_head = pack_head(unpack_tag(old_head) + 1U, idx);
      if (free_head_.compare_exchange_weak(
              old_head, new_head, std::memory_order_release, std::memory_order_relaxed)) {
        return;
      }
    }
  }

  [[nodiscard]] uint32_t pop_shared_index() noexcept {
    uint64_t old_head = free_head_.load(std::memory_order_acquire);
    for (;;) {
      const uint32_t idx = unpack_idx(old_head);
      if (idx == kInvalidIndex) {
        return kInvalidIndex;
      }
      const uint32_t next     = next_free_idx_[idx];
      const uint64_t new_head = pack_head(unpack_tag(old_head) + 1U, next);
      if (free_head_.compare_exchange_weak(
              old_head, new_head, std::memory_order_acquire, std::memory_order_acquire)) {
        return idx;
      }
    }
  }

  /// Steals the entire shared Treiber list into `owner_magazine_` in one atomic RMW.
  void refill_owner_magazine() noexcept {
    uint64_t old_head = free_head_.load(std::memory_order_acquire);
    for (;;) {
      if (unpack_idx(old_head) == kInvalidIndex) {
        return;
      }
      const uint64_t empty_head = pack_head(unpack_tag(old_head) + 1U, kInvalidIndex);
      if (free_head_.compare_exchange_weak(
              old_head, empty_head, std::memory_order_acquire, std::memory_order_acquire)) {
        uint32_t cur = unpack_idx(old_head);
        while (cur != kInvalidIndex) {
          const uint32_t next = next_free_idx_[cur];
          owner_magazine_.push_back(cur);
          cur = next;
        }
        return;
      }
    }
  }

  [[nodiscard]] bool is_owner_caller() const noexcept {
    return has_owner_thread_.load(std::memory_order_acquire) &&
        std::this_thread::get_id() == owner_tid_;
  }

public:
  explicit NumaArena(size_t slab_capacity        = DEFAULT_CAPACITY,
      size_t                individual_slab_size = DEFAULT_SLAB_SIZE,
      bool                  advise_hugepages     = false) {
    if (slab_capacity == 0 || individual_slab_size == 0 ||
        slab_capacity >= static_cast<size_t>(kInvalidIndex)) {
      capacity_  = 0;
      slab_size_ = 0;
      return;
    }

    slab_size_ = individual_slab_size;
    if (slab_size_ % CACHELINE_ALIGN != 0) {
      size_t rem = CACHELINE_ALIGN - (slab_size_ % CACHELINE_ALIGN);
      if (slab_size_ > std::numeric_limits<size_t>::max() - rem) {
        capacity_ = 0;
        return;
      }
      slab_size_ += rem;
    }

    size_t total_bytes = 0;
    if (__builtin_mul_overflow(slab_capacity, slab_size_, &total_bytes)) {
      capacity_ = 0;
      return;
    }

    size_t align_bytes = CACHELINE_ALIGN;
#if defined(__linux__)
    if (advise_hugepages && total_bytes >= (2ULL * 1024 * 1024)) {
      align_bytes = 2ULL * 1024 * 1024;
    }
#else
    (void)advise_hugepages;
#endif

    void *mem_ptr = nullptr;
    if (posix_memalign(&mem_ptr, align_bytes, total_bytes) != 0 || mem_ptr == nullptr) {
      raw_memory_ = nullptr;
      capacity_   = 0;
      return;
    }
    raw_memory_ = static_cast<uint8_t *>(mem_ptr);

#if defined(__linux__) && defined(MADV_HUGEPAGE)
    if (advise_hugepages) {
      (void)::madvise(raw_memory_, total_bytes, MADV_HUGEPAGE);
    }
#endif

    capacity_      = slab_capacity;
    descriptors_   = std::make_unique<PooledBuffer[]>(capacity_);
    next_free_idx_ = std::make_unique<uint32_t[]>(capacity_);

    for (size_t i = 0; i < capacity_; ++i) {
      const uint32_t idx           = static_cast<uint32_t>(i);
      descriptors_[i].capacity     = static_cast<uint32_t>(slab_size_);
      descriptors_[i].pool_index   = idx;
      descriptors_[i].arena        = this;
      descriptors_[i].payload_data = reinterpret_cast<std::byte *>(raw_memory_ + (i * slab_size_));
      descriptors_[i].ref_count.store(0, std::memory_order_relaxed);
      descriptors_[i].recycle_fn = [](PooledBuffer *b) noexcept {
        if (b && b->arena) {
          b->arena->free_buffer(b);
        }
      };
      next_free_idx_[i] = (i == 0) ? kInvalidIndex : static_cast<uint32_t>(i - 1);
      RIVET_ASAN_POISON(descriptors_[i].payload_data, slab_size_);
    }

    free_head_.store(
        pack_head(1U, static_cast<uint32_t>(capacity_ - 1)), std::memory_order_release);
    valid_ = true;
  }

  ~NumaArena() {
    assert(active_allocations_.load(std::memory_order_acquire) == 0 &&
        "NumaArena destroyed while PooledBuffers remain in flight!");
    if (descriptors_ && raw_memory_) {
      for (size_t i = 0; i < capacity_; ++i) {
        RIVET_ASAN_UNPOISON(descriptors_[i].payload_data, slab_size_);
        descriptors_[i].arena = nullptr;
      }
    }
    if (raw_memory_) {
      std::free(raw_memory_);
      raw_memory_ = nullptr;
    }
  }

  /// Non-copyable, non-movable to guarantee descriptor pointer stability
  NumaArena(const NumaArena &)            = delete;
  NumaArena &operator=(const NumaArena &) = delete;
  NumaArena(NumaArena &&)                 = delete;
  NumaArena &operator=(NumaArena &&)      = delete;

  /// Binds the calling thread (e.g. IngestReactor Thread 0) as the zero-atomic fast-path owner.
  void bind_owner_thread() noexcept {
    owner_tid_ = std::this_thread::get_id();
    owner_magazine_.reserve(capacity_);
    has_owner_thread_.store(true, std::memory_order_release);
    refill_owner_magazine();
  }

  [[nodiscard]] bool is_valid() const noexcept { return valid_; }

  /// Allocates a PooledBuffer from the arena with initial `initial_refs` (default 1).
  ///\return Pointer to the PooledBuffer, or nullptr if arena is exhausted.
  [[nodiscard]] PooledBuffer *allocate(uint32_t initial_refs = 1) noexcept {
    if (!valid_) [[unlikely]] {
      exhaustion_count_.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }

    uint32_t idx = kInvalidIndex;
    if (is_owner_caller()) {
      if (owner_magazine_.empty()) {
        refill_owner_magazine();
      }
      if (!owner_magazine_.empty()) {
        idx = owner_magazine_.back();
        owner_magazine_.pop_back();
      }
    } else {
      idx = pop_shared_index();
    }

    if (idx == kInvalidIndex) [[unlikely]] {
      exhaustion_count_.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }

    PooledBuffer *buf = &descriptors_[idx];
    RIVET_ASAN_UNPOISON(buf->payload_data, slab_size_);
    buf->init_refs(initial_refs);
    active_allocations_.fetch_add(1, std::memory_order_relaxed);
    return buf;
  }

  /// Returns a PooledBuffer back to the arena's free list.
  /// Called automatically by PooledBuffer::release() when ref_count reaches 0.
  void free_buffer(PooledBuffer *buf) noexcept {
    if (!buf || buf->arena != this || buf->pool_index >= capacity_) {
      return;
    }

    RIVET_ASAN_POISON(buf->payload_data, slab_size_);
    const uint32_t idx = buf->pool_index;
    /// Decrement active_allocations_ BEFORE publishing `idx` onto the free list so `in_flight()`
    /// and `~NumaArena()` never observe a freed slab as still in flight.
    active_allocations_.fetch_sub(1, std::memory_order_release);

    if (is_owner_caller()) {
      owner_magazine_.push_back(idx);
    } else {
      push_shared_index(idx);
    }
  }

  /// Number of currently available buffers
  [[nodiscard]] size_t available() const noexcept {
    const size_t inflight = active_allocations_.load(std::memory_order_acquire);
    return (inflight <= capacity_) ? (capacity_ - inflight) : 0;
  }

  /// Total capacity of the arena in slabs
  [[nodiscard]] size_t total_capacity() const noexcept { return capacity_; }

  /// Number of buffers currently in flight
  [[nodiscard]] size_t in_flight() const noexcept {
    return active_allocations_.load(std::memory_order_acquire);
  }

  /// Cumulative number of failed `allocate()` attempts due to pool exhaustion
  [[nodiscard]] uint64_t exhaustion_count() const noexcept {
    return exhaustion_count_.load(std::memory_order_relaxed);
  }

  /// Size in bytes of each individual slab
  [[nodiscard]] size_t get_slab_size() const noexcept { return slab_size_; }
};

/// Three-tier size-classed slab allocator per RivetWorker process:
///   - Small    (256 B)   : Audio frames, RTCP reports, standalone SPS/PPS/VPS
///   - Standard (2,048 B) : Standard MTU RTP video packets
///   - Jumbo    (65,536 B): Oversized TCP interleaved `$` frames up to the 65,535 B RFC limit
class MultiClassArena {
public:
  static constexpr size_t SMALL_SLAB_BYTES    = 256;
  static constexpr size_t STANDARD_SLAB_BYTES = NumaArena::DEFAULT_SLAB_SIZE;
  static constexpr size_t JUMBO_SLAB_BYTES    = 65536;

private:
  NumaArena small_;
  NumaArena standard_;
  NumaArena jumbo_;

public:
  explicit MultiClassArena(size_t standard_capacity  = NumaArena::DEFAULT_CAPACITY,
      size_t                      standard_slab_size = STANDARD_SLAB_BYTES,
      size_t                      small_capacity     = 2048,
      size_t                      jumbo_capacity     = 32,
      bool                        advise_hugepages   = false)
      : small_(small_capacity, SMALL_SLAB_BYTES, false)
      , standard_(standard_capacity, standard_slab_size, advise_hugepages)
      , jumbo_(jumbo_capacity, JUMBO_SLAB_BYTES, false) {}

  /// Convenience 4-arg constructor: `(standard_capacity, jumbo_capacity, small_capacity,
  /// advise_hugepages)`
  MultiClassArena(size_t standard_capacity,
      size_t             jumbo_capacity,
      size_t             small_capacity,
      bool               advise_hugepages)
      : MultiClassArena(standard_capacity,
            STANDARD_SLAB_BYTES,
            small_capacity,
            jumbo_capacity,
            advise_hugepages) {}

  void bind_owner_thread() noexcept {
    small_.bind_owner_thread();
    standard_.bind_owner_thread();
    jumbo_.bind_owner_thread();
  }

  [[nodiscard]] PooledBuffer *allocate_for_size(size_t required_bytes,
      uint32_t                                         initial_refs = 1) noexcept {
    if (required_bytes <= small_.get_slab_size()) {
      if (auto *b = small_.allocate(initial_refs)) return b;
    }
    if (required_bytes <= standard_.get_slab_size()) {
      if (auto *b = standard_.allocate(initial_refs)) return b;
    }
    if (required_bytes <= jumbo_.get_slab_size()) {
      return jumbo_.allocate(initial_refs);
    }
    return nullptr;
  }

  [[nodiscard]] NumaArena       &small_tier() noexcept { return small_; }
  [[nodiscard]] const NumaArena &small_tier() const noexcept { return small_; }
  [[nodiscard]] NumaArena       &standard_tier() noexcept { return standard_; }
  [[nodiscard]] const NumaArena &standard_tier() const noexcept { return standard_; }
  [[nodiscard]] NumaArena       &jumbo_tier() noexcept { return jumbo_; }
  [[nodiscard]] const NumaArena &jumbo_tier() const noexcept { return jumbo_; }

  [[nodiscard]] size_t total_in_flight() const noexcept {
    return small_.in_flight() + standard_.in_flight() + jumbo_.in_flight();
  }

  [[nodiscard]] size_t in_flight() const noexcept { return total_in_flight(); }

  [[nodiscard]] uint64_t total_exhaustion_count() const noexcept {
    return small_.exhaustion_count() + standard_.exhaustion_count() + jumbo_.exhaustion_count();
  }
};

} //< namespace rivet::memory
