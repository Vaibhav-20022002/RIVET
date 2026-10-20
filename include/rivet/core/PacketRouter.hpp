#pragma once

#include "rivet/memory/PacketRef.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace rivet::core {

/// Standard L1/L2 data cacheline boundary for modern x86_64 and aarch64 architectures
///\todo Set this constant for every build by detecting the cacheline size and architecture
inline constexpr size_t CACHE_LINE_SIZE = 64;

/// A wait-free, lock-free Single-Producer Single-Consumer (SPSC) Ring Buffer.
/// Guarantees zero system calls and zero mutex contention between Ingest and Egress threads
template<typename T, size_t Capacity = 1024> class alignas(CACHE_LINE_SIZE) SpscQueue {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be an exact power of two");

  /// Storage array sized to hold pre-retained PacketRef handles
  alignas(CACHE_LINE_SIZE) std::array<T, Capacity> ring{};

  /// Head Pointer: Modified exclusively by the Producer (Ingest Thread)
  alignas(CACHE_LINE_SIZE) std::atomic<size_t> head{0};

  /// Tail Pointer: Modified exclusively by the Consumer (Egress Thread)
  alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail{0};

public:
  SpscQueue() noexcept = default;
  ~SpscQueue()         = default;

  /// Non-copyable, non-movable to ensure stable cacheline memory placement
  SpscQueue(const SpscQueue &)            = delete;
  SpscQueue &operator=(const SpscQueue &) = delete;
  SpscQueue(SpscQueue &&)                 = delete;
  SpscQueue &operator=(SpscQueue &&)      = delete;

  /// Enqueues an item into the ring buffer. Called exclusively by Producer.
  ///\param item The item to enqueue (copying PacketRef increments buffer ref_count)
  ///\return True if queued, false if queue is saturated (Zone 4 backpressure)
  template<typename P> bool try_push(P &&item) noexcept {
    const size_t current_head = head.load(std::memory_order_relaxed);
    const size_t current_tail = tail.load(std::memory_order_acquire);

    if ((current_head - current_tail) >= Capacity) {
      return false; //< Queue full: subscriber lag or kernel backpressure
    }

    ring[current_head & (Capacity - 1)] = std::forward<P>(item);
    head.store(current_head + 1, std::memory_order_release);
    return true;
  }

  /// Dequeues an item from the ring buffer. Called exclusively by Consumer.
  ///\param out_item Reference to receive the popped handle via move semantics
  ///\return True if item retrieved, false if queue is empty
  bool try_pop(T &out_item) noexcept {
    const size_t current_tail = tail.load(std::memory_order_relaxed);
    const size_t current_head = head.load(std::memory_order_acquire);

    if (current_tail == current_head) {
      return false; //< Queue empty
    }

    out_item = std::move(ring[current_tail & (Capacity - 1)]);
    tail.store(current_tail + 1, std::memory_order_release);
    return true;
  }

  /// Approximate occupancy count
  [[nodiscard]] size_t size() const noexcept {
    const size_t current_head = head.load(std::memory_order_relaxed);
    const size_t current_tail = tail.load(std::memory_order_relaxed);
    return (current_head >= current_tail) ? (current_head - current_tail) : 0;
  }

  [[nodiscard]] bool empty() const noexcept {
    return head.load(std::memory_order_relaxed) == tail.load(std::memory_order_relaxed);
  }

  [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity; }
};

/// PacketRouter manages the fan-out from 1 Ingest Reactor to N Egress Reactors.
/// It dispatches pre-retained PacketRef batches across dedicated SPSC channels.
class PacketRouter {
public:
  static constexpr size_t DEFAULT_CHANNEL_CAPACITY = 1024;
  using PacketQueue = SpscQueue<memory::PacketRef, DEFAULT_CHANNEL_CAPACITY>;

private:
  /// Dedicated point-to-point SPSC channel for each Egress Reactor shard
  std::vector<std::unique_ptr<PacketQueue>> egress_channels;

public:
  PacketRouter() = default;

  /// Registers a new Egress Reactor shard and returns its assigned channel ID
  size_t register_egress_channel() {
    egress_channels.push_back(std::make_unique<PacketQueue>());
    return egress_channels.size() - 1;
  }

  /// Broadcasts a published batch of PacketRefs to all registered Egress Reactors.
  /// Copying PacketRef into the SPSC slot safely increments the intrusive ref_count.
  ///\param batch Array of packets emitted by IngestReactor::publish_batch()
  ///\return Total number of successful packet deliveries across all channels
  size_t broadcast(const std::vector<memory::PacketRef> &batch) {
    size_t total_dispatched = 0;

    for (const auto &packet : batch) {
      for (auto &channel : egress_channels) {
        /// Copy constructor calls buffer->retain()
        if (channel->try_push(packet)) {
          ++total_dispatched;
        } else {
          /// Zone 4 backpressure: Egress queue overflow
          /// The lagging reactor drops the frame without stalling Ingest
        }
      }
    }

    return total_dispatched;
  }

  /// Retrieves consumer queue handle for a specific Egress Reactor shard
  [[nodiscard]] PacketQueue *get_channel(size_t channel_id) noexcept {
    if (channel_id < egress_channels.size()) {
      return egress_channels[channel_id].get();
    }
    return nullptr;
  }

  [[nodiscard]] size_t channel_count() const noexcept { return egress_channels.size(); }
};

} //< namespace rivet::core
