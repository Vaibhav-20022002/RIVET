#pragma once

#include "../memory/PacketRef.hpp"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <mutex>
#include <optional>
#include <vector>

namespace rivet::media {

/// The RetainedPacketLog keeps an ordered rolling window of most recent network packets.
/// When a new viewer joins the stream, they need an IDR (keyframe) to start decoding.
/// The log allows RIVET to instantly reach back in time, grab the last IDR, and replay the stream
/// to the new viewer, allowing them to catch up to the live edge seamlessly.
class RetainedPacketLog {
  /// The physical capacity limit of the log.
  /// 2000 packets is roughly 2-3 seconds of 4K@60fps video
  /// PacketRef is just a 32-byte handle, 2000 items is less than 64KB of RAM!
  static constexpr size_t MAX_RETAINED_PACKETS = 2000;

  /// Use a deque because we need fast O(1) push back and O(1) pop front.
  ///\todo Use polymorphic one...
  std::deque<memory::PacketRef> log;

  /// To allow multiple subscribers to read the log safely while the Ingest Reactor writes to it.
  ///\todo Make it lock-free...
  mutable std::mutex log_mutex;

public:
  RetainedPacketLog() = default;

  /// Appends a new packet from the camera to the live edge of the log.
  /// If the log exceeds capacity, the oldest packet is evicted and its underlying kernel memory
  /// buffer is released back to the MemoryPool automatically.
  void append(memory::PacketRef packet) {
    std::lock_guard<std::mutex> lock(log_mutex);

    log.push_back(std::move(packet));

    if (log.size() > MAX_RETAINED_PACKETS) {
      log.pop_front();
    }
  }

  /// Searches the log backwards to find the Sequence Number of the most recent Random Access Point
  /// (IDR or CRA). New subscribers MUST start here!
  ///\return The sequence number of the RAP, or std::nullopt if no RAP exists yet.
  [[nodiscard]] std::optional<uint16_t> find_latest_rap_sequence() const {
    std::lock_guard<std::mutex> lock(log_mutex);

    /// Search backwards (rbegin to rend) so we find the latest keyframe
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
      if (it->is_idr() || it->is_cra()) {
        return it->sequence;
      }
    }

    return std::nullopt;
  }

  /// Extracts a snapshot (copy) of all packets from a specific sequence number up to the current
  /// live edge. This is used to BURST old packets to a new subscriber.
  ///\param start_sequence The sequence number the subscriber wants to start at
  ///\return An ordered vector of safely reference-counted packets
  [[nodiscard]] std::vector<memory::PacketRef> snapshot_from(uint16_t start_sequence) const {
    std::lock_guard<std::mutex> lock(log_mutex);

    std::vector<memory::PacketRef> burst;
    burst.reserve(500); //< Pre-allocate to avoid reallocations

    bool found = false;
    for (const auto &pkt : log) {
      if (!found) {
        if (pkt.sequence == start_sequence) {
          found = true;
        }
      }

      if (found) {
        /// Because we are iterating over `const PacketRef&` and placing them into a vector, the
        /// PacketRef copy constructor is called. This safely increments the atomic reference count
        /// of the underlying kernel memory buffer!
        burst.push_back(pkt);
        if (burst.size() >= 128) {
          break; //< Bound snapshot depth to prevent kernel buffer overflows
        }
      }
    }

    return burst;
  }

  /// Clears all retained packets from the log, releasing underlying buffers immediately
  void clear() noexcept {
    std::lock_guard<std::mutex> lock(log_mutex);
    log.clear();
  }

  /// Returns the current number of packets in the log
  [[nodiscard]] size_t size() const noexcept {
    std::lock_guard<std::mutex> lock(log_mutex);
    return log.size();
  }
};

} //< namespace rivet::media