#pragma once

#include <cstdint>
#include <vector>

namespace rivet::proto::rtp {

/// Represents an RTCP Receiver Report (RR) block (RFC 3550 Section 6.4.2)
struct ReceiverReportBlock {
  uint32_t ssrc{0};                //< The SSRC of the stream this report is about
  uint8_t  fraction_lost{0};       //< Percentage of packets lost since last report (0-255)
  uint32_t cumulative_lost{0};     //< Total packets lost since the beginning of the stream (24-bit)
  uint32_t highest_seq_recv{0};    //< Extended highest sequence number received (32-bit)
  uint32_t jitter{0};              //< Smoothed interarrival jitter (From JitterEstimator)
  uint32_t last_sr_timestamp{0};   //< Middle 32 bits of NTP timestamp from last Sender Report
  uint32_t delay_since_last_sr{0}; //< Delay since receiving the last Sender Report
};

/// Generates binary RTCP feedback packets to send back to the camera
class RtcpReportGenerator {
  uint32_t our_ssrc{0}; //< The SSRC that RIVET uses to identify itself to the camera

public:
  explicit RtcpReportGenerator(uint32_t ssrc) noexcept
      : our_ssrc(ssrc) {}

  /// Builds a complete binary RTCP Receiver Report (RR) packet
  /// This binary blob can be directly written to the UDP socket
  [[nodiscard]] std::vector<uint8_t> build_rr(const ReceiverReportBlock &block) const {
    std::vector<uint8_t> packet(32, 0); //< A standard RR with 1 block is exactly 32 bytes

    /// RTCP Header (8 bytes)
    packet[0] = 0x81; //< Version 2 (10), Padding (0), Report Count (1)
    packet[1] = 201;  //< Packet Type: Receiver Report (201)

    /// Length of the packet in 32-bit words minus 1 (32 bytes = 8 words. 8 - 1 = 7)
    packet[2] = 0;
    packet[3] = 7;

    /// Our SSRC identifying RIVET (4 bytes)
    packet[4] = (our_ssrc >> 24) & 0xFF;
    packet[5] = (our_ssrc >> 16) & 0xFF;
    packet[6] = (our_ssrc >> 8) & 0xFF;
    packet[7] = our_ssrc & 0xFF;

    /// Report Block (24 bytes)
    /// Target SSRC (The camera we are reporting on)
    packet[8]  = (block.ssrc >> 24) & 0xFF;
    packet[9]  = (block.ssrc >> 16) & 0xFF;
    packet[10] = (block.ssrc >> 8) & 0xFF;
    packet[11] = block.ssrc & 0xFF;

    /// Fraction Lost (1 byte) + Cumulative Lost (3 bytes)
    packet[12] = block.fraction_lost;
    packet[13] = (block.cumulative_lost >> 16) & 0xFF;
    packet[14] = (block.cumulative_lost >> 8) & 0xFF;
    packet[15] = block.cumulative_lost & 0xFF;

    /// Extended Highest Sequence Number (4 bytes)
    packet[16] = (block.highest_seq_recv >> 24) & 0xFF;
    packet[17] = (block.highest_seq_recv >> 16) & 0xFF;
    packet[18] = (block.highest_seq_recv >> 8) & 0xFF;
    packet[19] = block.highest_seq_recv & 0xFF;

    /// Jitter (4 bytes)
    packet[20] = (block.jitter >> 24) & 0xFF;
    packet[21] = (block.jitter >> 16) & 0xFF;
    packet[22] = (block.jitter >> 8) & 0xFF;
    packet[23] = block.jitter & 0xFF;

    /// Last SR Timestamp (4 bytes)
    packet[24] = (block.last_sr_timestamp >> 24) & 0xFF;
    packet[25] = (block.last_sr_timestamp >> 16) & 0xFF;
    packet[26] = (block.last_sr_timestamp >> 8) & 0xFF;
    packet[27] = block.last_sr_timestamp & 0xFF;

    /// Delay Since Last SR (4 bytes)
    packet[28] = (block.delay_since_last_sr >> 24) & 0xFF;
    packet[29] = (block.delay_since_last_sr >> 16) & 0xFF;
    packet[30] = (block.delay_since_last_sr >> 8) & 0xFF;
    packet[31] = block.delay_since_last_sr & 0xFF;

    return packet;
  }
};

} //< namespace rivet::proto::rtp