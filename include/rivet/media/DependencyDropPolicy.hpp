#pragma once

#include "../memory/PacketRef.hpp"
#include "AccessUnitView.hpp"
#include <algorithm>
#include <cstdint>

namespace rivet::media {

/// The action the subscriber pipeline should take for a packet / Access Unit.
enum class DropDecision : uint8_t {
  /// Admit packet / Access Unit
  Keep = 0,

  /// Drop entire Access Unit cleanly while keeping connection alive
  Drop = 1,

  /// Queue completely saturated (100%+) or sustained stall -> disconnect subscriber
  Disconnect = 2
};

/// Compact per-subscriber Access Unit admission state machine.
/// Enforces whole-AU atomic drops, parameter-set pass-through, `wait_irap` reference-integrity
/// recovery, Open-GOP `rasl_gate` filtering, and HEVC TemporalId (`tid_ceiling`) scaling with
/// 500 ms hysteresis and TSA/STSA up-switch awareness.
struct AuGate {
  uint64_t tid_lowered_at_ns{0};
  uint32_t current_au_ts{0};
  uint8_t  tid_ceiling{7};
  bool     has_au_ts{false};
  bool     vcl_decided_for_au{false};
  bool     dropping_current_au{false};
  bool     wait_irap{false};
  bool     rasl_gate{false};

  void reset(uint8_t initial_tid_ceiling = 7,
      bool           start_wait_irap     = false,
      bool           start_rasl_gate     = false) noexcept {
    tid_lowered_at_ns   = 0;
    current_au_ts       = 0;
    tid_ceiling         = initial_tid_ceiling;
    has_au_ts           = false;
    vcl_decided_for_au  = false;
    dropping_current_au = false;
    wait_irap           = start_wait_irap;
    rasl_gate           = start_rasl_gate;
  }

  void on_gap_or_overflow() noexcept {
    vcl_decided_for_au  = true;
    dropping_current_au = true;
    wait_irap           = true;
  }
};

/// Result of `DependencyDropPolicy::admit()` with telemetry breakdown flags.
struct AuAdmitResult {
  DropDecision decision{DropDecision::Keep};
  bool         started_au_drop{false};   //< true on first dropped VCL packet of a dropped AU
  bool         dropped_rasl{false};      //< dropped specifically due to Open-GOP RASL gate
  bool         dropped_sublayer{false};  //< dropped specifically due to TemporalId > tid_ceiling
  bool         entered_wait_irap{false}; //< Reference/IRAP drop broke DPB -> needs PLI / next IRAP
};

/// Dependency-aware whole-AU backpressure & DPB reference-integrity policy.
class DependencyDropPolicy {
public:
  static constexpr uint8_t  DISPOSABLE_DROP_THRESHOLD = 60;
  static constexpr uint8_t  SUBLAYER_CAP_MID_PCT      = 60;
  static constexpr uint8_t  SUBLAYER_CAP_BASE_PCT     = 75;
  static constexpr uint8_t  REFERENCE_DROP_THRESHOLD  = 90;
  static constexpr uint8_t  CRITICAL_DROP_THRESHOLD   = 95;
  static constexpr uint64_t TID_HYSTERESIS_NS         = 500'000'000ULL; //< 500 ms

  DependencyDropPolicy() noexcept = default;

  /// Stateless priority vs fill-percentage evaluator (used by unit tests & simple callers).
  [[nodiscard]] DropDecision evaluate(DependencyPriority prio,
      uint8_t                                            queue_fill_pct) const noexcept {
    if (queue_fill_pct >= 100) {
      return DropDecision::Disconnect;
    }
    if (prio == DependencyPriority::Disposable && queue_fill_pct >= DISPOSABLE_DROP_THRESHOLD) {
      return DropDecision::Drop;
    }
    if (prio == DependencyPriority::Reference && queue_fill_pct >= REFERENCE_DROP_THRESHOLD) {
      return DropDecision::Drop;
    }
    if (prio == DependencyPriority::Critical && queue_fill_pct >= CRITICAL_DROP_THRESHOLD) {
      return DropDecision::Drop;
    }
    return DropDecision::Keep;
  }

  [[nodiscard]] DropDecision evaluate(const AccessUnitView &au,
      uint8_t                                               queue_fill_pct) const noexcept {
    return evaluate(au.get_priority(), queue_fill_pct);
  }

