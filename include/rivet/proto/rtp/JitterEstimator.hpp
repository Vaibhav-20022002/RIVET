#pragma once

#include <cstdint>

namespace rivet::proto::rtp {

/// Implements the RFC 3550 Appendix A.8 Interarrival Jitter calculation.
/// Uses 32-bit modular arithmetic for `(R_j - R_i) - (S_j - S_i)` so
/// 32-bit RTP timestamp or local clock wrap-around never produces a spurious ~2^32 spike.
class JitterEstimator {
  double   jitter_{0.0};
  uint32_t prev_rtp_ts_{0};
  uint32_t prev_arrival_{0};
  bool     first_packet_{true};

public:
  JitterEstimator() noexcept = default;

  void reset() noexcept { *this = JitterEstimator{}; }

  double update(uint32_t rtp_timestamp, uint32_t arrival_time) noexcept {
    if (first_packet_) {
      prev_rtp_ts_  = rtp_timestamp;
      prev_arrival_ = arrival_time;
      first_packet_ = false;
      return jitter_;
    }

    /// Modular 32-bit difference: D(i, j) = (R_j - R_i) - (S_j - S_i)
    const int32_t delta_r = static_cast<int32_t>(arrival_time - prev_arrival_);
    const int32_t delta_s = static_cast<int32_t>(rtp_timestamp - prev_rtp_ts_);
    const int64_t d       = static_cast<int64_t>(delta_r) - static_cast<int64_t>(delta_s);

    const double abs_d = static_cast<double>(d < 0 ? -d : d);
    jitter_ += (abs_d - jitter_) / 16.0;

    prev_rtp_ts_  = rtp_timestamp;
    prev_arrival_ = arrival_time;
    return jitter_;
  }

  [[nodiscard]] uint32_t get_rtcp_jitter() const noexcept { return static_cast<uint32_t>(jitter_); }
};

} //< namespace rivet::proto::rtp
