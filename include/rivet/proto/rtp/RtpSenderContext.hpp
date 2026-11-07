#pragma once

#include "rivet/memory/PacketRef.hpp"
#include <cstdint>
#include <cstring>
#include <random>
#include <span>

namespace rivet::proto::rtp {

/// Defines how RIVET translates outgoing RTP packet headers.
enum class SenderMode : uint8_t {
  /// Mode A: Forwards upstream SSRC, sequence numbers, and RTP timestamps unchanged
  TransparentPassThrough,

  /// Mode B: Originates per-subscriber SSRC and affine sequence/timestamp timeline
  /// (preserving B-frame PTS-DTS non-monotonic deltas and observable mid-AU sequence loss)
  SessionOriginTranslation
};

/// Per-subscriber RTP stream synchronization and scatter-gather header rewrite context.
class RtpSenderContext {
  SenderMode mode_{SenderMode::TransparentPassThrough};
  uint32_t   ssrc_out_{0};

  uint16_t seq_offset_{0};
  uint32_t ts_offset_{0};

  uint16_t first_seq_out_{0};
  uint32_t first_ts_out_{0};
  uint16_t last_seq_out_{0};
  uint32_t last_ts_out_{0};
  bool     anchored_{false};

  uint32_t sent_packets_{0};
  uint32_t sent_octets_{0};

public:
  RtpSenderContext() noexcept = default;

  explicit RtpSenderContext(SenderMode m, uint32_t ssrc, uint16_t initial_seq_base = 1000) noexcept
      : mode_(m)
      , ssrc_out_(ssrc)
      , last_seq_out_(initial_seq_base) {
    if (ssrc_out_ == 0U && mode_ == SenderMode::SessionOriginTranslation) {
      std::random_device rd;
      ssrc_out_ = rd();
      if (ssrc_out_ == 0U) {
        ssrc_out_ = 0x52495654U;
      }
    }
  }

  [[nodiscard]] SenderMode get_mode() const noexcept { return mode_; }
  [[nodiscard]] uint32_t   get_ssrc_out() const noexcept { return ssrc_out_; }
  void                     set_ssrc_out(uint32_t ssrc) noexcept { ssrc_out_ = ssrc; }

  [[nodiscard]] uint16_t last_seq_out() const noexcept { return last_seq_out_; }
  [[nodiscard]] uint32_t last_ts_out() const noexcept { return last_ts_out_; }
  [[nodiscard]] uint16_t first_seq_out() const noexcept { return first_seq_out_; }
  [[nodiscard]] uint32_t first_ts_out() const noexcept { return first_ts_out_; }
  [[nodiscard]] bool     is_anchored() const noexcept { return anchored_; }
  [[nodiscard]] uint32_t sent_packet_count() const noexcept { return sent_packets_; }
  [[nodiscard]] uint32_t sent_octet_count() const noexcept { return sent_octets_; }

  /// Anchors initial sequence & timestamp offsets on the subscriber's first forwarded packet
  /// (used to populate RTSP `PLAY` `RTP-Info: url=...;seq=...;rtptime=...`).
  void anchor_initial(uint16_t in_seq, uint32_t in_ts) noexcept {
    if (anchored_) return;
    if (mode_ == SenderMode::SessionOriginTranslation) {
      seq_offset_    = static_cast<uint16_t>(last_seq_out_ - in_seq);
      ts_offset_     = 0U; //< Preserve relative PTS timeline from join anchor
      first_seq_out_ = static_cast<uint16_t>(in_seq + seq_offset_);
      first_ts_out_  = static_cast<uint32_t>(in_ts + ts_offset_);
    } else {
      first_seq_out_ = in_seq;
      first_ts_out_  = in_ts;
    }
    last_seq_out_ = first_seq_out_;
    last_ts_out_  = first_ts_out_;
    anchored_     = true;
  }

  /// Splices a restarted camera stream or resynchronized RAP onto a continuous downstream timeline.
  void resync_stream(uint16_t in_seq, uint32_t in_ts) noexcept {
    if (mode_ == SenderMode::TransparentPassThrough) {
      return;
    }
    if (!anchored_) {
      anchor_initial(in_seq, in_ts);
      return;
    }
    seq_offset_ = static_cast<uint16_t>((last_seq_out_ + 1U) - in_seq);
    ts_offset_  = static_cast<uint32_t>((last_ts_out_ + 3000U) - in_ts);
  }

  /// Computes outgoing sequence/timestamp for `packet` and updates RTCP SR counters.
  void prepare_forward(const memory::PacketRef &packet) noexcept {
    if (!anchored_) {
      anchor_initial(packet.sequence, packet.rtp_timestamp);
    }
    if (mode_ == SenderMode::TransparentPassThrough) {
      last_seq_out_ = packet.sequence;
      last_ts_out_  = packet.rtp_timestamp;
    } else {
      last_seq_out_ = static_cast<uint16_t>(packet.sequence + seq_offset_);
      last_ts_out_  = static_cast<uint32_t>(packet.rtp_timestamp + ts_offset_);
    }
    ++sent_packets_;
    sent_octets_ += packet.length;
  }

  /// Writes a rewritten 12..128 byte RTP header into `dst_hdr` for scatter-gather `iovec[0]`
  /// transmission in Mode B without mutating the shared immutable `PooledBuffer`!
  /// Returns the RTP header length written (`0` if `mode == TransparentPassThrough` or invalid).
  [[nodiscard]] uint32_t build_rewritten_rtp_header(const memory::PacketRef &packet,
      uint8_t                                                               *dst_hdr,
      size_t                                                                 max_hdr_cap) noexcept {
    prepare_forward(packet);
    if (mode_ == SenderMode::TransparentPassThrough) {
      return 0U;
    }
    const uint32_t hlen = packet.rtp_header_length();
    if (hlen < 12U || hlen > max_hdr_cap) {
      return 0U;
    }
    auto src = packet.payload();
    std::memcpy(dst_hdr, src.data(), hlen);

    dst_hdr[2]  = static_cast<uint8_t>((last_seq_out_ >> 8U) & 0xFFU);
    dst_hdr[3]  = static_cast<uint8_t>(last_seq_out_ & 0xFFU);
    dst_hdr[4]  = static_cast<uint8_t>((last_ts_out_ >> 24U) & 0xFFU);
    dst_hdr[5]  = static_cast<uint8_t>((last_ts_out_ >> 16U) & 0xFFU);
    dst_hdr[6]  = static_cast<uint8_t>((last_ts_out_ >> 8U) & 0xFFU);
    dst_hdr[7]  = static_cast<uint8_t>(last_ts_out_ & 0xFFU);
    dst_hdr[8]  = static_cast<uint8_t>((ssrc_out_ >> 24U) & 0xFFU);
    dst_hdr[9]  = static_cast<uint8_t>((ssrc_out_ >> 16U) & 0xFFU);
    dst_hdr[10] = static_cast<uint8_t>((ssrc_out_ >> 8U) & 0xFFU);
    dst_hdr[11] = static_cast<uint8_t>(ssrc_out_ & 0xFFU);
    return hlen;
  }
};

} //< namespace rivet::proto::rtp
