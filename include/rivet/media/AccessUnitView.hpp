#pragma once

#include "../memory/PacketRef.hpp"
#include <cstdint>
#include <utility>
#include <vector>

namespace rivet::media {

/// Describes the importance of this Access Unit for decoding
enum class DependencyPriority : uint8_t {
  /// Cannot be dropped lightly (IDR / CRA / BLA / VPS / SPS / PPS)
  Critical = 0,

  /// Reference frame (P-frames / reference B-frames / RADL)
  Reference = 1,

  /// Non-reference / trailing disposable frame (non-ref B-frames / RASL / sub-layer non-ref)
  Disposable = 2
};

[[nodiscard]] constexpr uint8_t priority_importance(DependencyPriority p) noexcept {
  switch (p) {
    case DependencyPriority::Critical:
      return 2;
    case DependencyPriority::Reference:
      return 1;
    case DependencyPriority::Disposable:
      return 0;
  }
  return 0;
}

[[nodiscard]] constexpr DependencyPriority max_priority(DependencyPriority a,
    DependencyPriority                                                     b) noexcept {
  return (priority_importance(a) >= priority_importance(b)) ? a : b;
}

/// An AccessUnitView represents one complete, decodable video frame (Access Unit).
/// Holds zero-copy reference-counted handles (`PacketRef`) to the underlying arena slabs.
class AccessUnitView {
  uint16_t                       start_sequence{0};
  uint32_t                       rtp_timestamp{0};
  DependencyPriority             priority{DependencyPriority::Reference};
  std::vector<memory::PacketRef> packets;

public:
  AccessUnitView() = default;

  /// Starts building a new Access Unit
  void begin(uint16_t seq, uint32_t timestamp, DependencyPriority prio) {
    start_sequence = seq;
    rtp_timestamp  = timestamp;
    priority       = prio;
    packets.clear();
  }

  /// Elevates AU importance when a higher-priority NAL (e.g. IDR slice after AUD/SEI) arrives
  void elevate_priority(DependencyPriority candidate) noexcept {
    priority = max_priority(priority, candidate);
  }

  /// Appends a packet to this Access Unit
  void append(memory::PacketRef packet) { packets.push_back(std::move(packet)); }

  void clear() noexcept { packets.clear(); }

  [[nodiscard]] bool is_empty() const noexcept { return packets.empty(); }

  [[nodiscard]] size_t packet_count() const noexcept { return packets.size(); }

  [[nodiscard]] uint16_t get_start_sequence() const noexcept { return start_sequence; }

  [[nodiscard]] uint32_t get_rtp_timestamp() const noexcept { return rtp_timestamp; }

  [[nodiscard]] DependencyPriority get_priority() const noexcept { return priority; }

  [[nodiscard]] const std::vector<memory::PacketRef> &get_packets() const noexcept {
    return packets;
  }
};

} //< namespace rivet::media
