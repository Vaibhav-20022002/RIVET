#include "rivet/core/SubscriberTable.hpp"
#include "rivet/media/DependencyDropPolicy.hpp"
#include "rivet/media/HevcPayload.hpp"
#include "rivet/media/HevcRapClassifier.hpp"
#include "rivet/media/RetainedPacketLog.hpp"
#include "rivet/media/StartupPolicy.hpp"
#include "rivet/memory/NumaArena.hpp"
#include "rivet/memory/PacketFlags.hpp"
#include "rivet/proto/rtp/RtpPacketClassifier.hpp"
#include "tests/support/ArenaLeakGuard.hpp"
#include "tests/support/HevcSynth.hpp"
#include "tests/support/RtpBuilder.hpp"
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <random>
#include <vector>

using namespace rivet::media;
using namespace rivet::memory;
using namespace rivet::test;

/// ============================================================================
/// 1. Exhaustive 64-Entry `hevc::kLut` Verification
/// ============================================================================

TEST(HevcLutTest, Exhaustive64NalTypeSemantics) {
  for (uint8_t t = 0; t < 64; ++t) {
    const auto info = hevc::kLut[t];

    /// VCL check: 0..31 are VCL, 32..63 are non-VCL
    if (t <= 31) {
      EXPECT_NE(info.attr & hevc::kVcl, 0) << "NAL " << int(t);
    } else {
      EXPECT_EQ(info.attr & hevc::kVcl, 0) << "NAL " << int(t);
    }

    /// TSA / STSA temporal switch-up points: strictly 2, 3, 4, 5
    if (t >= 2 && t <= 5) {
      EXPECT_NE(info.attr & hevc::kSwitchUp, 0) << "NAL " << int(t);
    } else {
      EXPECT_EQ(info.attr & hevc::kSwitchUp, 0) << "NAL " << int(t);
    }

    /// RADL: strictly 6, 7
    if (t == 6 || t == 7) {
      EXPECT_NE(info.flags & pkt::kRadl, 0) << "NAL " << int(t);
    } else {
      EXPECT_EQ(info.flags & pkt::kRadl, 0) << "NAL " << int(t);
    }

    /// RASL: strictly 8, 9 (NAL 9 = RASL_R MUST be kRasl, NOT kRadl!)
    if (t == 8 || t == 9) {
      EXPECT_NE(info.flags & pkt::kRasl, 0) << "NAL " << int(t);
      EXPECT_EQ(info.flags & pkt::kRadl, 0) << "NAL " << int(t);
    } else {
      EXPECT_EQ(info.flags & pkt::kRasl, 0) << "NAL " << int(t);
    }

    /// Closed vs Open IRAPs
    if (t == 16 || t == 21) {
      EXPECT_NE(info.flags & pkt::kOpenIrap, 0) << "NAL " << int(t);
      EXPECT_EQ(info.flags & pkt::kClosedIrap, 0) << "NAL " << int(t);
    } else if (t >= 17 && t <= 20) {
      EXPECT_NE(info.flags & pkt::kClosedIrap, 0) << "NAL " << int(t);
      EXPECT_EQ(info.flags & pkt::kOpenIrap, 0) << "NAL " << int(t);
    } else {
      EXPECT_EQ(info.flags & pkt::kIrapMask, 0) << "NAL " << int(t);
    }

    /// Parameter sets: strictly 32 (VPS), 33 (SPS), 34 (PPS)
    if (t >= 32 && t <= 34) {
      EXPECT_NE(info.flags & pkt::kParamSet, 0) << "NAL " << int(t);
    } else {
      EXPECT_EQ(info.flags & pkt::kParamSet, 0) << "NAL " << int(t);
    }
  }

  /// Compatibility wrapper check for NAL 9 (`RASL_R`) vs NAL 7 (`RADL_R`)
  const auto rasl9 = HevcSynth::make_rasl_r(16);
  const auto radl7 = HevcSynth::make_radl_r(16);
  EXPECT_EQ(HevcRapClassifier::classify(rasl9.data(), rasl9.size()), HevcNalType::RASL);
  EXPECT_EQ(HevcRapClassifier::classify(radl7.data(), radl7.size()), HevcNalType::RADL);
}

/// ============================================================================
/// 2. RFC 7798 Payload Classifier: Single-NAL, AP (48), FU (49), PACI (50), DON
/// ============================================================================