  /// Stateful whole-AU packet admission preserving H.264 / HEVC DPB reference integrity.
  [[nodiscard]] AuAdmitResult admit(AuGate &gate,
      const memory::PacketRef              &pkt,
      uint8_t                               fill_pct,
      uint8_t                               highest_tid     = 0,
      uint64_t                              now_ns          = 0,
      bool                                  allow_open_irap = true,
      bool                                  is_video_track  = true) const noexcept {
    AuAdmitResult res{};

    if (fill_pct >= 100) {
      res.decision = DropDecision::Disconnect;
      return res;
    }

    /// Non-video tracks (Audio / Opaque): no GOP/IRAP dependency chain, drop only at >= 95%.
    if (!is_video_track) {
      res.decision =
          (fill_pct >= CRITICAL_DROP_THRESHOLD) ? DropDecision::Drop : DropDecision::Keep;
      return res;
    }

    /// Detect AU boundary via RTP timestamp change or explicit pkt::kAuStart flag.
    const bool new_au =
        !gate.has_au_ts || (pkt.rtp_timestamp != gate.current_au_ts) || pkt.is_au_start();
    if (new_au && (!gate.has_au_ts || pkt.rtp_timestamp != gate.current_au_ts)) {
      gate.current_au_ts       = pkt.rtp_timestamp;
      gate.has_au_ts           = true;
      gate.vcl_decided_for_au  = false;
      gate.dropping_current_au = false;
    }

    /// 1. Parameter-set packets (VPS / SPS / PPS) ALWAYS pass through below 100% fill
    ///    and must NOT lock in the VCL decision for the enclosing AU.
    if (pkt.is_param_set() && !pkt.is_vcl()) {
      res.decision = DropDecision::Keep;
      return res;
    }

    /// 2. If the VCL admission decision for this AU has already been locked in,
    ///    every subsequent slice/fragment/suffix-SEI of this AU follows it atomically!
    if (gate.vcl_decided_for_au) {
      res.decision = gate.dropping_current_au ? DropDecision::Drop : DropDecision::Keep;
      return res;
    }

    /// 3. Non-VCL prefix NALs (e.g. AUD, Prefix SEI) arriving before the AU's first VCL slice:
    ///    Do NOT lock `vcl_decided_for_au` yet so an upcoming IDR slice at the same timestamp
    ///    is evaluated cleanly!
    const bool treat_as_vcl =
        pkt.is_vcl() || pkt.is_rap() || pkt.is_rasl() || pkt.is_radl() || pkt.is_reference();
    if (!treat_as_vcl) {
      if (gate.wait_irap || fill_pct >= DISPOSABLE_DROP_THRESHOLD) {
        res.decision = DropDecision::Drop;
      } else {
        res.decision = DropDecision::Keep;
      }
      return res;
    }

    /// 4. First VCL packet of the Access Unit: update TemporalId ceiling & make whole-AU decision.
    const uint8_t max_tid = std::min<uint8_t>(highest_tid, 7);
    if (gate.tid_ceiling > max_tid) {
      gate.tid_ceiling = max_tid;
    }

    /// Congestion-driven TemporalId shedding
    if (max_tid > 0) {
      if (fill_pct >= SUBLAYER_CAP_BASE_PCT) {
        if (gate.tid_ceiling > 0) {
          gate.tid_ceiling       = 0;
          gate.tid_lowered_at_ns = now_ns;
        }
      } else if (fill_pct >= SUBLAYER_CAP_MID_PCT) {
        const uint8_t target = static_cast<uint8_t>(max_tid - 1);
        if (gate.tid_ceiling > target) {
          gate.tid_ceiling       = target;
          gate.tid_lowered_at_ns = now_ns;
        }
      }
    }

    const uint8_t pkt_tid      = pkt.temporal_id();
    const bool    is_irap      = pkt.is_rap();
    const bool    is_closed    = pkt.is_idr();
    const bool    is_open_only = pkt.is_open_rap() && !is_closed;

    /// Case A: IRAP Access Unit (IDR / CRA / BLA)
    if (is_irap) {
      if (fill_pct >= CRITICAL_DROP_THRESHOLD) {
        /// L3 congestion: cannot even fit IRAP -> drop entire AU and force wait_irap
        gate.vcl_decided_for_au  = true;
        gate.dropping_current_au = true;
        if (!gate.wait_irap) {
          res.entered_wait_irap = true;
        }
        gate.wait_irap      = true;
        res.decision        = DropDecision::Drop;
        res.started_au_drop = true;
        return res;
      }

      if (gate.wait_irap && is_open_only && !allow_open_irap) {
        /// Policy forbids recovering on an Open-GOP CRA/BLA_W_LP; keep waiting for closed IDR
        gate.vcl_decided_for_au  = true;
        gate.dropping_current_au = true;
        res.decision             = DropDecision::Drop;
        res.started_au_drop      = true;
        return res;
      }

      const bool was_missing_prior_gop = gate.wait_irap || (gate.tid_ceiling < max_tid);
      gate.wait_irap                   = false;

      if (fill_pct < SUBLAYER_CAP_MID_PCT) {
        gate.tid_ceiling = max_tid;
      }

      if (is_closed) {
        gate.rasl_gate = false;
      } else if (is_open_only && was_missing_prior_gop) {
        /// Recovering or stepping up sub-layers on Open-GOP IRAP -> arm RASL gate!
        gate.rasl_gate = true;
      }

      gate.vcl_decided_for_au  = true;
      gate.dropping_current_au = false;
      res.decision             = DropDecision::Keep;
      return res;
    }

    /// Case B: Waiting for IRAP recovery (`wait_irap == true`) -> drop all delta AUs
    if (gate.wait_irap) {
      gate.vcl_decided_for_au  = true;
      gate.dropping_current_au = true;
      res.decision             = DropDecision::Drop;
      res.started_au_drop      = true;
      return res;
    }

    /// Case C: Open-GOP RASL gate active (`rasl_gate == true`)
    if (gate.rasl_gate) {
      if (pkt.is_rasl()) {
        gate.vcl_decided_for_au  = true;
        gate.dropping_current_au = true;
        res.decision             = DropDecision::Drop;
        res.started_au_drop      = true;
        res.dropped_rasl         = true;
        return res;
      }
      if (!pkt.is_radl()) {
        /// First trailing picture reached -> Open-GOP leading picture phase is over!
        gate.rasl_gate = false;
      }
    }

    /// Case D: Temporal sub-layer step-up check after 500 ms hysteresis
    if (gate.tid_ceiling < max_tid && fill_pct < SUBLAYER_CAP_MID_PCT &&
        (now_ns >= gate.tid_lowered_at_ns + TID_HYSTERESIS_NS)) {
      if (pkt.is_switch_up() && pkt_tid == static_cast<uint8_t>(gate.tid_ceiling + 1)) {
        gate.tid_ceiling = pkt_tid;
      }
    }

    /// Case E: Temporal sub-layer filtering (`pkt_tid > tid_ceiling`)
    /// Dropping higher TemporalId never corrupts lower TemporalId references!
    if (pkt_tid > gate.tid_ceiling) {
      gate.vcl_decided_for_au  = true;
      gate.dropping_current_au = true;
      res.decision             = DropDecision::Drop;
      res.started_au_drop      = true;
      res.dropped_sublayer     = true;
      return res;
    }

    /// Case F: Priority vs congestion thresholds for `pkt_tid <= tid_ceiling`
    const bool is_ref = pkt.is_reference();
    if (!is_ref && fill_pct >= DISPOSABLE_DROP_THRESHOLD) {
      /// Sub-layer non-reference / non-ref B-frame / RASL: safe to drop without wait_irap!
      gate.vcl_decided_for_au  = true;
      gate.dropping_current_au = true;
      res.decision             = DropDecision::Drop;
      res.started_au_drop      = true;
      return res;
    }

    if (is_ref && fill_pct >= REFERENCE_DROP_THRESHOLD) {
      /// Dropping a Reference picture breaks DPB prediction for subsequent delta frames ->
      /// MUST arm `wait_irap = true`!
      gate.vcl_decided_for_au  = true;
      gate.dropping_current_au = true;
      gate.wait_irap           = true;
      res.decision             = DropDecision::Drop;
      res.started_au_drop      = true;
      res.entered_wait_irap    = true;
      return res;
    }

    gate.vcl_decided_for_au  = true;
    gate.dropping_current_au = false;
    res.decision             = DropDecision::Keep;
    return res;
  }
};

} //< namespace rivet::media
