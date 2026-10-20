#pragma once

#include "PooledBuffer.hpp"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

namespace rivet::memory {

/// Standard cacheline alignment
inline constexpr size_t CACHELINE_ALIGN = 64;

/// NumaArena is a NUMA-aware, zero-fragmentation fixed-size slab allocator.
/// Pre-allocates a contiguous pool of 64-byte aligned slabs for network packet payloads,
/// eliminating heap allocation latency on the hot data path.
class NumaArena {
public:
  static constexpr size_t DEFAULT_SLAB_SIZE =
      2048; //< Accommodates standard 1500 MTU packets + headers
  static constexpr size_t DEFAULT_CAPACITY = 4096; //< 4096 * 2KB = 8 MB default slab pool per arena

private:
  size_t slab_size{DEFAULT_SLAB_SIZE};
  size_t capacity{DEFAULT_CAPACITY};

  /// Contiguous raw memory block aligned to 64 bytes
  uint8_t *raw_memory{nullptr};

  /// Array of PooledBuffer descriptors
  std::unique_ptr<PooledBuffer[]> descriptors;

  /// Free list of available PooledBuffer pointers
  std::vector<PooledBuffer *> free_list;

  /// Fast spinlock/mutex protecting free list access across threads
  mutable std::mutex arena_mutex;

  /// Total allocated count for telemetry
  std::atomic<size_t> active_allocations{0};

public:
  explicit NumaArena(size_t slab_capacity        = DEFAULT_CAPACITY,
      size_t                individual_slab_size = DEFAULT_SLAB_SIZE)
      : slab_size(individual_slab_size)
      , capacity(slab_capacity) {
    /// Ensure slab size is a multiple of cacheline size for 64-byte alignment
    if (slab_size % CACHELINE_ALIGN != 0) {
      slab_size = ((slab_size + CACHELINE_ALIGN - 1) / CACHELINE_ALIGN) * CACHELINE_ALIGN;
    }

    const size_t total_bytes = capacity * slab_size;

    /// Allocate 64-byte cacheline aligned contiguous memory
#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L || defined(__APPLE__)
    if (posix_memalign(reinterpret_cast<void **>(&raw_memory), CACHELINE_ALIGN, total_bytes) != 0) {
      raw_memory = nullptr;
    }
#else
    raw_memory = static_cast<uint8_t *>(std::aligned_alloc(CACHELINE_ALIGN, total_bytes));
#endif

    descriptors = std::make_unique<PooledBuffer[]>(capacity);
    free_list.reserve(capacity);

    if (raw_memory) {
      for (size_t i = 0; i < capacity; ++i) {
        descriptors[i].capacity     = static_cast<uint32_t>(slab_size);
        descriptors[i].arena        = this;
        descriptors[i].payload_data = reinterpret_cast<std::byte *>(raw_memory + (i * slab_size));
        descriptors[i].ref_count.store(0, std::memory_order_relaxed);
        descriptors[i].recycle_fn = [](PooledBuffer *b) noexcept {
          if (b && b->arena) {
            b->arena->free_buffer(b);
          }
        };
        free_list.push_back(&descriptors[i]);
      }
    }
  }

  ~NumaArena() {
    std::lock_guard<std::mutex> lock(arena_mutex);
    if (descriptors) {
      for (size_t i = 0; i < capacity; ++i) {
        descriptors[i].arena = nullptr;
      }
    }
    if (raw_memory) {
      std::free(raw_memory);
      raw_memory = nullptr;
    }
  }

  /// Non-copyable, non-movable to guarantee descriptor pointer stability
  NumaArena(const NumaArena &)            = delete;
  NumaArena &operator=(const NumaArena &) = delete;
  NumaArena(NumaArena &&)                 = delete;
  NumaArena &operator=(NumaArena &&)      = delete;

  /// Allocates a PooledBuffer from the arena with initial ref_count = 1
  ///\return Pointer to the PooledBuffer, or nullptr if arena is exhausted
  [[nodiscard]] PooledBuffer *allocate() noexcept {
    std::lock_guard<std::mutex> lock(arena_mutex);
    if (free_list.empty()) {
      return nullptr;
    }

    PooledBuffer *buf = free_list.back();
    free_list.pop_back();

    buf->ref_count.store(1, std::memory_order_relaxed);
    active_allocations.fetch_add(1, std::memory_order_relaxed);
    return buf;
  }

  /// Returns a PooledBuffer back to the arena's free list
  /// Called automatically by PooledBuffer::release() when ref_count reaches 0
  void free_buffer(PooledBuffer *buf) noexcept {
    if (!buf || buf->arena != this) {
      return;
    }

    std::lock_guard<std::mutex> lock(arena_mutex);
    free_list.push_back(buf);
    active_allocations.fetch_sub(1, std::memory_order_relaxed);
  }

  /// Number of currently available buffers
  [[nodiscard]] size_t available() const noexcept {
    std::lock_guard<std::mutex> lock(arena_mutex);
    return free_list.size();
  }

  /// Total capacity of the arena in slabs
  [[nodiscard]] size_t total_capacity() const noexcept { return capacity; }

  /// Number of buffers currently in flight
  [[nodiscard]] size_t in_flight() const noexcept {
    return active_allocations.load(std::memory_order_relaxed);
  }

  /// Size in bytes of each individual slab
  [[nodiscard]] size_t get_slab_size() const noexcept { return slab_size; }
};

} //< namespace rivet::memory
