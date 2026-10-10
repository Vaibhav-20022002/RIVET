#pragma once

#include "PacketRef.hpp"
#include <cstdint>

namespace rivet::proto::rtp {

/// Define how the RIVET server modifies outgoing RTP packets before sending them
enum class SenderMode : uint8_t {
  /// Transparently forwards the exact packet from the camera (same SSRC, seq, and TS)
  TransparentPassThrough,

  /// Rewrites the SSRC, Sequence, and Timestamp to originate from RIVET, making it look like a
  /// completely new, contiguous stream to the client
  SessionOriginTranslation
};

/// Maintains state for an individual RTP subscriber, determining how their outgoing stream headers
/// are rewritten (or preserved) before io_uring dispatch.
class RtpSenderContext {
  SenderMode mode{SenderMode::TransparentPassThrough};

  /// The SSRC (Synchronization Source ID) that this specific client expects to see
  uint32_t ssrc_out{0};

  /// To create a seamless stream even if the camera restarts or we switch sources, we must apply
  /// continuous offsets to the sequence numbers and timestamps.
  uint16_t seq_offset{0};
  uint32_t ts_offset{0};

  /// Tracks the last sequence number and timestamp sent out to this specific client
  uint16_t last_seq_out{0};
  uint32_t last_ts_out{0};

public:
  RtpSenderContext() noexcept = default;

  explicit RtpSenderContext(SenderMode m, uint32_t ssrc) noexcept
      : mode(m)
      , ssrc_out(ssrc) {}

  [[nodiscard]] SenderMode get_mode() const noexcept { return mode; }

  [[nodiscard]] uint32_t get_ssrc_out() const noexcept { return ssrc_out; }

  /// Configure the offsets required to cleanly splice a new camera stream into the client's
  /// existing viewing session
  void resync_stream(uint16_t in_seq, uint32_t in_ts) noexcept {
    if (mode == SenderMode::TransparentPassThrough) {
      return;
    }

    /// The new outgoing sequence should cleanly pick up where we left off
    seq_offset = (last_seq_out + 1) - in_seq;

    /// The timestamp should also cleanly pick up without a huge jump
    /// (Add ~3000 ticks as a safe 90kHz baseline for 1 video frame delta)
    ts_offset = (last_ts_out + 3000) - in_ts;
  }

  /// Process the packet reference based on the sender mode.
  /// If SessionOriginTranslation is used, this calculates the new header offsets.
  void prepare_forward(const memory::PacketRef &packet) noexcept {
    if (mode == SenderMode::TransparentPassThrough) {
      last_seq_out = packet.sequence;
      last_ts_out  = packet.rtp_timestamp;
      return;
    }

    /// Calculate the translated values for this specific client
    last_seq_out = packet.sequence + seq_offset;
    last_ts_out  = packet.rtp_timestamp + ts_offset;
  }
};

} //< namespace rivet::proto::rtp