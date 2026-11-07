#include "rivet/core/IngestReactor.hpp"
#include "rivet/media/AccessUnitView.hpp"
#include "rivet/media/AuAssembler.hpp"
#include "rivet/media/ConfigGeneration.hpp"
#include "rivet/media/DependencyDropPolicy.hpp"
#include "rivet/media/H264RapClassifier.hpp"
#include "rivet/media/HevcRapClassifier.hpp"
#include "rivet/media/RetainedPacketLog.hpp"
#include "rivet/media/StartupPolicy.hpp"
#include "rivet/memory/NumaArena.hpp"
#include "rivet/memory/PacketFlags.hpp"
#include "rivet/proto/rtp/RtpPacketClassifier.hpp"
#include "tests/support/ArenaLeakGuard.hpp"
#include "tests/support/RtpBuilder.hpp"
#include <cstring>
#include <gtest/gtest.h>

using namespace rivet::media;
using namespace rivet::memory;

/// ============================================================================
/// H264RapClassifier Tests
/// ============================================================================

TEST(H264RapClassifierTest, ClassifyNalTypes) {
  uint8_t short_p[1] = {0x00};
  EXPECT_EQ(H264RapClassifier::classify(short_p, 0), H264NalType::Regular);

  uint8_t idr_pkt[] = {0x65, 0x88, 0x80};
  EXPECT_EQ(H264RapClassifier::classify(idr_pkt, sizeof(idr_pkt)), H264NalType::IDR);

  uint8_t sps_pkt[] = {0x67, 0x42, 0xC0, 0x1F};
  EXPECT_EQ(H264RapClassifier::classify(sps_pkt, sizeof(sps_pkt)), H264NalType::SPS);

  uint8_t pps_pkt[] = {0x68, 0xCE, 0x01};
  EXPECT_EQ(H264RapClassifier::classify(pps_pkt, sizeof(pps_pkt)), H264NalType::PPS);

  uint8_t p_pkt[] = {0x41, 0x9A};
  EXPECT_EQ(H264RapClassifier::classify(p_pkt, sizeof(p_pkt)), H264NalType::Regular);

  /// H.264 NRI=2 Type 5 (0x45) and Type 2 (0x42) MUST NOT be
  /// misclassified as HEVC PPS/SPS!
  uint8_t idr_nri2[] = {0x45, 0x88, 0x80};
  EXPECT_EQ(H264RapClassifier::classify(idr_nri2, sizeof(idr_nri2)), H264NalType::IDR);
  uint8_t dpa_nri2[] = {0x42, 0x11, 0x22};
  EXPECT_EQ(H264RapClassifier::classify(dpa_nri2, sizeof(dpa_nri2)), H264NalType::Regular);

  uint8_t fua_idr[] = {0x7C, 0x85, 0x12};
  EXPECT_EQ(H264RapClassifier::classify(fua_idr, sizeof(fua_idr)), H264NalType::IDR);

  uint8_t stap_a[] = {0x78, 0x00, 0x03, 0x67, 0x42, 0x00};
  EXPECT_EQ(H264RapClassifier::classify(stap_a, sizeof(stap_a)), H264NalType::STAP_A);
}

TEST(H264RapClassifierTest, ForEachParameterSetStapA) {
  uint8_t stap_a[] = {0x78, 0x00, 0x03, 0x67, 0x42, 0x00, 0x00, 0x03, 0x68, 0xCE, 0x01};

  std::vector<H264NalType> found_types;
  std::vector<size_t>      found_sizes;

  H264RapClassifier::for_each_parameter_set(
      stap_a, sizeof(stap_a), [&](H264NalType type, const uint8_t *, size_t len) {
        found_types.push_back(type);
        found_sizes.push_back(len);
      });

  ASSERT_EQ(found_types.size(), 2u);
  EXPECT_EQ(found_types[0], H264NalType::SPS);
  EXPECT_EQ(found_sizes[0], 3u);
  EXPECT_EQ(found_types[1], H264NalType::PPS);
  EXPECT_EQ(found_sizes[1], 3u);
}

/// ============================================================================
/// HevcRapClassifier Tests (NAL 9 == RASL)
/// ============================================================================

