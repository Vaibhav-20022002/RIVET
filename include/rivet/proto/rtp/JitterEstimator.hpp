#pragma once

#include <cmath>
#include <cstdint>

namespace rivet::proto::rtp {

/// Implements the RFC 3550 Appendix A.8 Interarrival Jitter calculation.
/// This calculates the smoothed variance in network latency (jitter), which is strictly required to
/// be sent back to camera via RTCP Receiver Reports (RR).
class JitterEstimator {
  /// The running smoothed jitter estimate, stored in RTP timestamp units (e.g., 90kHz)
  double jitter{0};

  /// The difference between the local receive time and the packet's send time for the previous
  /// packet we received.
  int64_t previous_transit{0};

  /// True if this is the first packet we are processing
  bool first_packet{true};

public:
  JitterEstimator() noexcept = default;

  /// Updates the jitter estimate using the arrival of a new packet
  ///\param rtp_timestamp The RTP tiimestamp embedded in the packet's header (S_i)
  ///\param arrival_time The local clock time the packet arrived, converted into the exact same unit
  ///                    as the rtp_timestamp (R_i)
  ///\return The current smoothed jitter value (for logging and thresholding)
  double update(uint32_t rtp_timestamp, uint32_t arrival_time) noexcept {
    /// Calculate the transit time of this packet (R_i - S_i)
    /// We use int64_t to safely handle wrap-around differences
    int64_t transit = static_cast<int64_t>(arrival_time) - static_cast<int64_t>(rtp_timestamp);

    if (first_packet) {
      previous_transit = transit;
      first_packet     = false;
      return jitter;
    }

    /// Calculate the difference in transit times between this packet and the previous one
    /// D(i, j) = (R_j - S_j) - (R_i - S_i)
    int64_t d = transit - previous_transit;

    /// RFC 3550 states jitter is smoothed using an Exponentially Weighted Moving Average (EWMA) :
    /// J(i) = J(i - 1) + (|D(i - 1, i)| - J(i - 1)) / 16
    double diff = std::abs(static_cast<double>(d));
    jitter += (diff - jitter) / 16.0;

    previous_transit = transit;

    return jitter;
  }

  /// Returns the current smoothed jitter as an unsigned 32-bit integer, which is the exact format
  /// required for the RTCP Receiver Report packet structure
  [[nodiscard]] uint32_t get_rtcp_jitter() const noexcept { return static_cast<uint32_t>(jitter); }
};

} //< namespace rivet::proto::rtp