#pragma once

#include "../memory/PacketRef.hpp"
#include <cstdint>
#include <vector>

namespace rivet::media {

/// Describes the importance of this Access Unit for decoding
enum class DependencyPriority : uint8_t {
  /// Cannot be dropped (IDR / CRA / SPS / PPS)
  Critical,

  /// Can be dropped if the network is highly congested (P-frames)
  Reference,

  /// Can be safely dropped at any time without corrupting the stream (B-frames / Non-reference)
  Disposable
};

/// An AccessUnitView represents one complete, decodable video frame.
/// NOTE: Because out network stack is Zero-Copy, this object does NOT contain actual video bytes!
///       Instead, it holds a lightweight list of reference-counted pointers(`PacketRef`) to the
///       original kernel memory buffers where the UDP packets reside.
class AccessUnitView {
  /// The RTP sequence number of the FIRST packet in this Access Unit
  uint16_t start_sequence{0};

  /// The RTP timestamp of the video frame(all packets in the AU share the exact timestamp)
  uint32_t rtp_timestamp{0};

  /// The importance of this frame
  DependencyPriority priority{DependencyPriority::Reference};

  /// The ordered list of network packets that make up this frame.
  /// Because PacketRef manages the atomic reference count, the MemoryPool will keep these kernels
  /// buffers alive for exactly as long as this AccessUnit exists!
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

  /// Appends a packet to this Access Unit
  /// We take the PacketRef by value and std::move it into the vector, transferring ownership of the
  /// atomic reference count safely.
  void append(memory::PacketRef packet) { packets.push_back(std::move(packet)); }

  [[nodiscard]] bool is_empty() const noexcept { return packets.empty(); }

  [[nodiscard]] size_t packet_count() const noexcept { return packets.size(); }

  [[nodiscard]] uint16_t get_start_sequence() const noexcept { return start_sequence; }

  [[nodiscard]] uint32_t get_rtp_timestamp() const noexcept { return rtp_timestamp; }

  [[nodiscard]] DependencyPriority get_priority() const noexcept { return priority; }

  /// Exposes the underlying packet references for the EgressReactor to send
  [[nodiscard]] const std::vector<memory::PacketRef> &get_packets() const noexcept {
    return packets;
  }
};

} //< namespace rivet::media