TEST(HevcRapClassifierTest, ClassifyNalTypes) {
  uint8_t short_p[1] = {0x00};
  EXPECT_EQ(HevcRapClassifier::classify(short_p, 1), HevcNalType::Regular);

  auto make_hevc = [](uint8_t type) -> std::vector<uint8_t> {
    return {static_cast<uint8_t>((type & 0x3F) << 1), 0x01};
  };

  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(6).data(), 2), HevcNalType::RADL);
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(7).data(), 2), HevcNalType::RADL);
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(8).data(), 2), HevcNalType::RASL);
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(9).data(), 2), HevcNalType::RASL);
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(19).data(), 2), HevcNalType::IDR);
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(20).data(), 2), HevcNalType::IDR);
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(21).data(), 2), HevcNalType::CRA);
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(32).data(), 2), HevcNalType::ParameterSet);
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(33).data(), 2), HevcNalType::ParameterSet);
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(34).data(), 2), HevcNalType::ParameterSet);
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(1).data(), 2), HevcNalType::Regular);
}

/// ============================================================================
/// AccessUnitView & AuAssembler Tests
/// ============================================================================

TEST(AccessUnitViewTest, BeginAndAppend) {
  NumaArena                   arena(4, 256);
  rivet::test::ArenaLeakGuard guard(arena);
  auto                       *slab = arena.allocate();

  AccessUnitView au;
  EXPECT_TRUE(au.is_empty());

  au.begin(100, 90000, DependencyPriority::Critical);
  EXPECT_EQ(au.get_start_sequence(), 100u);
  EXPECT_EQ(au.get_rtp_timestamp(), 90000u);
  EXPECT_EQ(au.get_priority(), DependencyPriority::Critical);

  auto pkt = PacketRef::adopt(slab, 0, 50, 0, 90000, 100, 0, 0);
  au.append(std::move(pkt));

  EXPECT_FALSE(au.is_empty());
  EXPECT_EQ(au.packet_count(), 1u);
}

TEST(AuAssemblerTest, MultiPacketAuUpgradesPriorityAndDetectsIntraAuGap) {
  NumaArena                   arena(8, 256);
  rivet::test::ArenaLeakGuard guard(arena);

  AuAssembler assembler;
  /// First packet in AU is SPS/PPS (Reference, non-keyframe); second packet is IDR (Critical)!
  auto p_sps =
      PacketRef::adopt(arena.allocate(), 0, 20, 0, 90000, 10, 0, pkt::kParamSet | pkt::kRef);
  auto p_idr = PacketRef::adopt(
      arena.allocate(), 0, 60, 0, 90000, 11, 0, pkt::kClosedIrap | pkt::kRef | pkt::kMarker);

  EXPECT_FALSE(assembler.on_packet(std::move(p_sps), false).has_value());
  auto complete = assembler.on_packet(std::move(p_idr), true);
  ASSERT_TRUE(complete.has_value());
  EXPECT_EQ(complete->packet_count(), 2u);
  /// Priority MUST upgrade from Reference -> Critical across all packets of the AU!
  EXPECT_EQ(complete->get_priority(), DependencyPriority::Critical);

  /// Intra-AU sequence gap (`seq 20` then `seq 22` skipping `21`) MUST poison/discard torn AU!
  auto g1 = PacketRef::adopt(arena.allocate(), 0, 20, 0, 93000, 20, 0, pkt::kRef);
  auto g2 = PacketRef::adopt(arena.allocate(), 0, 20, 0, 93000, 22, 0, pkt::kRef | pkt::kMarker);
  EXPECT_FALSE(assembler.on_packet(std::move(g1), false).has_value());
  EXPECT_FALSE(assembler.on_packet(std::move(g2), true).has_value());
}

TEST(AuAssemblerTest, AbortAuOnTimestampChangeBeforeMarkerWithoutLeakingSlab) {
  NumaArena                   arena(4, 256);
  rivet::test::ArenaLeakGuard guard(arena);

  AuAssembler assembler;
  auto        pkt1 = PacketRef::adopt(arena.allocate(), 0, 20, 0, 90000, 10, 0, pkt::kRef);
  EXPECT_FALSE(assembler.on_packet(std::move(pkt1), false).has_value());

  auto pkt2 = PacketRef::adopt(
      arena.allocate(), 0, 20, 0, 93000, 12, 0, pkt::kClosedIrap | pkt::kRef | pkt::kMarker);
  auto res2 = assembler.on_packet(std::move(pkt2), true);

  ASSERT_TRUE(res2.has_value());
  EXPECT_EQ(res2->get_start_sequence(), 12u);
  EXPECT_EQ(res2->get_rtp_timestamp(), 93000u);
  EXPECT_EQ(res2->get_priority(), DependencyPriority::Critical);
}

