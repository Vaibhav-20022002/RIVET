#pragma once

#include "HevcPayload.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>

namespace rivet::media {

/// The classification of an H.264 (AVC) NAL Unit according to RFC 6184
enum class H264NalType : uint8_t { Regular, IDR, SPS, PPS, STAP_A, FU_A };

struct H264SpsInfo {
  uint8_t     profile_idc{0x42};
  uint8_t     constraint_flags{0xE0};
  uint8_t     level_idc{0x1F};
  uint8_t     sps_id{0};
  std::string profile_level_id_hex{"42e01f"};
};

/// Parses H.264 SPS NAL unit (including 1-byte NAL header `0x67`/`0x27`/`0x47`) for
/// `profile-level-id` (3 raw bytes after NAL header per RFC 6184 §8.1) and `seq_parameter_set_id`.
[[nodiscard]] inline std::optional<H264SpsInfo> parse_h264_sps_info(
    std::span<const uint8_t> nal) noexcept {
  if (nal.size() < 4U || (nal[0] & 0x80U) != 0U || (nal[0] & 0x1FU) != 7U) {
    return std::nullopt;
  }
  H264SpsInfo info{};
  info.profile_idc      = nal[1];
  info.constraint_flags = nal[2];
  info.level_idc        = nal[3];

  char hex[7]{};
  std::snprintf(hex, sizeof(hex), "%02x%02x%02x", nal[1], nal[2], nal[3]);
  info.profile_level_id_hex.assign(hex, 6);

  std::array<uint8_t, 64> rbsp{};
  const size_t            n = hevc::strip_emulation_prevention(nal, 4U, rbsp.data(), rbsp.size());
  if (n > 0U) {
    hevc::BitReader br(rbsp.data(), n);
    if (auto id = br.read_ue(); id && *id < 32U) {
      info.sps_id = static_cast<uint8_t>(*id);
    }
  }
  return info;
}

[[nodiscard]] inline std::optional<uint8_t> parse_h264_pps_id(
    std::span<const uint8_t> nal) noexcept {
  if (nal.size() < 2U || (nal[0] & 0x80U) != 0U || (nal[0] & 0x1FU) != 8U) {
    return std::nullopt;
  }
  std::array<uint8_t, 32> rbsp{};
  const size_t            n = hevc::strip_emulation_prevention(nal, 1U, rbsp.data(), rbsp.size());
  if (n == 0U) return std::nullopt;
  hevc::BitReader br(rbsp.data(), n);
  if (auto id = br.read_ue(); id && *id < 64U) {
    return static_cast<uint8_t>(*id);
  }
  return 0U;
}

/// High-performance, zero-copy classifier and parameter set extractor for H.264 video streams.
class H264RapClassifier {
public:
  H264RapClassifier() noexcept = default;

  /// Extracts the H.264 NAL type from the raw RTP payload (past the RTP header!)
  [[nodiscard]] static H264NalType classify(const uint8_t *payload, size_t length) noexcept {
    if (!payload || length < 1U || (payload[0] & 0x80U) != 0U) {
      return H264NalType::Regular;
    }

    const uint8_t nal_type = payload[0] & 0x1FU;

    /// FU-A fragmented NAL unit (RFC 6184 Section 5.8)
    if (nal_type == 28U) {
      if (length < 2U) {
        return H264NalType::FU_A;
      }
      const uint8_t fu_type  = payload[1] & 0x1FU;
      const bool    fu_start = (payload[1] & 0x80U) != 0;
      if (fu_type == 5U) {
        return fu_start ? H264NalType::IDR : H264NalType::FU_A;
      }
      if (fu_type == 7U) {
        return H264NalType::SPS;
      }
      if (fu_type == 8U) {
        return H264NalType::PPS;
      }
      return H264NalType::FU_A;
    }

    /// STAP-A aggregation packet (RFC 6184 Section 5.7)
    if (nal_type == 24U) {
      return H264NalType::STAP_A;
    }

    switch (nal_type) {
      case 5U:
        return H264NalType::IDR;
      case 7U:
        return H264NalType::SPS;
      case 8U:
        return H264NalType::PPS;
      default:
        return H264NalType::Regular;
    }
  }

  /// Extracts H.264 SPS/PPS parameter sets from single NAL or STAP-A RTP payloads.
  /// HEVC parameter-set extraction uses `hevc::for_each_parameter_set`.
  template<typename Callback>
  static void
  for_each_parameter_set(const uint8_t *payload, size_t length, Callback &&cb) noexcept {
    if (!payload || length < 1U || (payload[0] & 0x80U) != 0U) {
      return;
    }

    const uint8_t nal_type = payload[0] & 0x1FU;

    if (nal_type == 7U) {
      cb(H264NalType::SPS, payload, length);
      return;
    }
    if (nal_type == 8U) {
      cb(H264NalType::PPS, payload, length);
      return;
    }

    /// STAP-A aggregation packet: parse internal NALUs
    if (nal_type == 24U) {
      size_t off = 1U;
      while (off + 2U <= length) {
        const uint16_t nalu_size =
            static_cast<uint16_t>((static_cast<uint16_t>(payload[off]) << 8U) | payload[off + 1U]);
        off += 2U;
        if (nalu_size == 0U || off + nalu_size > length) {
          break;
        }
        if ((payload[off] & 0x80U) == 0U) {
          const uint8_t sub_type = payload[off] & 0x1FU;
          if (sub_type == 7U) {
            cb(H264NalType::SPS, payload + off, nalu_size);
          } else if (sub_type == 8U) {
            cb(H264NalType::PPS, payload + off, nalu_size);
          }
        }
        off += nalu_size;
      }
    }
  }
};

} //< namespace rivet::media
