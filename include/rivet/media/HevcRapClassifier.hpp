#pragma once

#include "HevcPayload.hpp"
#include <cstddef>
#include <cstdint>

namespace rivet::media {

/// The classification of an HEVC (H.265) NAL Unit
enum class HevcNalType : uint8_t { Regular, IDR, CRA, RASL, RADL, ParameterSet };

/// Thin compatibility wrapper over `hevc::kLut`.
/// Classifies NAL type 9 (`RASL_R`) as `RASL`, `RADL` (6, 7), and `BLA` (16..18).
/// Full RFC 7798 AP / FU / PACI / DON classification lives in `media::hevc::classify`.
class HevcRapClassifier {
public:
  HevcRapClassifier() noexcept = default;

  [[nodiscard]] static HevcNalType classify(const uint8_t *payload, size_t length) noexcept {
    if (!payload || length < 2U) {
      return HevcNalType::Regular;
    }

    const uint8_t       nal_type = static_cast<uint8_t>((payload[0] >> 1U) & 0x3FU);
    const hevc::NalInfo info     = hevc::kLut[nal_type];
    using namespace memory::pkt;

    if ((info.flags & kClosedIrap) != 0) return HevcNalType::IDR;
    if ((info.flags & kOpenIrap) != 0) return HevcNalType::CRA;
    if ((info.flags & kRasl) != 0) return HevcNalType::RASL;
    if ((info.flags & kRadl) != 0) return HevcNalType::RADL;
    if ((info.flags & kParamSet) != 0) return HevcNalType::ParameterSet;
    return HevcNalType::Regular;
  }
};

} //< namespace rivet::media
