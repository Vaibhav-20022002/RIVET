#include "rivet/media/AccessUnitView.hpp"
#include "rivet/media/AuAssembler.hpp"
#include "rivet/media/ConfigGeneration.hpp"
#include "rivet/media/DependencyDropPolicy.hpp"
#include "rivet/media/H264RapClassifier.hpp"
#include "rivet/media/HevcRapClassifier.hpp"
#include "rivet/media/RetainedPacketLog.hpp"
#include "rivet/media/StartupPolicy.hpp"
#include "rivet/memory/NumaArena.hpp"
#include <gtest/gtest.h>

using namespace rivet::media;
using namespace rivet::memory;

/// ============================================================================
/// H264RapClassifier Tests
/// ============================================================================

TEST(H264RapClassifierTest, ClassifyNalTypes) {
  uint8_t short_p[1] = {0x00};
  EXPECT_EQ(H264RapClassifier::classify(short_p, 0), H264NalType::Regular);

  /// Single NAL: 0x65 = NRI 3, Type 5 (IDR)
  uint8_t idr_pkt[] = {0x65, 0x88, 0x80};
  EXPECT_EQ(H264RapClassifier::classify(idr_pkt, sizeof(idr_pkt)), H264NalType::IDR);

  /// SPS: 0x67 = NRI 3, Type 7
  uint8_t sps_pkt[] = {0x67, 0x42, 0x00};
  EXPECT_EQ(H264RapClassifier::classify(sps_pkt, sizeof(sps_pkt)), H264NalType::SPS);

  /// PPS: 0x68 = NRI 3, Type 8
  uint8_t pps_pkt[] = {0x68, 0xCE, 0x01};
  EXPECT_EQ(H264RapClassifier::classify(pps_pkt, sizeof(pps_pkt)), H264NalType::PPS);

  /// Non-IDR Slice: 0x41 = NRI 2, Type 1 (P-frame)
  uint8_t p_pkt[] = {0x41, 0x9A};
  EXPECT_EQ(H264RapClassifier::classify(p_pkt, sizeof(p_pkt)), H264NalType::Regular);

  /// FU-A IDR: Type 28, Indicator 0x7C, FU header 0x85 (Start=1, Type=5)
  uint8_t fua_idr[] = {0x7C, 0x85, 0x12};
  EXPECT_EQ(H264RapClassifier::classify(fua_idr, sizeof(fua_idr)), H264NalType::IDR);

  /// STAP-A: 0x78 = Type 24
  uint8_t stap_a[] = {0x78, 0x00, 0x03, 0x67, 0x42, 0x00};
  EXPECT_EQ(H264RapClassifier::classify(stap_a, sizeof(stap_a)), H264NalType::STAP_A);
}

TEST(H264RapClassifierTest, ForEachParameterSetStapA) {
  /// STAP-A containing SPS (3 bytes) and PPS (3 bytes)
  uint8_t stap_a[] = {
      0x78, //< STAP-A header
      0x00,
      0x03, //< NALU 1 size = 3
      0x67,
      0x42,
      0x00, //< SPS
      0x00,
      0x03, //< NALU 2 size = 3
      0x68,
      0xCE,
      0x01 //< PPS
  };

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
/// HevcRapClassifier Tests
/// ============================================================================

TEST(HevcRapClassifierTest, ClassifyNalTypes) {
  /// Payload too short (<2 bytes)
  uint8_t short_p[1] = {0x00};
  EXPECT_EQ(HevcRapClassifier::classify(short_p, 1), HevcNalType::Regular);

  /// HEVC NAL format: byte 0 has type in bits [1..6] -> (type << 1)
  auto make_hevc = [](uint8_t type) -> std::vector<uint8_t> {
    return {static_cast<uint8_t>((type & 0x3F) << 1), 0x01};
  };

  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(8).data(), 2), HevcNalType::RASL);
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(9).data(), 2), HevcNalType::RADL);
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(19).data(), 2), HevcNalType::IDR);
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(20).data(), 2), HevcNalType::IDR);
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(21).data(), 2), HevcNalType::CRA);
  EXPECT_EQ(
      HevcRapClassifier::classify(make_hevc(32).data(), 2), HevcNalType::ParameterSet); //< VPS
  EXPECT_EQ(
      HevcRapClassifier::classify(make_hevc(33).data(), 2), HevcNalType::ParameterSet); //< SPS
  EXPECT_EQ(
      HevcRapClassifier::classify(make_hevc(34).data(), 2), HevcNalType::ParameterSet); //< PPS
  EXPECT_EQ(HevcRapClassifier::classify(make_hevc(1).data(), 2), HevcNalType::Regular);
}

