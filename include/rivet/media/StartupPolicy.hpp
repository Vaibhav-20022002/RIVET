#pragma once

#include "RetainedPacketLog.hpp"
#include <vector>

namespace rivet::media {

/// An interface that defines how a new subscriber should join the live stream
class StartupPolicy {
public:
  virtual ~StartupPolicy() = default;

  /// Determines the initial burst of packets to send to a newly connected client
  ///\param log The rolling time-machine buffer of recent network packets
  ///\return A vector of packets to immediately burst to the client
  [[nodiscard]] virtual std::vector<memory::PacketRef> start(
      const RetainedPacketLog &log) const = 0;

  /// Returns true if this policy requires us to send an RTCP PLI (Picture Loss Indication) back to
  /// the camera to force it to generate a brand new keyframe
  [[nodiscard]] virtual bool requires_rtcp_pli() const noexcept = 0;
};

/// This policy prioritizes absolute lowest latency (Zero Delay)
/// It does NOT use the historical packet log. Instead, it demands a brand new IDR frame from the
/// camera. The client's screen will be black for a fraction of a second while waiting for the new
/// IDR, but once it arrives, they are 100% live.
class FreshnessFirstPolicy final : public StartupPolicy {
public:
  [[nodiscard]] std::vector<memory::PacketRef> start(const RetainedPacketLog &) const override {
    /// Return an empty burst. We wait for the live edge
    return {};
  }

  [[nodiscard]] bool requires_rtcp_pli() const noexcept override {
    /// Yes! Tell the camera to generate a new IDR immediately
    return true;
  }
};

/// This policy prioritizes absolute fastest video loading (Zero Buffering).
/// It reaches back into the time-machine, finds the last IDR frame, and blasts the historical
/// packets to the client so they get a picture instantly.
class FastPictureFirstPolicy final : public StartupPolicy {
public:
  [[nodiscard]] std::vector<memory::PacketRef> start(const RetainedPacketLog &log) const override {
    auto rap_seq = log.find_latest_rap_sequence();

    if (!rap_seq) {
      /// If the camera literally just turned on and hasn't sent an IDR yet, we have nothing to burst
      return {};
    }

    /// Extract a zero-copy snapshot of every packet from the IDR up to right now.
    /// The EgressReactor will blast this vector over the network as fast as the io_uring socket can
    /// physically handle it.
    return log.snapshot_from(rap_seq.value());
  }

  [[nodiscard]] bool requires_rtcp_pli() const noexcept override {
    /// No need to bother the camera! We already have an IDR in our memory
    return false;
  }
};

} //< namespace rivet::media