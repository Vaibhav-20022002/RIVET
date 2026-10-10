#pragma once

#include "AccessUnitView.hpp"
#include "HevcRapClassifier.hpp"
#include <optional>

namespace rivet::media {

/// Reassembles raw UDP/RTP packets back into complete Video Frames (Access Units).
/// If a packet is lost in transit, it forces the entire Access Unit to be dropped, protecting the
/// client's decoder from trying to render corrupted garbage.
class AuAssembler {
  AccessUnitView current_au;
  bool           is_building{false};

  /// The timestamp of the current AU. When this changes, it means the camera has finished the old
  /// frame and started a new one
  uint32_t current_timestamp{0};

public:
  AuAssembler() noexcept = default;

  /// Process an incoming RTP packet
  /// \param packet The raw network packet containing a piece of video
  /// \param marker_bit True if the camera says this is the LAST packet of the frame
  /// \return A completed AccessUnitView if the frame is fully assembled, `std::nullopt` otherwise
  [[nodiscard]] std::optional<AccessUnitView> on_packet(memory::PacketRef packet, bool marker_bit) {
    uint32_t pkt_timestamp = packet.rtp_timestamp;

    /// 1. Did the timestamp change? (New Frame started)
    if (is_building && pkt_timestamp != current_timestamp) {
      /// The camera started sending a new frame before finishing the old one!
      /// This happens if the internet dropped the `marker_bit` packet of the previous frame
      /// We must immediately abort and drop the old, corrupted frame
      is_building = false;
      current_au  = AccessUnitView(); //< Reset
    }

    /// 2. Are we starting a new frame ?
    if (!is_building) {
      is_building       = true;
      current_timestamp = pkt_timestamp;

      /// We peek at the first 2 bytes of the payload to classify the frame's importance
      HevcNalType type = HevcRapClassifier::classify(
          reinterpret_cast<const uint8_t *>(packet.payload().data()), packet.length);

      DependencyPriority prio = DependencyPriority::Reference;
      if (type == HevcNalType::IDR || type == HevcNalType::CRA ||
          type == HevcNalType::ParameterSet) {
        prio = DependencyPriority::Critical;
      } else if (type == HevcNalType::RASL) {
        prio = DependencyPriority::Disposable;
      }

      current_au.begin(packet.sequence, current_timestamp, prio);
    }

    /// 3. Add the packet to the current frame
    current_au.append(std::move(packet));

    /// 4. Is the frame finished ?
    if (marker_bit) {
      is_building = false;
      return std::move(current_au);
    }

    /// Keep waiting for more packets
    return std::nullopt;
  }
};

} //< namespace rivet::media