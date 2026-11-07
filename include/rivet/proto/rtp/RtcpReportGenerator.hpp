#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rivet::proto::rtp {

/// RTCP Packet Type constants (RFC 3550 & RFC 4585)
inline constexpr uint8_t kRtcpPtSr    = 200;
inline constexpr uint8_t kRtcpPtRr    = 201;
inline constexpr uint8_t kRtcpPtSdes  = 202;
inline constexpr uint8_t kRtcpPtBye   = 203;
inline constexpr uint8_t kRtcpPtApp   = 204;
inline constexpr uint8_t kRtcpPtRtpfb = 205;
inline constexpr uint8_t kRtcpPtPsfb  = 206; //< Payload-Specific Feedback (PLI: FMT=1, FIR: FMT=4)

/// Represents an RTCP Receiver Report (RR) block (RFC 3550 Section 6.4.2)
struct ReceiverReportBlock {
  uint32_t ssrc{0};
  uint8_t  fraction_lost{0};
  uint32_t cumulative_lost{0};
  uint32_t highest_seq_recv{0};
  uint32_t jitter{0};
  uint32_t last_sr_timestamp{0};
  uint32_t delay_since_last_sr{0};
};

/// Represents an RTCP Sender Report (SR) header (RFC 3550 Section 6.4.1)
struct SenderReportInfo {
  uint32_t ntp_sec{0};
  uint32_t ntp_frac{0};
  uint32_t rtp_timestamp{0};
  uint32_t sender_packet_count{0};
  uint32_t sender_octet_count{0};
};

struct ParsedRtcpSummary {
  bool                               valid{false};
  uint8_t                            packet_type{0};
  uint8_t                            rc_or_fmt{0};
  uint32_t                           sender_ssrc{0};
  std::optional<ReceiverReportBlock> first_rr{};
  std::optional<SenderReportInfo>    sr_info{};
};

/// Generates and parses binary RFC 3550 / RFC 4585 RTCP packets (SR, RR, PLI, BYE).
class RtcpReportGenerator {
  uint32_t our_ssrc_{0};

  static void write_u32_be(uint8_t *dst, uint32_t val) noexcept {
    dst[0] = static_cast<uint8_t>((val >> 24U) & 0xFFU);
    dst[1] = static_cast<uint8_t>((val >> 16U) & 0xFFU);
    dst[2] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
    dst[3] = static_cast<uint8_t>(val & 0xFFU);
  }

  [[nodiscard]] static uint32_t read_u32_be(const uint8_t *src) noexcept {
    return (static_cast<uint32_t>(src[0]) << 24U) | (static_cast<uint32_t>(src[1]) << 16U) |
        (static_cast<uint32_t>(src[2]) << 8U) | static_cast<uint32_t>(src[3]);
  }

public:
  explicit RtcpReportGenerator(uint32_t ssrc = 0) noexcept
      : our_ssrc_(ssrc) {}

  void                   set_ssrc(uint32_t ssrc) noexcept { our_ssrc_ = ssrc; }
  [[nodiscard]] uint32_t ssrc() const noexcept { return our_ssrc_; }

  /// Builds a 32-byte binary RTCP Receiver Report (RR, PT=201) with 1 report block.
  [[nodiscard]] std::vector<uint8_t> build_rr(const ReceiverReportBlock &block) const {
    std::vector<uint8_t> packet(32, 0);
    packet[0] = 0x81; //< V=2, P=0, RC=1
    packet[1] = kRtcpPtRr;
    packet[2] = 0;
    packet[3] = 7; //< 8 32-bit words minus 1

    write_u32_be(&packet[4], our_ssrc_);
    write_u32_be(&packet[8], block.ssrc);

    packet[12] = block.fraction_lost;
    packet[13] = static_cast<uint8_t>((block.cumulative_lost >> 16U) & 0xFFU);
    packet[14] = static_cast<uint8_t>((block.cumulative_lost >> 8U) & 0xFFU);
    packet[15] = static_cast<uint8_t>(block.cumulative_lost & 0xFFU);

    write_u32_be(&packet[16], block.highest_seq_recv);
    write_u32_be(&packet[20], block.jitter);
    write_u32_be(&packet[24], block.last_sr_timestamp);
    write_u32_be(&packet[28], block.delay_since_last_sr);
    return packet;
  }