TEST(HevcClassifierTest, SingleNalAndTemporalIdDecoding) {
  /// H.265 TID on wire is `nuh_temporal_id_plus1`, so wire tid=3 -> decoded tid=2
  const auto tsa = HevcSynth::make_tsa_r(32, /*wire tid+1*/ 3);
  const auto cls = hevc::classify(tsa.data(), tsa.size(), false);
  EXPECT_FALSE(cls.malformed);
  EXPECT_TRUE(cls.vcl);
  EXPECT_TRUE(cls.switch_up);
  EXPECT_EQ(cls.tid, 2u);
  EXPECT_NE(cls.flags & pkt::kRef, 0);

  /// Non-base layer (`LayerId = 3`) is forwarded opaquely (`kRef`, `nonbase_layer = true`)
  const auto shvc = HevcSynth::make_nal(19 /* IDR */, 24, /*tid+1*/ 1, /*layer_id*/ 3);
  const auto scls = hevc::classify(shvc.data(), shvc.size(), false);
  EXPECT_FALSE(scls.malformed);
  EXPECT_TRUE(scls.nonbase_layer);
  EXPECT_EQ(scls.flags, pkt::kRef);
}

TEST(HevcClassifierTest, AggregationPacketWithAndWithoutDon) {
  const auto vps = HevcSynth::make_vps();
  const auto sps = HevcSynth::make_sps();
  const auto pps = HevcSynth::make_pps();
  const auto idr = HevcSynth::make_idr(48);

  /// Standard AP (`Type = 48`) bundling VPS + SPS + PPS + IDR
  const auto ap  = HevcSynth::make_ap({vps, sps, pps, idr}, /*tid+1*/ 1, /*with_don*/ false);
  const auto cls = hevc::classify(ap.data(), ap.size(), /*don*/ false);
  EXPECT_FALSE(cls.malformed);
  EXPECT_TRUE(cls.vcl);
  EXPECT_EQ(cls.ps_mask, 0x07u); //< VPS(1) | SPS(2) | PPS(4)
  EXPECT_NE(cls.flags & pkt::kClosedIrap, 0);
  EXPECT_NE(cls.flags & pkt::kParamSet, 0);
  EXPECT_EQ(cls.tid, 0u);

  /// AP with 16-bit DONL on unit 0 and 8-bit DOND on units 1..3 (`sprop-max-don-diff > 0`)
  const auto ap_don  = HevcSynth::make_ap({vps, sps, pps, idr}, 1, /*with_don*/ true, 500);
  const auto cls_don = hevc::classify(ap_don.data(), ap_don.size(), /*don*/ true);
  EXPECT_FALSE(cls_don.malformed);
  EXPECT_EQ(cls_don.ps_mask, 0x07u);
  EXPECT_NE(cls_don.flags & pkt::kClosedIrap, 0);
}

TEST(HevcClassifierTest, FragmentationUnitMarksIrapStrictlyOnStartFragment) {
  const auto cra   = HevcSynth::make_cra(3000);
  const auto frags = HevcSynth::fragment_fu(cra, /*max_chunk*/ 1000, /*with_donl*/ true, 77);
  ASSERT_EQ(frags.size(), 3u);

  /// Start fragment (`S=1, E=0`): carries `kOpenIrap | kRef`
  const auto c0 = hevc::classify(frags[0].data(), frags[0].size(), /*don*/ true);
  EXPECT_FALSE(c0.malformed);
  EXPECT_TRUE(c0.is_fu);
  EXPECT_TRUE(c0.fu_start);
  EXPECT_FALSE(c0.fu_end);
  EXPECT_NE(c0.flags & pkt::kOpenIrap, 0);
  EXPECT_NE(c0.flags & pkt::kRef, 0);

  /// Middle fragment (`S=0, E=0`): `kRef` preserved, `kIrapMask` cleared!
  const auto c1 = hevc::classify(frags[1].data(), frags[1].size(), /*don*/ true);
  EXPECT_FALSE(c1.malformed);
  EXPECT_TRUE(c1.is_fu);
  EXPECT_FALSE(c1.fu_start);
  EXPECT_FALSE(c1.fu_end);
  EXPECT_EQ(c1.flags & pkt::kIrapMask, 0);
  EXPECT_NE(c1.flags & pkt::kRef, 0);

  /// End fragment (`S=0, E=1`): `kRef` preserved, `kIrapMask` cleared!
  const auto c2 = hevc::classify(frags[2].data(), frags[2].size(), /*don*/ true);
  EXPECT_FALSE(c2.malformed);
  EXPECT_TRUE(c2.is_fu);
  EXPECT_FALSE(c2.fu_start);
  EXPECT_TRUE(c2.fu_end);
  EXPECT_EQ(c2.flags & pkt::kIrapMask, 0);
  EXPECT_NE(c2.flags & pkt::kRef, 0);
}

