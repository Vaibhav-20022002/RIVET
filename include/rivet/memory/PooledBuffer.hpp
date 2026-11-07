#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace rivet::memory {

/// Forward declare the arena that owns the slabs
class NumaArena;

/// A fixed-size chunk of memory with an intrusive atomic reference count.
/// Allocated once at startup; never destroyed, only recycled.
class PooledBuffer {
public:
  /// Align to 64 bytes (cache-line-size) to prevent false sharing when different CPU cores modify
  /// the ref_count of adjacent buffers.
  alignas(64) std::atomic<uint32_t> ref_count{0};

  uint32_t   capacity{0};
  uint32_t   pool_index{0}; //< Stable descriptor index inside owning NumaArena
  NumaArena *arena{nullptr};

  /// Pointer to the actual media payload storage
  std::byte *payload_data{nullptr};

  using RecycleFn = void (*)(PooledBuffer *) noexcept;
  RecycleFn recycle_fn{nullptr};

  /// Batch-initializes reference count on fresh allocation (e.g., 1 + num_shards).
  void init_refs(uint32_t count) noexcept {
    assert(count > 0);
    ref_count.store(count, std::memory_order_relaxed);
  }

  /// Increments the reference count (called when a new handle retains this packet)
  void retain() noexcept {
    /// memory_order_relaxed is safe here because we already hold a valid reference.
    ref_count.fetch_add(1, std::memory_order_relaxed);
  }

  /// Decrements the reference count by `count`. If it hits zero, returns the buffer to the arena.
  void release_n(uint32_t count = 1) noexcept {
    assert(count > 0);
    if (ref_count.fetch_sub(count, std::memory_order_acq_rel) == count) {
      recycle();
    }
  }

  /// Decrements the reference count by 1. If it hits zero, returns the buffer to the arena.
  void release() noexcept { release_n(1); }

  [[nodiscard]] std::byte *data() const noexcept { return payload_data; }
  [[nodiscard]] uint8_t   *u8_data() const noexcept {
    return reinterpret_cast<uint8_t *>(payload_data);
  }

  [[nodiscard]] uint32_t size() const noexcept { return capacity; }

private:
  /// Returns this buffer back to the NumaArena's free list.
  inline void recycle() noexcept {
    if (recycle_fn) {
      recycle_fn(this);
    }
  }
};

static_assert(sizeof(PooledBuffer) == 64, "PooledBuffer must fit in a single 64-byte cache line");

} //< namespace rivet::memory
