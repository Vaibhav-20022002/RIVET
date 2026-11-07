#pragma once

#include "AccessUnitView.hpp"
#include "HevcRapClassifier.hpp"
#include "rivet/core/Zones.hpp"
#include "rivet/memory/PacketFlags.hpp"
#include <cstdint>
#include <optional>
#include <utility>

namespace rivet::media {

/// Reassembles RTP packets into complete Video Access Units (opt-in / lazy).
/// Aborts partial AUs on timestamp jumps without marker OR intra-AU sequence gaps,
/// and computes AU priority as the maximum importance across all packets in the AU.
class AuAssembler {
  AccessUnitView current_au;
  bool           is_building{false};
  bool           poisoned_au{false};
  uint32_t       current_timestamp{0};
  uint16_t       last_sequence{0};

  [[nodiscard]] static DependencyPriority classify_packet_priority(
      const memory::PacketRef &packet) noexcept {
    const uint8_t f          = packet.flags;
    const uint8_t media_bits = static_cast<uint8_t>(
        f & static_cast<uint8_t>(~(memory::pkt::kMarker | memory::pkt::kAuStart)));

    if ((f & (memory::pkt::kIrapMask | memory::pkt::kParamSet)) != 0) {
      return DependencyPriority::Critical;
    }
    if ((f & memory::pkt::kRasl) != 0) {
      return DependencyPriority::Disposable;
    }
    if ((f & memory::pkt::kRef) != 0) {
      return DependencyPriority::Reference;
    }
    if (media_bits != 0) {
      return DependencyPriority::Disposable;
    }

    /// Fallback for unclassified packets: inspect RTP payload (never the 12-byte RTP header).
    auto span = packet.rtp_payload();
    if (span.empty()) {
      auto raw = packet.payload();
      if (raw.empty()) {
        return DependencyPriority::Reference;
      }
      const auto *bytes = reinterpret_cast<const uint8_t *>(raw.data());
      HevcNalType t     = HevcRapClassifier::classify(bytes, raw.size());
      if (t == HevcNalType::IDR || t == HevcNalType::CRA || t == HevcNalType::ParameterSet) {
        return DependencyPriority::Critical;
      }
      if (t == HevcNalType::RASL) {
        return DependencyPriority::Disposable;
      }
      return DependencyPriority::Reference;
    }

    const auto *bytes = reinterpret_cast<const uint8_t *>(span.data());
    HevcNalType type  = HevcRapClassifier::classify(bytes, span.size());
    if (type == HevcNalType::IDR || type == HevcNalType::CRA || type == HevcNalType::ParameterSet) {
      return DependencyPriority::Critical;
    }
    if (type == HevcNalType::RASL) {
      return DependencyPriority::Disposable;
    }
    return DependencyPriority::Reference;
  }

public:
  AuAssembler() noexcept = default;

  void reset() noexcept {
    current_au.clear();
    is_building       = false;
    poisoned_au       = false;
    current_timestamp = 0;
    last_sequence     = 0;
  }

  /// Process an incoming RTP packet.
  /// \return Completed AccessUnitView when marker_bit closes an intact AU, std::nullopt otherwise.
  [[nodiscard]] std::optional<AccessUnitView> on_packet(memory::PacketRef packet, bool marker_bit) {
    RIVET_ZONE("Media::AuAssembler");
    const uint32_t pkt_timestamp = packet.rtp_timestamp;

    /// 1. Did the timestamp change?
    if ((is_building || poisoned_au) && pkt_timestamp != current_timestamp) {
      current_au.clear();
      is_building = false;
      poisoned_au = false;
    }

    /// If the current timestamp's AU already experienced a sequence gap, drop remaining fragments
    if (poisoned_au) {
      if (marker_bit) {
        poisoned_au = false;
      }
      return std::nullopt;
    }

    /// 2. Check intra-AU sequence continuity
    if (is_building) {
      const uint16_t expected_seq = static_cast<uint16_t>(last_sequence + 1u);
      if (packet.sequence != expected_seq) {
        current_au.clear();
        is_building = false;
        if (!marker_bit) {
          poisoned_au = true;
        }
        return std::nullopt;
      }
    }

    const DependencyPriority pkt_prio = classify_packet_priority(packet);

    /// 3. Start a new AU or elevate the existing AU's priority (max across all packets in AU)
    if (!is_building) {
      is_building       = true;
      current_timestamp = pkt_timestamp;
      current_au.begin(packet.sequence, current_timestamp, pkt_prio);
    } else {
      current_au.elevate_priority(pkt_prio);
    }

    last_sequence = packet.sequence;
    current_au.append(std::move(packet));

    /// 4. Complete AU on RTP Marker bit
    if (marker_bit) {
      is_building        = false;
      AccessUnitView out = std::move(current_au);
      current_au         = AccessUnitView{};
      return out;
    }

    return std::nullopt;
  }
};

} //< namespace rivet::media