TEST(HevcClassifierTest, PaciUnwrapsInnerSingleApAndFu) {
  const auto idr      = HevcSynth::make_idr(40);
  const auto idr_body = std::vector<uint8_t>(idr.begin() + 2, idr.end());
  const auto paci     = HevcSynth::wrap_paci(idr_body, /*ctype*/ 19, {0xDE, 0xAD}, /*tid+1*/ 1);

  const auto cls = hevc::classify(paci.data(), paci.size(), false);
  EXPECT_FALSE(cls.malformed);
  EXPECT_TRUE(cls.vcl);
  EXPECT_NE(cls.flags & pkt::kClosedIrap, 0);
}

TEST(HevcClassifierTest, MalformedPacketsDetectedAndDiagnosticReasonsReported) {
  /// 1. Forbidden zero bit F=1
  const auto bad_f = HevcSynth::make_nal(1, 16, 1, 0, /*forbidden_f*/ true);
  auto       r     = hevc::classify(bad_f.data(), bad_f.size());
  EXPECT_TRUE(r.malformed);
  EXPECT_EQ(r.reason, hevc::MalformedReason::BadNalHeader);

  /// 2. TID == 0 (`nuh_temporal_id_plus1 == 0` is illegal in H.265)
  const auto bad_tid = HevcSynth::make_nal(1, 16, /*tid*/ 0);
  r                  = hevc::classify(bad_tid.data(), bad_tid.size());
  EXPECT_TRUE(r.malformed);
  EXPECT_EQ(r.reason, hevc::MalformedReason::BadNalHeader);

  /// 3. AP with < 2 NAL units
  const auto single_ap = HevcSynth::make_ap({HevcSynth::make_vps()});
  r                    = hevc::classify(single_ap.data(), single_ap.size());
  EXPECT_TRUE(r.malformed);
  EXPECT_EQ(r.reason, hevc::MalformedReason::BadAp);

  /// 4. Nested AP inside AP (`sub_hdr.type == 48`)
  const auto valid_ap  = HevcSynth::make_ap({HevcSynth::make_vps(), HevcSynth::make_pps()});
  const auto nested_ap = HevcSynth::make_ap({HevcSynth::make_vps(), valid_ap});
  r                    = hevc::classify(nested_ap.data(), nested_ap.size());
  EXPECT_TRUE(r.malformed);
  EXPECT_EQ(r.reason, hevc::MalformedReason::BadAp);

  /// 5. FU with both S=1 and E=1 simultaneously
  auto fu_se = HevcSynth::make_nal(49, 8, 1);
  fu_se[2]   = static_cast<uint8_t>(0x80U | 0x40U | 19U); //< S=1, E=1, FuType=19
  r          = hevc::classify(fu_se.data(), fu_se.size());
  EXPECT_TRUE(r.malformed);
  EXPECT_EQ(r.reason, hevc::MalformedReason::BadFu);

  /// 6. FU with FuType >= 48
  auto fu_bad_type = HevcSynth::make_nal(49, 8, 1);
  fu_bad_type[2]   = static_cast<uint8_t>(0x80U | 48U);
  r                = hevc::classify(fu_bad_type.data(), fu_bad_type.size());
  EXPECT_TRUE(r.malformed);
  EXPECT_EQ(r.reason, hevc::MalformedReason::BadFu);

  /// 7. PACI with A=1 or oversized PHSsize
  const auto paci_a1 = HevcSynth::wrap_paci({0x01, 0x02}, 1, {0xAA}, 1, /*a_bit*/ true);
  r                  = hevc::classify(paci_a1.data(), paci_a1.size());
  EXPECT_TRUE(r.malformed);
  EXPECT_EQ(r.reason, hevc::MalformedReason::BadPaci);

  auto paci_trunc = HevcSynth::wrap_paci({0x01}, 1, {0x11, 0x22, 0x33, 0x44}, 1, false);
  paci_trunc.resize(5); //< Shorter than 4 + PHSsize (8)
  r = hevc::classify(paci_trunc.data(), paci_trunc.size());
  EXPECT_TRUE(r.malformed);
  EXPECT_EQ(r.reason, hevc::MalformedReason::BadPaci);
}