/// ============================================================================
/// ConfigTracker & Ingest Parameter-Set Extraction Tests
/// ============================================================================

TEST(ConfigTrackerTest, PerAuStagedCommitAndBounded4SnapshotHistory) {
  ConfigTracker tracker;
  EXPECT_EQ(tracker.current_generation(), 0u);

  /// Stage VPS + SPS + PPS in a single AU -> commit_au() bumps generation ONCE (0 -> 1)!
  uint8_t vps1[] = {0x40, 0x01, 0x0C, 0x01, 0xFF, 0xFF};
  uint8_t sps1[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00};
  uint8_t pps1[] = {0x44, 0x01, 0xC1, 0x73};
  tracker.stage_vps(vps1, sizeof(vps1));
  tracker.stage_sps(sps1, sizeof(sps1));
  tracker.stage_pps(pps1, sizeof(pps1));
  EXPECT_EQ(tracker.commit_au(), 1u);
  EXPECT_EQ(tracker.current_generation(), 1u);

  /// Re-staging identical parameter sets produces NO generation bump
  tracker.stage_vps(vps1, sizeof(vps1));
  tracker.stage_sps(sps1, sizeof(sps1));
  tracker.stage_pps(pps1, sizeof(pps1));
  EXPECT_EQ(tracker.commit_au(), 1u);

  /// Push 5 distinct PPS updates to verify bounded 4-snapshot history ring
  for (uint8_t i = 0; i < 5; ++i) {
    uint8_t pps_mod[] = {0x44, 0x01, 0xC1, static_cast<uint8_t>(0x10U + i)};
    EXPECT_GT(tracker.update_pps(pps_mod, sizeof(pps_mod)), 1u);
  }
  EXPECT_EQ(tracker.current_generation(), 6u);
  /// Oldest generation 1 evicted from 4-slot ring; latest 4 generations (3..6) remain accessible
  EXPECT_FALSE(tracker.get_generation(1).has_value());
  EXPECT_TRUE(tracker.get_generation(6).has_value());
}

TEST(IngestParameterSetRegressionTest, SkipsRtpHeaderAndNeverTreatsH264Idr0x45AsHevcPps) {
  NumaArena                   arena(16, 2048);
  rivet::test::ArenaLeakGuard guard(arena);
  rivet::core::IngestReactor  ingest;

  StreamDescriptor desc;
  TrackDescriptor  vtrk;
  vtrk.index = 0;
  vtrk.kind  = MediaKind::Video;
  vtrk.codec = VideoCodec::H264;
  desc.tracks.push_back(vtrk);
  ingest.set_stream_descriptor(desc);

  auto adopt_rtp = [&](const std::vector<uint8_t> &wire) {
    auto  info = rivet::proto::rtp::RtpPacketClassifier::classify(wire.data(), wire.size(), vtrk);
    auto *slab = arena.allocate();
    std::memcpy(slab->payload_data, wire.data(), wire.size());
    return PacketRef::adopt(slab,
        0,
        static_cast<uint32_t>(wire.size()),
        1000,
        info.timestamp,
        info.seq,
        info.packed_track_meta,
        info.flags);
  };

  /// Packet 1 (seq 10): STAP-A containing real H.264 SPS (0x67 42 C0 1F) and PPS (0x68 CE 3C 80)
  std::vector<uint8_t> stap_a = {
      0x78, 0x00, 0x04, 0x67, 0x42, 0xC0, 0x1F, 0x00, 0x04, 0x68, 0xCE, 0x3C, 0x80};
  auto rtp1 =
      rivet::test::RtpBuilder().seq(10).timestamp(90000).marker(false).payload(stap_a).build();

  /// Packet 2 (seq 11): H.264 IDR slice starting with 0x45 (`NRI=2, Type=5`, which looks like HEVC
  /// NAL 34 PPS `(0x45 >> 1) & 0x3F == 34` if misrouted!)
  std::vector<uint8_t> idr_0x45 = {0x45, 0x88, 0x84, 0x21, 0xAA, 0xBB};
  auto                 rtp2 =
      rivet::test::RtpBuilder().seq(11).timestamp(90000).marker(true).payload(idr_0x45).build();

  (void)ingest.on_upstream_packet(adopt_rtp(rtp1));
  (void)ingest.on_upstream_packet(adopt_rtp(rtp2));
  ingest.clear_batch();

  const auto snap = ingest.get_config_tracker().current_snapshot();
  ASSERT_NE(snap, nullptr);
  EXPECT_EQ(snap->generation_id, 1u);
  const auto expect_sps = std::vector<uint8_t>{0x67, 0x42, 0xC0, 0x1F};
  const auto expect_pps = std::vector<uint8_t>{0x68, 0xCE, 0x3C, 0x80};
  EXPECT_EQ(snap->sps, expect_sps);
  EXPECT_EQ(snap->pps, expect_pps); //< NOT overwritten by 0x45 IDR or 0x80 RTP header!
}

