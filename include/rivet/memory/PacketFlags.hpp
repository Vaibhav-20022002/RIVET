#pragma once

#include <cstdint>

namespace rivet::memory::pkt {

/// ============================================================================
/// PacketRef Byte 31 (`flags`) Bitmask Constants
/// ============================================================================
inline constexpr uint8_t kNone       = 0x00;
inline constexpr uint8_t kClosedIrap = 0x01; //< H.264 IDR; HEVC IDR_*, BLA_W_RADL, BLA_N_LP
inline constexpr uint8_t kOpenIrap   = 0x02; //< HEVC CRA_NUT, BLA_W_LP (RASL may follow)
inline constexpr uint8_t kIrapMask   = kClosedIrap | kOpenIrap;
inline constexpr uint8_t kRef        = 0x04; //< Reference picture (dropping breaks later pictures)
inline constexpr uint8_t kMarker     = 0x08; //< RTP Marker bit (M = 1, end of AU)
inline constexpr uint8_t kRasl       = 0x10; //< HEVC RASL_N / RASL_R leading picture
inline constexpr uint8_t kRadl       = 0x20; //< HEVC RADL_N / RADL_R decodable leading picture
inline constexpr uint8_t kParamSet   = 0x40; //< Contains VPS/SPS/PPS (H.264: SPS/PPS)
inline constexpr uint8_t kAuStart    = 0x80; //< First packet of an Access Unit (set at ingest)

/// ============================================================================
/// PacketRef Byte 30 (`track_id`) Packed Metadata Helpers
///   Bits 0..2 : track index (0..7)
///   Bits 3..5 : temporal_id (0..6; 7 = non-VCL / unknown)
///   Bit  6    : vcl (1 if packet carries at least one VCL NAL)
///   Bit  7    : switch_up (1 if HEVC TSA / STSA temporal up-switch point)
///
/// Note: A plain integer track index `0..7` has tid=0, vcl=0, switch_up=0,
/// preserving full backwards compatibility with callers passing `0` or `1`.
/// ============================================================================
[[nodiscard]] constexpr uint8_t pack_track_meta(uint8_t track,
    uint8_t                                             temporal_id = 0,
    bool                                                vcl         = false,
    bool                                                switch_up   = false) noexcept {
  return static_cast<uint8_t>((track & 0x07U) | ((temporal_id & 0x07U) << 3U) |
      (vcl ? 0x40U : 0x00U) | (switch_up ? 0x80U : 0x00U));
}

[[nodiscard]] constexpr uint8_t unpack_track(uint8_t packed) noexcept {
  return static_cast<uint8_t>(packed & 0x07U);
}

[[nodiscard]] constexpr uint8_t unpack_temporal_id(uint8_t packed) noexcept {
  return static_cast<uint8_t>((packed >> 3U) & 0x07U);
}

[[nodiscard]] constexpr bool unpack_vcl(uint8_t packed) noexcept {
  return (packed & 0x40U) != 0;
}

[[nodiscard]] constexpr bool unpack_switch_up(uint8_t packed) noexcept {
  return (packed & 0x80U) != 0;
}

} //< namespace rivet::memory::pkt