/// ============================================================================
/// 3. SPS / VPS / PPS Parsers (Emulation Prevention) & `FuParamReassembler`
/// ============================================================================

TEST(HevcParamSetTest, ParseSpsPtlStripsEmulationPreventionBytesBeforeLevelId) {
  /// `force_epb = true` injects `0x00 00 03` inside profile_compatibility/constraint flags
  /// right before `general_level_idc`!
  const auto sps = HevcSynth::make_sps(/*sps_id*/ 3,
      /*vps_id*/ 1,
      /*max_sub_layers*/ 3,
      /*profile_space*/ 0,
      /*tier_flag*/ 1,
      /*profile_id*/ 2 /* Main 10 */,
      /*level_id*/ 153 /* Level 5.1 */,
      /*force_epb*/ true);

  /// Verify an actual `0x00 00 03` byte triple exists on the wire
  bool found_epb = false;
  for (size_t i = 2; i + 2 < sps.size(); ++i) {
    if (sps[i] == 0x00 && sps[i + 1] == 0x00 && sps[i + 2] == 0x03) {
      found_epb = true;
      break;
    }
  }
  EXPECT_TRUE(found_epb);

  const auto ptl = hevc::parse_sps_ptl(sps);
  ASSERT_TRUE(ptl.has_value());
  EXPECT_EQ(ptl->profile_space, 0u);
  EXPECT_EQ(ptl->tier_flag, 1u);
  EXPECT_EQ(ptl->profile_idc, 2u);
  EXPECT_EQ(ptl->level_idc, 153u);
  EXPECT_EQ(ptl->max_sub_layers, 3u);
  EXPECT_EQ(ptl->sps_id, 3u);

  const auto vps      = HevcSynth::make_vps(/*vps_id*/ 1, /*max_sub_layers*/ 3, 2, 1, 153);
  const auto vps_info = hevc::parse_vps_info(vps);
  ASSERT_TRUE(vps_info.has_value());
  EXPECT_EQ(vps_info->vps_id, 1u);
  EXPECT_EQ(vps_info->ptl.profile_idc, 2u);
  EXPECT_EQ(vps_info->ptl.level_idc, 153u);

  const auto pps    = HevcSynth::make_pps(/*pps_id*/ 17, /*sps_id*/ 3);
  const auto pps_id = hevc::parse_pps_id(pps);
  ASSERT_TRUE(pps_id.has_value());
  EXPECT_EQ(*pps_id, 17u);
}

TEST(HevcParamSetTest, FuParamReassemblerAssemblesUnder4KiBAndDiscardsOnGapOrOverflow) {
  hevc::FuParamReassembler reasm;

  /// Build a ~2.4 KiB SPS by padding trailing RBSP bytes
  auto big_sps = HevcSynth::make_sps(2, 0, 2, 0, 0, 1, 120, true);
  big_sps.resize(2400, 0x55);

  const auto frags = HevcSynth::fragment_fu(big_sps, /*max_chunk*/ 900);
  ASSERT_EQ(frags.size(), 3u);

  std::vector<uint8_t> assembled;
  uint8_t              assembled_type = 0;
  uint16_t             seq            = 100;
  for (const auto &f : frags) {
    EXPECT_TRUE(reasm.on_fu_packet(
        f.data(), f.size(), seq++, false, [&](uint8_t t, const uint8_t *ptr, size_t len) {
          assembled_type = t;
          assembled.assign(ptr, ptr + len);
        }));
  }
  EXPECT_EQ(assembled_type, 33u);
  EXPECT_EQ(assembled, big_sps);

  /// Sequence gap (`100`, skip `101`, feed `102`) discards cleanly (`returns false`)
  assembled.clear();
  EXPECT_TRUE(reasm.on_fu_packet(
      frags[0].data(), frags[0].size(), 200, false, [&](uint8_t, const uint8_t *, size_t) {
        FAIL() << "Must not complete on start frag";
      }));
  EXPECT_FALSE(reasm.on_fu_packet(frags[2].data(),
      frags[2].size(),
      202 /* gap! */,
      false,
      [&](uint8_t, const uint8_t *, size_t) { FAIL() << "Must discard on sequence gap"; }));

  /// > 4 KiB oversized parameter set discards cleanly (`returns false`)
  auto huge_sps = HevcSynth::make_sps();
  huge_sps.resize(5000, 0xAA);
  const auto huge_frags = HevcSynth::fragment_fu(huge_sps, 1400);
  ASSERT_GE(huge_frags.size(), 4u);
  bool     overflow_detected = false;
  uint16_t hseq              = 300;
  for (const auto &f : huge_frags) {
    if (!reasm.on_fu_packet(
            f.data(), f.size(), hseq++, false, [&](uint8_t, const uint8_t *, size_t) {
              FAIL() << "Must not emit > 4 KiB SPS";
            })) {
      overflow_detected = true;
      break;
    }
  }
  EXPECT_TRUE(overflow_detected);
}