/// ============================================================================
/// DependencyDropPolicy, RetainedPacketLog & StartupPolicy Tests
/// ============================================================================

TEST(DependencyDropPolicyTest, StrictThresholdBoundaries) {
  DependencyDropPolicy policy;

  EXPECT_EQ(policy.evaluate(DependencyPriority::Disposable, 59), DropDecision::Keep);
  EXPECT_EQ(policy.evaluate(DependencyPriority::Disposable, 60), DropDecision::Drop);
  EXPECT_EQ(policy.evaluate(DependencyPriority::Reference, 89), DropDecision::Keep);
  EXPECT_EQ(policy.evaluate(DependencyPriority::Reference, 90), DropDecision::Drop);
  EXPECT_EQ(policy.evaluate(DependencyPriority::Critical, 94), DropDecision::Keep);
  EXPECT_EQ(policy.evaluate(DependencyPriority::Critical, 95), DropDecision::Drop);
  EXPECT_EQ(policy.evaluate(DependencyPriority::Critical, 99), DropDecision::Drop);
  EXPECT_EQ(policy.evaluate(DependencyPriority::Critical, 100), DropDecision::Disconnect);
}

TEST(RetainedPacketLogTest, Large300PacketIdrBurstNotTruncatedAt128) {
  NumaArena                   arena(512, 2048);
  rivet::test::ArenaLeakGuard guard(arena);
  RetainedPacketLog           log(4096, 16ULL * 1024ULL * 1024ULL);

  /// Append a 300-packet fragmented 4K IDR AU (exceeding old 128-packet burst cap!)
  const uint64_t start_pos  = log.ring().write_pos();
  const uint64_t start_byte = log.ring().write_bytes();
  for (uint16_t i = 0; i < 300; ++i) {
    uint8_t fl = pkt::kRef;
    if (i == 0) fl |= (pkt::kClosedIrap | pkt::kAuStart);
    if (i == 299) fl |= pkt::kMarker;
    auto p = PacketRef::adopt(
        arena.allocate(), 0, 1200, 1000, 90000, static_cast<uint16_t>(1000 + i), 0, fl);
    p.generation_id = 1;
    log.ring().append(std::move(p));
  }
  RapEntry rap{};
  rap.pos        = start_pos;
  rap.byte_pos   = start_byte;
  rap.rtp_ts     = 90000;
  rap.generation = 1;
  rap.au_bytes   = 300 * 1200;
  rap.track_id   = 0;
  rap.open_rap   = false;
  rap.valid      = true;
  log.ring().record_rap(rap);

  FastPictureFirstPolicy policy(2 * 1024 * 1024, 1, true, 0);
  const auto             plan = policy.plan_cursor(log.ring(), 0, 1, 2 * 1024 * 1024, true);
  EXPECT_TRUE(plan.has_rap);
  EXPECT_EQ(plan.start_pos, start_pos);

  auto burst = policy.start(log);
  ASSERT_EQ(burst.size(), 300u);
  EXPECT_EQ(burst.front().sequence, 1000u);
  EXPECT_EQ(burst.back().sequence, 1299u);
}

TEST(StartupPolicyTest, FreshnessFirstAndFastPictureFirstPolicies) {
  NumaArena                   arena(4, 256);
  rivet::test::ArenaLeakGuard guard(arena);
  FreshnessFirstPolicy        fresh;
  RetainedPacketLog           log;
  EXPECT_TRUE(fresh.requires_rtcp_pli());
  EXPECT_TRUE(fresh.start(log).empty());

  FastPictureFirstPolicy fast;
  EXPECT_FALSE(fast.requires_rtcp_pli());
  EXPECT_TRUE(fast.start(log).empty());

  auto idr_pkt = PacketRef::adopt(arena.allocate(), 0, 20, 0, 90000, 100, 0, pkt::kClosedIrap);
  log.append(std::move(idr_pkt));
  auto burst = fast.start(log);
  ASSERT_EQ(burst.size(), 1u);
  EXPECT_EQ(burst[0].sequence, 100u);
}