/// ============================================================================
/// AccessUnitView Tests
/// ============================================================================

TEST(AccessUnitViewTest, BeginAndAppend) {
  NumaArena arena(4, 256);
  auto     *slab = arena.allocate();

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
  EXPECT_EQ(au.get_packets().size(), 1u);
}

/// ============================================================================
/// AuAssembler Tests
/// ============================================================================

TEST(AuAssemblerTest, SinglePacketAuAssembly) {
  NumaArena arena(4, 256);
  auto     *slab = arena.allocate();

  AuAssembler assembler;
  auto        pkt = PacketRef::adopt(slab, 0, 20, 0, 90000, 10, 0, 0x08);

  auto result = assembler.on_packet(std::move(pkt), true /* marker */);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->get_start_sequence(), 10u);
  EXPECT_EQ(result->get_rtp_timestamp(), 90000u);
  EXPECT_EQ(result->packet_count(), 1u);
}

TEST(AuAssemblerTest, MultiPacketAuAssembly) {
  NumaArena arena(4, 256);
  auto     *slab1 = arena.allocate();
  auto     *slab2 = arena.allocate();

  AuAssembler assembler;
  auto        pkt1 = PacketRef::adopt(slab1, 0, 20, 0, 90000, 10, 0, 0);
  auto        pkt2 = PacketRef::adopt(slab2, 0, 20, 0, 90000, 11, 0, 0x08);

  auto res1 = assembler.on_packet(std::move(pkt1), false);
  EXPECT_FALSE(res1.has_value());

  auto res2 = assembler.on_packet(std::move(pkt2), true);
  ASSERT_TRUE(res2.has_value());
  EXPECT_EQ(res2->packet_count(), 2u);
  EXPECT_EQ(res2->get_start_sequence(), 10u);
}

TEST(AuAssemblerTest, AbortAuOnTimestampChangeBeforeMarker) {
  NumaArena arena(4, 256);
  auto     *slab1 = arena.allocate();
  auto     *slab2 = arena.allocate();

  AuAssembler assembler;
  /// Frame 1 packet without marker
  auto                  pkt1 = PacketRef::adopt(slab1, 0, 20, 0, 90000, 10, 0, 0);
  [[maybe_unused]] auto res1 = assembler.on_packet(std::move(pkt1), false);

  /// New frame started (ts 93000) before frame 1 finished
  auto pkt2 = PacketRef::adopt(slab2, 0, 20, 0, 93000, 12, 0, true);
  auto res2 = assembler.on_packet(std::move(pkt2), true);

  ASSERT_TRUE(res2.has_value());
  /// Old frame was discarded; res2 contains the new frame starting at seq 12
  EXPECT_EQ(res2->get_start_sequence(), 12u);
  EXPECT_EQ(res2->get_rtp_timestamp(), 93000u);
}

/// ============================================================================
/// ConfigTracker Tests
/// ============================================================================

TEST(ConfigTrackerTest, MonotonicGenerationTracking) {
  ConfigTracker tracker;
  EXPECT_EQ(tracker.current_generation(), 0u);

  uint8_t  sps1[] = {0x67, 0x42, 0x00};
  uint32_t gen1   = tracker.update_sps(sps1, sizeof(sps1));
  EXPECT_EQ(gen1, 1u);
  EXPECT_EQ(tracker.current_generation(), 1u);

  /// Identical SPS does not bump generation
  uint32_t gen1_again = tracker.update_sps(sps1, sizeof(sps1));
  EXPECT_EQ(gen1_again, 1u);

  /// Changed PPS bumps generation
  uint8_t  pps1[] = {0x68, 0xCE, 0x01};
  uint32_t gen2   = tracker.update_pps(pps1, sizeof(pps1));
  EXPECT_EQ(gen2, 2u);

  /// Changed VPS bumps generation
  uint8_t  vps1[] = {0x40, 0x01, 0x0C};
  uint32_t gen3   = tracker.update_vps(vps1, sizeof(vps1));
  EXPECT_EQ(gen3, 3u);

  /// Historical generation retrieval
  auto historical = tracker.get_generation(1);
  ASSERT_TRUE(historical.has_value());
  EXPECT_EQ(historical->generation_id, 1u);
  EXPECT_EQ(historical->sps.size(), 3u);
  EXPECT_TRUE(historical->pps.empty());
}