/// ============================================================================
/// 4. Open-GOP `CRA` Startup + `RASL` Gate
/// ============================================================================

TEST(HevcStartupAndGateTest, OpenGopCraStartupArmsRaslGateAndDropsLeadingRaslUntilTrail) {
  NumaArena         arena(32, 256);
  ArenaLeakGuard    leak_guard(arena);
  RetainedPacketLog log;

  TrackDescriptor hevc_track{};
  hevc_track.index = 0;
  hevc_track.kind  = MediaKind::Video;
  hevc_track.codec = VideoCodec::H265;

  auto push_au = [&](uint16_t seq, uint32_t ts, const std::vector<uint8_t> &nal) {
    const auto wire = RtpBuilder().seq(seq).timestamp(ts).marker(true).payload(nal).build();
    auto      *slab = arena.allocate();
    std::memcpy(slab->payload_data, wire.data(), wire.size());
    const auto info =
        rivet::proto::rtp::RtpPacketClassifier::classify(slab->u8_data(), wire.size(), hevc_track);
    auto pkt = PacketRef::adopt(slab,
        0,
        static_cast<uint16_t>(wire.size()),
        0,
        ts,
        seq,
        info.packed_track_meta,
        static_cast<uint8_t>(info.flags | pkt::kAuStart));
    log.append(std::move(pkt));
    return log.ring().try_get(log.ring().write_pos() - 1).value();
  };

  /// Stream order after Open-GOP CRA: CRA (ts=90000) -> RASL_R (ts=84000) -> RADL_R (ts=87000) ->
  /// TRAIL_R (ts=93000)
  const auto cra_pkt   = push_au(1, 90000, HevcSynth::make_cra(64));
  const auto rasl_pkt  = push_au(2, 84000, HevcSynth::make_rasl_r(32));
  const auto radl_pkt  = push_au(3, 87000, HevcSynth::make_radl_r(32));
  const auto trail_pkt = push_au(4, 93000, HevcSynth::make_trail_r(32));
  /// Subsequent RASL in the NEXT Open-GOP (when viewer already has prior GOP!) MUST be kept!
  const auto next_cra  = push_au(5, 180000, HevcSynth::make_cra(64));
  const auto next_rasl = push_au(6, 174000, HevcSynth::make_rasl_r(32));

  FastPictureFirstPolicy policy;
  /// When `allow_open_irap = false`, no closed IDR exists -> falls back to `FreshnessFirst`!
  const auto closed_only_plan =
      policy.plan_cursor(log.ring(), 0, 0, 4 * 1024 * 1024, /*allow_open_irap*/ false);
  EXPECT_FALSE(closed_only_plan.has_rap);
  EXPECT_TRUE(closed_only_plan.wait_irap);
  EXPECT_TRUE(closed_only_plan.request_pli);

  /// When `allow_open_irap = true`, selects latest CRA (`ts=180000`) and arms `rasl_gate = true`!
  const auto open_plan =
      policy.plan_cursor(log.ring(), 0, 0, 4 * 1024 * 1024, /*allow_open_irap*/ true);
  EXPECT_TRUE(open_plan.has_rap);
  EXPECT_TRUE(open_plan.rasl_gate);
  EXPECT_FALSE(open_plan.wait_irap);

  /// Simulate AuGate starting at the first CRA join (`wait_irap = true` initially):
  DependencyDropPolicy drop;
  AuGate               gate;
  gate.reset(/*tid_ceiling*/ 7, /*start_wait_irap*/ true, /*start_rasl_gate*/ false);

  /// 1. CRA admitted, `rasl_gate` armed because `was_missing_prior_gop == true`
  auto r_cra = drop.admit(gate, cra_pkt, 10, 0, 1000, true);
  EXPECT_EQ(r_cra.decision, DropDecision::Keep);
  EXPECT_TRUE(gate.rasl_gate);

  /// 2. Leading RASL_R dropped (`dropped_rasl = true`)!
  auto r_rasl = drop.admit(gate, rasl_pkt, 10, 0, 2000, true);
  EXPECT_EQ(r_rasl.decision, DropDecision::Drop);
  EXPECT_TRUE(r_rasl.dropped_rasl);
  EXPECT_TRUE(gate.rasl_gate);

  /// 3. Leading RADL_R does NOT reference prior GOP -> kept!
  auto r_radl = drop.admit(gate, radl_pkt, 10, 0, 3000, true);
  EXPECT_EQ(r_radl.decision, DropDecision::Keep);

  /// 4. First trailing picture `TRAIL_R` clears `rasl_gate`!
  auto r_trail = drop.admit(gate, trail_pkt, 10, 0, 4000, true);
  EXPECT_EQ(r_trail.decision, DropDecision::Keep);
  EXPECT_FALSE(gate.rasl_gate);

  /// 5. Next normal Open-GOP CRA (with prior GOP intact!) keeps `rasl_gate == false`, so its RASL
  /// is KEPT!
  auto r_cra2 = drop.admit(gate, next_cra, 10, 0, 5000, true);
  EXPECT_EQ(r_cra2.decision, DropDecision::Keep);
  EXPECT_FALSE(gate.rasl_gate);

  auto r_rasl2 = drop.admit(gate, next_rasl, 10, 0, 6000, true);
  EXPECT_EQ(r_rasl2.decision, DropDecision::Keep);
}

