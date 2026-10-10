#pragma once

#include <cstddef>
#include <cstdint>

namespace rivet::media {

/// The classification of an HEVC (H.265) NAL Unit
enum class HevcNalType : uint8_t {
  /// Regular video frames (P-frames, B-frames, normal I-frames)
  Regular,

  /// Instantaneous Decoding Refresh. Completely resets the decoder. Safe to start playback here
  IDR,

  /// Clean Random Access. Safe to start playback, but requires dropping subsequent RASL frames
  CRA,

  /// Random Access Skipped Leading picture. Must be dropped if we just joined the CRA
  RASL,

  /// Random Access Decodable Leading picture. Safe to decode if we just joined a CRA
  RADL,

  /// Parameter Sets (VPS, SPS, PPS). Critical metadata for decoder initialization
  ParameterSet
};

/// A fast, zero-copy classifier for HEVC (H.265) video streams.
/// NOTE: Only parse the first 2 bytes of the NAL header to extract the NAL Unit type.
class HevcRapClassifier { //< H.265 Random Access Point Classifier
public:
  HevcRapClassifier() noexcept = default;

  /// Extracts the NAL type from the raw network payload
  /// NOTE: HEVC NAL headers are always exactly 2 bytes long.
  ///\param payload A pointer to the raw H.265 payload inside the UDP packet
  ///\param length The total length of the payload
  ///\return HevcNalType
  [[nodiscard]] static HevcNalType classify(const uint8_t *payload, size_t length) noexcept {
    if (length < 2) {
      return HevcNalType::Regular; //< Too small to be valid
    }

    /// The NAL Unit type is bit 1 through 6 of the first byte:
    /// F(1) | NALType(6) | LayerId(6) | TID(3)
    uint8_t nal_type = (payload[0] >> 1) & 0x3F;

    switch (nal_type) {
      case 8: //< RASL_NAL_UT
        return HevcNalType::RASL;

      case 9:
        return HevcNalType::RADL;

      case 19: //< IDR_W_RADL
      case 20: //< IDR_N_LP
        return HevcNalType::IDR;

      case 21: //< CRA_NAL_UT
        return HevcNalType::CRA;

      case 32: //< VPS_NAL_UT
      case 33: //< SPS_NAL_UT
      case 34: //< PPS_NAL_UT
        return HevcNalType::ParameterSet;

      default:
        return HevcNalType::Regular;
    }
  }
};

} //< namespace rivet::media