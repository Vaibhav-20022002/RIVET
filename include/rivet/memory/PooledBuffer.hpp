#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rivet::memory {

/// Forward declare the arena that owns the slabs
class NumaArena;

/// A fixed-size chunk of memory with an intrusive atomic reference count.
/// Allocated once at startup; never destroyed, only recycled.
class PooledBuffer {
public:
  /// We align to 64 bytes (cache-line-size) to prevent false sharing.
  /// When different CPU cores modify the ref_count of adjacent buffers.
  alignas(64) std::atomic<uint32_t> ref_count{0};

  uint32_t   capacity{0};
  NumaArena *arena{nullptr};

  /// Pointer to the actual media payload storage
  std::byte *payload_data{nullptr};

  /// Increments the reference count (called when a new subscriber needs this packet)
  void retain() noexcept {
    /// memory_order_relaxed is safe here because we already hold a valid reference, so we aren't
    /// synchronizing with a concurrent destruction.
    ref_count.fetch_add(1, std::memory_order_relaxed);
  }

  /// Decrements the reference count. If it hits zero, returns the buffer to the arena.
  void release() noexcept {
    /// memory_order_acq_rel ensures all prior reads/writes to the payload data by this thread are
    /// visible before the memory is recycled by another thread.
    if (ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      recycle();
    }
  }

  [[nodiscard]] std::byte *data() const noexcept { return payload_data; }

  [[nodiscard]] uint32_t size() const noexcept { return capacity; }

  using RecycleFn = void (*)(PooledBuffer *) noexcept;
  RecycleFn recycle_fn{nullptr};

private:
  /// Returns this buffer back to the NumaArena's free list.
  inline void recycle() noexcept {
    if (recycle_fn) {
      recycle_fn(this);
    }
  }
};

} //< namespace rivet::memory
