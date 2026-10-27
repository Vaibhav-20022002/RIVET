#pragma once

#include <cstddef>
#include <cstdint>

namespace rivet::media {

/// The classification of an H.264 (AVC) NAL Unit according to RFC 6184
enum class H264NalType : uint8_t {
  /// Regular video frames (P-frames, B-frames, non-IDR I-frames)
  Regular,

  /// Instantaneous Decoding Refresh (Keyframe). Safe to start decoding
  IDR,

  /// Sequence Parameter Set (SPS)
  SPS,

  /// Picture Parameter Set (PPS)
  PPS,

  /// Single-Time Aggregation Packet type A (STAP-A) containing multiple NALUs
  STAP_A,

  /// Fragmentation Unit type A (FU-A)
  FU_A
};

/// High-performance, zero-copy classifier and parameter set extractor for H.264 video streams.
class H264RapClassifier {
public:
  H264RapClassifier() noexcept = default;

  /// Extracts the H.264 NAL type from the raw RTP payload
  ///\param payload Pointer to raw RTP video payload
  ///\param length Total length of payload in bytes
  ///\return H264NalType
  [[nodiscard]] static H264NalType classify(const uint8_t *payload, size_t length) noexcept {
    if (!payload || length < 1) {
      return H264NalType::Regular;
    }

    uint8_t nal_type = payload[0] & 0x1F;

    /// FU-A fragmented NAL unit (RFC 6184 Section 5.8)
    if (nal_type == 28) {
      if (length < 2) {
        return H264NalType::FU_A;
      }
      uint8_t fu_type  = payload[1] & 0x1F;
      bool    fu_start = (payload[1] & 0x80) != 0;
      if (fu_type == 5) {
        return fu_start ? H264NalType::IDR : H264NalType::FU_A;
      }
      if (fu_type == 7) {
        return H264NalType::SPS;
      }
      if (fu_type == 8) {
        return H264NalType::PPS;
      }
      return H264NalType::FU_A;
    }

    /// STAP-A aggregation packet (RFC 6184 Section 5.7)
    if (nal_type == 24) {
      return H264NalType::STAP_A;
    }

    switch (nal_type) {
      case 5:
        return H264NalType::IDR;
      case 7:
        return H264NalType::SPS;
      case 8:
        return H264NalType::PPS;
      default:
        return H264NalType::Regular;
    }
  }

  /// Extracts SPS/PPS parameter sets from either single NAL, STAP-A aggregation,
  /// or HEVC parameter set packets, invoking the callback with each found parameter set.
  /// Callback signature: void(H264NalType type, const uint8_t *nal_ptr, size_t nal_len)
  template<typename Callback>
  static void
  for_each_parameter_set(const uint8_t *payload, size_t length, Callback &&cb) noexcept {
    if (!payload || length < 1) {
      return;
    }

    uint8_t nal_type = payload[0] & 0x1F;

    if (nal_type == 7) {
      cb(H264NalType::SPS, payload, length);
      return;
    }
    if (nal_type == 8) {
      cb(H264NalType::PPS, payload, length);
      return;
    }

    /// STAP-A aggregation packet: parse internal NALUs
    if (nal_type == 24) {
      size_t off = 1;
      while (off + 2 <= length) {
        uint16_t nalu_size = (static_cast<uint16_t>(payload[off]) << 8) | payload[off + 1];
        off += 2;
        if (off + nalu_size > length || nalu_size == 0) {
          break;
        }
        uint8_t sub_type = payload[off] & 0x1F;
        if (sub_type == 7) {
          cb(H264NalType::SPS, payload + off, nalu_size);
        } else if (sub_type == 8) {
          cb(H264NalType::PPS, payload + off, nalu_size);
        }
        off += nalu_size;
      }
      return;
    }

    /// Fallback for HEVC parameter sets if HEVC stream is fed
    if (length >= 2) {
      uint8_t hevc_type = (payload[0] >> 1) & 0x3F;
      if (hevc_type == 33) {
        cb(H264NalType::SPS, payload, length);
      } else if (hevc_type == 34) {
        cb(H264NalType::PPS, payload, length);
      }
    }
  }
};

} //< namespace rivet::media