/// ============================================================================
/// DependencyDropPolicy Tests
/// ============================================================================

TEST(DependencyDropPolicyTest, StrictThresholdEvaluation) {
  DependencyDropPolicy policy;

  /// Under 60% fill: Keep everything
  EXPECT_EQ(policy.evaluate(DependencyPriority::Disposable, 50), DropDecision::Keep);
  EXPECT_EQ(policy.evaluate(DependencyPriority::Reference, 50), DropDecision::Keep);
  EXPECT_EQ(policy.evaluate(DependencyPriority::Critical, 50), DropDecision::Keep);

  /// 60% to 89% fill: Drop Disposable (B-frames), Keep Reference and Critical
  EXPECT_EQ(policy.evaluate(DependencyPriority::Disposable, 60), DropDecision::Drop);
  EXPECT_EQ(policy.evaluate(DependencyPriority::Disposable, 80), DropDecision::Drop);
  EXPECT_EQ(policy.evaluate(DependencyPriority::Reference, 80), DropDecision::Keep);
  EXPECT_EQ(policy.evaluate(DependencyPriority::Critical, 80), DropDecision::Keep);

  /// 90% to 94% fill: Drop Reference (P-frames), Keep Critical (IDR)
  EXPECT_EQ(policy.evaluate(DependencyPriority::Reference, 90), DropDecision::Drop);
  EXPECT_EQ(policy.evaluate(DependencyPriority::Critical, 90), DropDecision::Keep);

  /// 95% to 99% fill: Drop Critical
  EXPECT_EQ(policy.evaluate(DependencyPriority::Critical, 95), DropDecision::Drop);

  /// 100% fill: Disconnect
  EXPECT_EQ(policy.evaluate(DependencyPriority::Critical, 100), DropDecision::Disconnect);
}

/// ============================================================================
/// RetainedPacketLog Tests
/// ============================================================================

TEST(RetainedPacketLogTest, AppendAndFindLatestRap) {
  NumaArena         arena(8, 256);
  RetainedPacketLog log;
  EXPECT_EQ(log.size(), 0u);
  EXPECT_FALSE(log.find_latest_rap_sequence().has_value());

  auto *slab1 = arena.allocate();
  auto *slab2 = arena.allocate();

  /// P-frame
  auto pkt1 = PacketRef::adopt(slab1, 0, 20, 0, 90000, 10, 0, 0x04);
  log.append(std::move(pkt1));
  EXPECT_EQ(log.size(), 1u);
  EXPECT_FALSE(log.find_latest_rap_sequence().has_value());

  /// IDR Keyframe (bit 0 = 1)
  auto pkt2 = PacketRef::adopt(slab2, 0, 20, 0, 93000, 11, 0, 0x01 | 0x04);
  log.append(std::move(pkt2));
  EXPECT_EQ(log.size(), 2u);

  auto rap = log.find_latest_rap_sequence();
  ASSERT_TRUE(rap.has_value());
  EXPECT_EQ(*rap, 11u);

  /// Snapshot from RAP
  auto burst = log.snapshot_from(11);
  EXPECT_EQ(burst.size(), 1u);
  EXPECT_EQ(burst[0].sequence, 11u);

  log.clear();
  EXPECT_EQ(log.size(), 0u);
}

/// ============================================================================
/// StartupPolicy Tests
/// ============================================================================

TEST(StartupPolicyTest, FreshnessFirstPolicy) {
  FreshnessFirstPolicy policy;
  RetainedPacketLog    log;
  EXPECT_TRUE(policy.requires_rtcp_pli());
  EXPECT_TRUE(policy.start(log).empty());
}

TEST(StartupPolicyTest, FastPictureFirstPolicy) {
  FastPictureFirstPolicy policy;
  EXPECT_FALSE(policy.requires_rtcp_pli());

  NumaArena         arena(4, 256);
  RetainedPacketLog log;
  /// Empty log
  EXPECT_TRUE(policy.start(log).empty());

  auto *slab    = arena.allocate();
  auto  idr_pkt = PacketRef::adopt(slab, 0, 20, 0, 90000, 100, 0, 0x01);
  log.append(std::move(idr_pkt));

  auto burst = policy.start(log);
  EXPECT_EQ(burst.size(), 1u);
  EXPECT_EQ(burst[0].sequence, 100u);
}
