#pragma once

#include "PacketRing.hpp"
#include "RetainedPacketLog.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rivet::media {

/// Cursor-based startup decision computed atomically when a subscriber transitions to PLAY.
struct StartupPlan {
  uint64_t start_pos{0};       //< Ring cursor position to begin reading from
  uint64_t live_pos{0};        //< Live write_pos at plan creation time
  uint64_t burst_bytes{0};     //< Estimated historical bytes in [start_pos, live_pos)
  uint32_t rap_timestamp{0};   //< RTP timestamp of selected RAP (if has_rap)
  uint32_t generation{0};      //< ConfigGeneration of selected RAP
  bool     has_rap{false};     //< true if starting at a retained IRAP AU
  bool     rasl_gate{false};   //< true if starting at an open IRAP (CRA / BLA_W_LP)
  bool     wait_irap{true};    //< true if no usable RAP was available (wait for next live IRAP)
  bool     request_pli{false}; //< true if worker should emit upstream RTCP PLI
};

/// Interface defining how a new subscriber joins a live stream.
class StartupPolicy {
public:
  virtual ~StartupPolicy() = default;

  /// Computes a zero-copy ring cursor plan for EgressReactor (no vector copy required).
  [[nodiscard]] virtual StartupPlan plan_cursor(const PacketRing &ring,
      uint8_t                                                     track_id            = 0,
      uint32_t                                                    required_generation = 0,
      size_t burst_budget_bytes = 4ULL * 1024ULL * 1024ULL,
      bool   allow_open_irap    = true) const noexcept = 0;

  /// Legacy vector-returning startup helper for unit tests.
  [[nodiscard]] virtual std::vector<memory::PacketRef> start(
      const RetainedPacketLog &log) const = 0;

  [[nodiscard]] virtual bool requires_rtcp_pli() const noexcept = 0;
};

/// Prioritizes lowest latency: joins at the live edge, gates until next IRAP, requests RTCP PLI.
class FreshnessFirstPolicy final : public StartupPolicy {
public:
  [[nodiscard]] StartupPlan plan_cursor(const PacketRing &ring,
      uint8_t /*track_id*/             = 0,
      uint32_t /*required_generation*/ = 0,
      size_t /*burst_budget_bytes*/    = 0,
      bool /*allow_open_irap*/         = true) const noexcept override {
    const uint64_t head = ring.write_pos();
    StartupPlan    plan{};
    plan.start_pos   = head;
    plan.live_pos    = head;
    plan.has_rap     = false;
    plan.rasl_gate   = false;
    plan.wait_irap   = true;
    plan.request_pli = true;
    return plan;
  }

  [[nodiscard]] std::vector<memory::PacketRef> start(const RetainedPacketLog &) const override {
    return {};
  }

  [[nodiscard]] bool requires_rtcp_pli() const noexcept override { return true; }
};

/// Prioritizes instant time-to-first-frame by selecting the latest retained IRAP AU matching
/// generation, open-GOP policy, and byte-burst budget, falling back cleanly to FreshnessFirst.
class FastPictureFirstPolicy final : public StartupPolicy {
  size_t   burst_budget_bytes_;
  uint32_t required_generation_;
  bool     allow_open_irap_;
  uint8_t  track_id_;

public:
  explicit FastPictureFirstPolicy(size_t burst_budget_bytes  = 4ULL * 1024ULL * 1024ULL,
      uint32_t                           required_generation = 0,
      bool                               allow_open_irap     = true,
      uint8_t                            track_id            = 0) noexcept
      : burst_budget_bytes_(burst_budget_bytes)
      , required_generation_(required_generation)
      , allow_open_irap_(allow_open_irap)
      , track_id_(track_id) {}

  [[nodiscard]] StartupPlan plan_cursor(const PacketRing &ring,
      uint8_t                                             track_id            = 0,
      uint32_t                                            required_generation = 0,
      size_t burst_budget_bytes = 4ULL * 1024ULL * 1024ULL,
      bool   allow_open_irap    = true) const noexcept override {
    const uint64_t head       = ring.write_pos();
    const uint64_t live_bytes = ring.write_bytes();

    auto rap =
        ring.find_best_rap(track_id, required_generation, burst_budget_bytes, allow_open_irap);
    if (!rap) {
      /// Fallback to FreshnessFirst: wait for next live IRAP and request PLI
      StartupPlan fallback{};
      fallback.start_pos   = head;
      fallback.live_pos    = head;
      fallback.has_rap     = false;
      fallback.rasl_gate   = false;
      fallback.wait_irap   = true;
      fallback.request_pli = true;
      return fallback;
    }

    StartupPlan plan{};
    plan.start_pos     = rap->pos;
    plan.live_pos      = head;
    plan.burst_bytes   = (live_bytes >= rap->byte_pos) ? (live_bytes - rap->byte_pos) : 0;
    plan.rap_timestamp = rap->rtp_ts;
    plan.generation    = rap->generation;
    plan.has_rap       = true;
    plan.rasl_gate     = rap->open_rap;
    plan.wait_irap     = false;
    plan.request_pli   = false;
    return plan;
  }

  [[nodiscard]] std::vector<memory::PacketRef> start(const RetainedPacketLog &log) const override {
    return log.snapshot_from_latest_rap(
        burst_budget_bytes_, required_generation_, allow_open_irap_, track_id_);
  }

  [[nodiscard]] bool requires_rtcp_pli() const noexcept override { return false; }
};

} //< namespace rivet::media