  /// Builds a fixed 28-byte RTCP Sender Report (SR, PT=200, RC=0) on the stack without allocation.
  [[nodiscard]] std::array<uint8_t, 28> build_sr(const SenderReportInfo &sr) const noexcept {
    std::array<uint8_t, 28> pkt{};
    pkt[0] = 0x80; //< V=2, P=0, RC=0
    pkt[1] = kRtcpPtSr;
    pkt[2] = 0;
    pkt[3] = 6; //< 7 32-bit words minus 1

    write_u32_be(&pkt[4], our_ssrc_);
    write_u32_be(&pkt[8], sr.ntp_sec);
    write_u32_be(&pkt[12], sr.ntp_frac);
    write_u32_be(&pkt[16], sr.rtp_timestamp);
    write_u32_be(&pkt[20], sr.sender_packet_count);
    write_u32_be(&pkt[24], sr.sender_octet_count);
    return pkt;
  }

  /// Builds a 12-byte RFC 4585 §6.3.1 Picture Loss Indication (PLI, PT=206, FMT=1) packet.
  [[nodiscard]] std::array<uint8_t, 12> build_pli(uint32_t media_source_ssrc) const noexcept {
    std::array<uint8_t, 12> pkt{};
    pkt[0] = 0x81; //< V=2, P=0, FMT=1 (PLI)
    pkt[1] = kRtcpPtPsfb;
    pkt[2] = 0;
    pkt[3] = 2; //< 3 32-bit words minus 1

    write_u32_be(&pkt[4], our_ssrc_);
    write_u32_be(&pkt[8], media_source_ssrc);
    return pkt;
  }

  /// Builds an 8-byte RFC 3550 §6.6 RTCP BYE (PT=203, SC=1) packet for SSRC discontinuity notice.
  [[nodiscard]] static std::array<uint8_t, 8> build_bye(uint32_t departing_ssrc) noexcept {
    std::array<uint8_t, 8> pkt{};
    pkt[0] = 0x81; //< V=2, P=0, SC=1
    pkt[1] = kRtcpPtBye;
    pkt[2] = 0;
    pkt[3] = 1; //< 2 32-bit words minus 1
    write_u32_be(&pkt[4], departing_ssrc);
    return pkt;
  }

  /// Zero-allocation parser for incoming RTCP packets (SR, RR, compound SR+SDES, PLI, BYE).
  [[nodiscard]] static ParsedRtcpSummary parse(std::span<const uint8_t> data) noexcept {
    ParsedRtcpSummary out{};
    if (data.size() < 8U) return out;

    const uint8_t v = (data[0] >> 6U) & 0x03U;
    if (v != 2U) return out;

    const uint8_t  pt = data[1];
    const uint16_t len_words =
        static_cast<uint16_t>((static_cast<uint16_t>(data[2]) << 8U) | data[3]);
    const size_t pkt_bytes = (static_cast<size_t>(len_words) + 1U) * 4U;
    if (pkt_bytes > data.size() || pt < 200U || pt > 207U) {
      return out;
    }

    out.valid       = true;
    out.packet_type = pt;
    out.rc_or_fmt   = data[0] & 0x1FU;
    out.sender_ssrc = read_u32_be(&data[4]);

    if (pt == kRtcpPtSr && pkt_bytes >= 28U) {
      SenderReportInfo sr{};
      sr.ntp_sec             = read_u32_be(&data[8]);
      sr.ntp_frac            = read_u32_be(&data[12]);
      sr.rtp_timestamp       = read_u32_be(&data[16]);
      sr.sender_packet_count = read_u32_be(&data[20]);
      sr.sender_octet_count  = read_u32_be(&data[24]);
      out.sr_info            = sr;

      if (out.rc_or_fmt >= 1U && pkt_bytes >= 52U) {
        ReceiverReportBlock rr{};
        rr.ssrc            = read_u32_be(&data[28]);
        rr.fraction_lost   = data[32];
        rr.cumulative_lost = (static_cast<uint32_t>(data[33]) << 16U) |
            (static_cast<uint32_t>(data[34]) << 8U) | data[35];
        rr.highest_seq_recv    = read_u32_be(&data[36]);
        rr.jitter              = read_u32_be(&data[40]);
        rr.last_sr_timestamp   = read_u32_be(&data[44]);
        rr.delay_since_last_sr = read_u32_be(&data[48]);
        out.first_rr           = rr;
      }
    } else if (pt == kRtcpPtRr && out.rc_or_fmt >= 1U && pkt_bytes >= 32U) {
      ReceiverReportBlock rr{};
      rr.ssrc            = read_u32_be(&data[8]);
      rr.fraction_lost   = data[12];
      rr.cumulative_lost = (static_cast<uint32_t>(data[13]) << 16U) |
          (static_cast<uint32_t>(data[14]) << 8U) | data[15];
      rr.highest_seq_recv    = read_u32_be(&data[16]);
      rr.jitter              = read_u32_be(&data[20]);
      rr.last_sr_timestamp   = read_u32_be(&data[24]);
      rr.delay_since_last_sr = read_u32_be(&data[28]);
      out.first_rr           = rr;
    }

    return out;
  }
};

} //< namespace rivet::proto::rtp