/// ============================================================================
/// 5. SIMD vs Scalar SubscriberTable Eligibility Equivalence
/// ============================================================================

TEST(SubscriberTableSimdTest, BitExactEquivalenceAcross0To1025Subscribers) {
  std::mt19937 rng(0x52495654U);

  for (size_t count : {size_t{0},
           size_t{1},
           size_t{7},
           size_t{15},
           size_t{16},
           size_t{17},
           size_t{31},
           size_t{32},
           size_t{33},
           size_t{63},
           size_t{64},
           size_t{255},
           size_t{1025}}) {
    std::vector<uint8_t>  active(count);
    std::vector<uint8_t>  fill(count);
    std::vector<uint32_t> out_scalar(count + 1, 0xFFFFFFFFU);
    std::vector<uint32_t> out_simd(count + 1, 0xEEEEEEEEU);

    for (size_t i = 0; i < count; ++i) {
      active[i] = static_cast<uint8_t>(rng() % 3 == 0 ? 0 : 1);
      fill[i]   = static_cast<uint8_t>(rng() % 105);
    }

    for (uint8_t limit :
        {uint8_t{1}, uint8_t{60}, uint8_t{75}, uint8_t{90}, uint8_t{95}, uint8_t{100}}) {
      const size_t n_scalar = rivet::core::SubscriberTable::filter_eligible_scalar(
          active.data(), fill.data(), count, limit, out_scalar.data());
      const size_t n_simd = rivet::core::SubscriberTable::filter_eligible_simd(
          active.data(), fill.data(), count, limit, out_simd.data());

      ASSERT_EQ(n_simd, n_scalar) << "count=" << count << " limit=" << int(limit);
      for (size_t k = 0; k < n_scalar; ++k) {
        ASSERT_EQ(out_simd[k], out_scalar[k]) << "idx=" << k << " count=" << count;
      }
    }
  }
}

/// ============================================================================
/// 6. 1,000-Random-GOP `AuGate` Property Test
///    Asserts ZERO torn AUs and ZERO missing DPB / TemporalId references!
/// ============================================================================

TEST(HevcAuGatePropertyTest, ThousandRandomGopsPreserveWholeAuAndDpbReferenceIntegrity) {
  NumaArena            arena(64, 256);
  ArenaLeakGuard       leak_guard(arena);
  DependencyDropPolicy policy;
  std::mt19937         rng(0xC0DEC265U);

  enum class SynthPicKind { ClosedIdr, OpenCra, RaslR, RadlR, TrailR, TrailN, TsaR };

  auto *dummy_slab = arena.allocate();
  ASSERT_NE(dummy_slab, nullptr);
  std::memset(dummy_slab->payload_data, 0x42, 128);

  uint16_t seq    = 1;
  uint32_t rtp_ts = 90000;
  uint64_t now_ns = 1'000'000'000ULL;

  AuGate            gate;
  constexpr uint8_t kMaxTid = 2; //< 3 temporal layers: TID 0, 1, 2
  gate.reset(kMaxTid, /*start_wait_irap*/ true, false);

  /// Reference decoder validity model:
  /// `dpb_valid[t]` is true iff every reference picture required to decode a picture at `TID == t`
  /// has been delivered intact since the most recent IRAP (or TSA switch-up).
  std::array<bool, 3> dpb_valid{false, false, false};
  bool                prior_gop_intact = false;

  for (int gop = 0; gop < 1000; ++gop) {
    const bool         open_gop   = (gop > 0) && ((rng() % 3) == 0);
    const SynthPicKind irap_kind  = open_gop ? SynthPicKind::OpenCra : SynthPicKind::ClosedIdr;
    const int          gop_frames = 6 + static_cast<int>(rng() % 10);

    for (int f = 0; f < gop_frames; ++f) {
      rtp_ts += 3000;
      now_ns += 33'333'333ULL; //< ~30 fps (allows 500 ms TID hysteresis to elapse naturally)

      SynthPicKind kind = irap_kind;
      uint8_t      tid  = 0;
      if (f == 0) {
        kind = irap_kind;
        tid  = 0;
      } else if (open_gop && f == 1) {
        kind = SynthPicKind::RaslR;
        tid  = 0;
      } else if (open_gop && f == 2) {
        kind = SynthPicKind::RadlR;
        tid  = 0;
      } else {
        const uint32_t pick = rng() % 10;
        if (pick < 3) {
          kind = SynthPicKind::TrailR;
          tid  = 0;
        } else if (pick < 5) {
          kind = SynthPicKind::TrailR;
          tid  = 1;
        } else if (pick < 7) {
          kind = SynthPicKind::TsaR;
          tid  = static_cast<uint8_t>(1 + (rng() % 2)); //< TID 1 or 2 switch-up
        } else {
          kind = SynthPicKind::TrailN;
          tid  = static_cast<uint8_t>(rng() % 3); //< Disposable non-ref at TID 0..2
        }
      }

      /// Choose fill_pct in [0..104] for this AU (held constant across the packets of this AU
      /// except when testing 100%+ hard disconnect reset).
      const uint8_t fill_pct = static_cast<uint8_t>(rng() % 103);
      if (fill_pct >= 100) {
        /// Simulate subscriber disconnect & fresh reconnect waiting for next IRAP
        dummy_slab->retain();
        auto       p = PacketRef::adopt(dummy_slab,
            0,
            64,
            12,
            rtp_ts,
            seq++,
            pkt::pack_track_meta(0, tid, true, false),
            pkt::kRef | pkt::kAuStart);
        const auto d = policy.admit(gate, p, fill_pct, kMaxTid, now_ns, true);
        ASSERT_EQ(d.decision, DropDecision::Disconnect);
        gate.reset(kMaxTid, /*start_wait_irap*/ true, false);
        dpb_valid        = {false, false, false};
        prior_gop_intact = false;
        continue;
      }

      /// Optionally prepend an in-band VPS/SPS/PPS parameter-set packet at the same timestamp
      const bool has_prefix_ps = ((rng() % 4) == 0);
      if (has_prefix_ps) {
        dummy_slab->retain();
        auto       ps_pkt = PacketRef::adopt(dummy_slab,
            0,
            48,
            12,
            rtp_ts,
            seq++,
            pkt::pack_track_meta(0, 7, false, false),
            pkt::kParamSet | pkt::kAuStart);
        const auto ps_res = policy.admit(gate, ps_pkt, fill_pct, kMaxTid, now_ns, true);
        ASSERT_EQ(ps_res.decision, DropDecision::Keep) << "Parameter set dropped below 100% fill!";
      }

      /// Build 1..5 VCL fragments for this AU
      const int num_frags  = 1 + static_cast<int>(rng() % 5);
      uint8_t   base_flags = 0;
      bool      is_ref_pic = false;
      bool      is_switch  = false;
      switch (kind) {
        case SynthPicKind::ClosedIdr:
          base_flags |= pkt::kClosedIrap | pkt::kRef;
          is_ref_pic = true;
          break;
        case SynthPicKind::OpenCra:
          base_flags |= pkt::kOpenIrap | pkt::kRef;
          is_ref_pic = true;
          break;
        case SynthPicKind::RaslR:
          base_flags |= pkt::kRasl | pkt::kRef;
          is_ref_pic = true;
          break;
        case SynthPicKind::RadlR:
          base_flags |= pkt::kRadl | pkt::kRef;
          is_ref_pic = true;
          break;
        case SynthPicKind::TrailR:
          base_flags |= pkt::kRef;
          is_ref_pic = true;
          break;
        case SynthPicKind::TrailN:
          is_ref_pic = false;
          break;
        case SynthPicKind::TsaR:
          base_flags |= pkt::kRef;
          is_ref_pic = true;
          is_switch  = true;
          break;
      }

      DropDecision first_decision = DropDecision::Keep;
      for (int frag = 0; frag < num_frags; ++frag) {
        uint8_t f_flags = base_flags;
        if (frag > 0) {
          /// Subsequent FU fragments clear kIrapMask on wire
          f_flags = static_cast<uint8_t>(f_flags & ~pkt::kIrapMask);
        }
        if (frag == 0 && !has_prefix_ps) {
          f_flags |= pkt::kAuStart;
        }
        if (frag + 1 == num_frags) {
          f_flags |= pkt::kMarker;
        }

        dummy_slab->retain();
        auto vcl_pkt = PacketRef::adopt(dummy_slab,
            0,
            100,
            12,
            rtp_ts,
            seq++,
            pkt::pack_track_meta(0, tid, true, is_switch),
            f_flags);

        const auto res = policy.admit(gate, vcl_pkt, fill_pct, kMaxTid, now_ns, true);
        if (frag == 0) {
          first_decision = res.decision;
        } else {
          /// INVARIANT 1: Whole-AU atomicity — ZERO torn AUs!
          ASSERT_EQ(res.decision, first_decision)
              << "Torn AU detected at gop=" << gop << " frame=" << f << " frag=" << frag;
        }
      }

      /// INVARIANT 2: DPB & Temporal Sub-Layer Reference Integrity!
      if (first_decision == DropDecision::Keep) {
        if (kind == SynthPicKind::ClosedIdr) {
          dpb_valid        = {true, true, true};
          prior_gop_intact = true;
        } else if (kind == SynthPicKind::OpenCra) {
          const bool had_full_prior_gop = dpb_valid[0] && dpb_valid[1] && dpb_valid[2];
          prior_gop_intact              = had_full_prior_gop;
          dpb_valid                     = {true, true, true};
        } else if (kind == SynthPicKind::RaslR) {
          ASSERT_TRUE(prior_gop_intact)
              << "RASL picture admitted without intact prior GOP at gop=" << gop;
        } else if (kind == SynthPicKind::RadlR) {
          ASSERT_TRUE(dpb_valid[0]) << "RADL picture admitted without valid IRAP at gop=" << gop;
        } else if (kind == SynthPicKind::TsaR) {
          ASSERT_GT(tid, 0u);
          ASSERT_TRUE(dpb_valid[tid - 1])
              << "TSA switch-up admitted without valid lower temporal layer at gop=" << gop;
          dpb_valid[tid] = true;
          for (uint8_t hi = static_cast<uint8_t>(tid + 1); hi <= kMaxTid; ++hi) {
            /// Higher layers above TSA still require their own TSA or IRAP
          }
        } else {
          /// TRAIL_R or TRAIL_N at `tid`
          for (uint8_t req = 0; req <= tid; ++req) {
            ASSERT_TRUE(dpb_valid[req])
                << "Delta picture at tid=" << int(tid) << " admitted with broken DPB at layer "
                << int(req) << " (gop=" << gop << ", frame=" << f << ")";
          }
        }
      } else {
        /// AU was dropped: update reference decoder validity model
        if (kind == SynthPicKind::ClosedIdr || kind == SynthPicKind::OpenCra) {
          dpb_valid        = {false, false, false};
          prior_gop_intact = false;
        } else if (is_ref_pic && kind != SynthPicKind::RaslR && kind != SynthPicKind::RadlR) {
          for (uint8_t hi = tid; hi <= kMaxTid; ++hi) {
            dpb_valid[hi] = false;
          }
          prior_gop_intact = false;
        }
      }
    }
  }

  dummy_slab->release();
}
