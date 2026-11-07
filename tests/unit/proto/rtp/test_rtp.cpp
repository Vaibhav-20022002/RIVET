#include "rivet/memory/NumaArena.hpp"
#include "rivet/memory/PacketFlags.hpp"
#include "rivet/memory/PacketRef.hpp"
#include "rivet/proto/rtp/JitterEstimator.hpp"
#include "rivet/proto/rtp/RtcpReportGenerator.hpp"
#include "rivet/proto/rtp/RtpPacketClassifier.hpp"
#include "rivet/proto/rtp/RtpSenderContext.hpp"
#include "rivet/proto/rtp/SequenceTracker.hpp"
#include "tests/support/H264Synth.hpp"
#include "tests/support/RtpBuilder.hpp"
#include <cstring>
#include <gtest/gtest.h>

using namespace rivet::proto::rtp;
using namespace rivet::test;
using rivet::memory::PacketRef;
namespace pkt = rivet::memory::pkt;

/// ============================================================================
/// RtpPacketClassifier Tests (including RTCP Demux Guard)
/// ============================================================================

TEST(RtpPacketClassifierTest, TooShortPacketInvalid) {
  uint8_t short_data[8]{};
  auto    info = RtpPacketClassifier::classify(short_data, sizeof(short_data), 0);
  EXPECT_FALSE(info.valid);
}

TEST(RtpPacketClassifierTest, NonVersion2Invalid) {
  auto p    = RtpBuilder().version(1).build();
  auto info = RtpPacketClassifier::classify(p.data(), p.size(), 0);
  EXPECT_FALSE(info.valid);
}

TEST(RtpPacketClassifierTest, RtcpPacketsNeverClassifiedAsRtp) {
  /// RFC 5761 multiplex RTCP range: raw byte 1 in 200..207 (and 72..79 without marker bit)
  for (uint8_t raw_b1 : {uint8_t{200},
           uint8_t{201},
           uint8_t{202},
           uint8_t{203},
           uint8_t{204},
           uint8_t{205},
           uint8_t{206},
           uint8_t{207},
           uint8_t{72},
           uint8_t{73},
           uint8_t{76}}) {
    uint8_t rtcp_pkt[28]{};
    rtcp_pkt[0] = 0x80; //< V=2
    rtcp_pkt[1] = raw_b1;
    rtcp_pkt[3] = 6;
    auto info   = RtpPacketClassifier::classify(rtcp_pkt, sizeof(rtcp_pkt), 0);
    EXPECT_FALSE(info.valid) << "raw byte 1 = " << static_cast<int>(raw_b1);
    EXPECT_TRUE(info.is_rtcp) << "raw byte 1 = " << static_cast<int>(raw_b1);
  }
}

TEST(RtpPacketClassifierTest, BasicHeaderAndSingleNalIdr) {
  auto idr_nal = H264Synth::make_idr(32);
  auto p       = RtpBuilder().seq(100).timestamp(90000).marker(true).payload(idr_nal).build();

  auto info = RtpPacketClassifier::classify(p.data(), p.size(), 0);
  EXPECT_TRUE(info.valid);
  EXPECT_FALSE(info.is_rtcp);
  EXPECT_EQ(info.seq, 100u);
  EXPECT_EQ(info.timestamp, 90000u);
  EXPECT_TRUE(info.marker);
  EXPECT_EQ(info.header_len, 12u);
  EXPECT_EQ(info.payload_len, idr_nal.size());

  EXPECT_NE(info.flags & pkt::kClosedIrap, 0u);
  EXPECT_NE(info.flags & pkt::kRef, 0u);
  EXPECT_NE(info.flags & pkt::kMarker, 0u);
}

TEST(RtpPacketClassifierTest, H264NonReferenceBFrame) {
  auto b_nal = H264Synth::make_b_frame(16); //< NRI = 0
  auto p     = RtpBuilder().seq(101).timestamp(93000).marker(false).payload(b_nal).build();

  auto info = RtpPacketClassifier::classify(p.data(), p.size(), 0);
  EXPECT_TRUE(info.valid);
  EXPECT_EQ(info.flags & pkt::kClosedIrap, 0u);
  EXPECT_EQ(info.flags & pkt::kRef, 0u);
  EXPECT_EQ(info.flags & pkt::kMarker, 0u);
  EXPECT_TRUE(pkt::unpack_vcl(info.packed_track_meta));
}

TEST(RtpPacketClassifierTest, FuAFragmentationStartAndEnd) {
  auto idr_nal = H264Synth::make_idr(2000);
  auto frags   = H264Synth::fragment_fua(idr_nal, 1000);
  ASSERT_EQ(frags.size(), 2u);

  auto p1 = RtpBuilder().seq(200).timestamp(90000).marker(false).payload(frags[0].payload).build();
  auto info1 = RtpPacketClassifier::classify(p1.data(), p1.size(), 0);
  EXPECT_TRUE(info1.valid);
  EXPECT_NE(info1.flags & pkt::kClosedIrap, 0u);
  EXPECT_NE(info1.flags & pkt::kRef, 0u);
  EXPECT_EQ(info1.flags & pkt::kMarker, 0u);

  auto p2 = RtpBuilder().seq(201).timestamp(90000).marker(true).payload(frags[1].payload).build();
  auto info2 = RtpPacketClassifier::classify(p2.data(), p2.size(), 0);
  EXPECT_TRUE(info2.valid);
  EXPECT_EQ(info2.flags & pkt::kClosedIrap, 0u);
  EXPECT_NE(info2.flags & pkt::kRef, 0u);
  EXPECT_NE(info2.flags & pkt::kMarker, 0u);
}

TEST(RtpPacketClassifierTest, AudioTrackAlwaysReference) {
  std::vector<uint8_t> aac_payload = {0x00, 0x10, 0x05, 0x20};
  auto p = RtpBuilder().seq(500).timestamp(48000).marker(true).payload(aac_payload).build();

  auto info = RtpPacketClassifier::classify(p.data(), p.size(), 1);
  EXPECT_TRUE(info.valid);
  EXPECT_EQ(info.track_id, 1u);
  EXPECT_NE(info.flags & pkt::kRef, 0u);
  EXPECT_NE(info.flags & pkt::kMarker, 0u);
}

TEST(RtpPacketClassifierTest, ExtensionAndPaddingHandling) {
  std::vector<uint8_t> payload = {0x65, 0x88, 0x80};
  auto                 p       = RtpBuilder()
                                     .seq(1)
                                     .add_csrc(0xAABBCCDD)
                                     .extension(0xBEDE, {0x11223344})
                                     .payload(payload)
                                     .padding(4)
                                     .build();

  auto info = RtpPacketClassifier::classify(p.data(), p.size(), 0);
  EXPECT_TRUE(info.valid);
  EXPECT_EQ(info.header_len, 24u);
  EXPECT_EQ(info.payload_len, payload.size());
}

/// ============================================================================
/// SequenceTracker Tests (RFC 3550 Appendix A.1)
/// ============================================================================

TEST(SequenceTrackerTest, TwoPacketInitialProbationAndInOrderProgression) {
  SequenceTracker tracker;
  EXPECT_TRUE(tracker.is_in_probation());

  /// Packet 1 starts probation (withheld until consecutive packet confirms source)
  auto s1 = tracker.update(100);
  EXPECT_FALSE(s1.has_value());
  EXPECT_TRUE(tracker.is_in_probation());

  /// Packet 2 confirms source and exits probation permanently
  auto s2 = tracker.update(101);
  ASSERT_TRUE(s2.has_value());
  EXPECT_EQ(*s2, 101u);
  EXPECT_FALSE(tracker.is_in_probation());

  auto s3 = tracker.update(102);
  ASSERT_TRUE(s3.has_value());
  EXPECT_EQ(*s3, 102u);
}

TEST(SequenceTrackerTest, RolloverAcross65535Boundary) {
  SequenceTracker tracker;
  EXPECT_FALSE(tracker.update(65534).has_value()); //< Probation packet 1
  auto s2 = tracker.update(65535);                 //< Probation packet 2
  ASSERT_TRUE(s2.has_value());
  EXPECT_EQ(*s2, 65535u);

  auto s3 = tracker.update(0);
  ASSERT_TRUE(s3.has_value());
  EXPECT_EQ(*s3, 65536u);

  auto s4 = tracker.update(1);
  ASSERT_TRUE(s4.has_value());
  EXPECT_EQ(*s4, 65537u);
}

TEST(SequenceTrackerTest, Full100PacketMisorderAnd128BitDedupWindow) {
  SequenceTracker tracker;
  (void)tracker.update(100);
  ASSERT_TRUE(tracker.update(101).has_value());

  /// Advance up to 200 while skipping seq 110 (90 packets behind 200 — beyond old 64-bit mask!)
  for (uint16_t s = 102; s <= 200; ++s) {
    if (s == 110) continue;
    ASSERT_TRUE(tracker.update(s).has_value());
  }

  /// Late arrival of seq 110 (dist = 90 <= MAX_MISORDER = 100) accepted once
  auto late = tracker.update(110);
  ASSERT_TRUE(late.has_value());
  EXPECT_EQ(*late, 110u);

  /// Second arrival of seq 110 (dist = 90) MUST be rejected by 128-bit dedup bitmap!
  EXPECT_FALSE(tracker.update(110).has_value());
  /// Duplicate of initial probation packet 100 is >100 behind and rejected
  EXPECT_FALSE(tracker.update(100).has_value());
}

TEST(SequenceTrackerTest, StaleSeq1NeverMatchesBadSeqSentinelAndResyncPreservesCycles) {
  SequenceTracker tracker;
  (void)tracker.update(65534);
  ASSERT_TRUE(tracker.update(65535).has_value());
  /// Advance into cycle 1 (`cycles_ == 65536`, `max_seq == 5000`, `max_ext_seq == 70536`)
  for (uint16_t s = 0; s <= 5000; ++s) {
    ASSERT_TRUE(tracker.update(s).has_value());
  }
  const uint64_t before_resync = tracker.highest_extended_seq();
  EXPECT_EQ(before_resync, 65536ULL + 5000ULL);

  /// Single stale packet with `seq == 1` (delta > MAX_DROPOUT) MUST NOT falsely match
  /// `BAD_SEQ_SENTINEL` (`65537 & 0xFFFF == 1` bug in old code!)
  EXPECT_FALSE(tracker.update(1).has_value());
  EXPECT_FALSE(tracker.did_resync_on_last());

  /// Genuine publisher restart at seq 200, 201: resyncs on 201 AND preserves monotonic extended seq!
  EXPECT_FALSE(tracker.update(200).has_value());
  auto resynced = tracker.update(201);
  ASSERT_TRUE(resynced.has_value());
  EXPECT_TRUE(tracker.did_resync_on_last());
  EXPECT_GT(*resynced, before_resync);
}

/// ============================================================================
/// JitterEstimator Tests (including 32-bit Wrap)
/// ============================================================================

TEST(JitterEstimatorTest, ConstantDelayProducesZeroJitter) {
  JitterEstimator estimator;
  EXPECT_DOUBLE_EQ(estimator.update(90000, 100000), 0.0);
  EXPECT_DOUBLE_EQ(estimator.update(93000, 103000), 0.0);
  EXPECT_DOUBLE_EQ(estimator.update(96000, 106000), 0.0);
  EXPECT_EQ(estimator.get_rtcp_jitter(), 0u);
}

TEST(JitterEstimatorTest, VarianceAccumulatesEwmaJitter) {
  JitterEstimator estimator;
  estimator.update(1000, 2000);
  double j = estimator.update(2000, 3160);
  EXPECT_NEAR(j, 10.0, 0.01);
  EXPECT_EQ(estimator.get_rtcp_jitter(), 10u);
}

TEST(JitterEstimatorTest, Modular32BitWrapProducesZeroSpike) {
  JitterEstimator estimator;
  /// RTP timestamp wraps across 2^32 while arrival wraps independently with +16 ticks jitter
  const uint32_t rtp0 = 0xFFFF'F800U;
  const uint32_t arr0 = 0xFFFF'FC00U;
  EXPECT_DOUBLE_EQ(estimator.update(rtp0, arr0), 0.0);

  const uint32_t rtp1 = rtp0 + 3000U;       //< wraps past 2^32
  const uint32_t arr1 = arr0 + 3000U + 16U; //< wraps past 2^32 with +16 tick transit delta
  const double   j    = estimator.update(rtp1, arr1);
  EXPECT_NEAR(j, 1.0, 0.01);
  EXPECT_EQ(estimator.get_rtcp_jitter(), 1u);
}

/// ============================================================================
/// RtcpReportGenerator Tests (RR, SR, PLI, BYE & Parse)
/// ============================================================================

TEST(RtcpReportGeneratorTest, Full32ByteReceiverReportAndParser) {
  RtcpReportGenerator generator(0x11223344);

  ReceiverReportBlock block{};
  block.ssrc                = 0xAABBCCDD;
  block.fraction_lost       = 5;
  block.cumulative_lost     = -3;
  block.highest_seq_recv    = 0x00010004U;
  block.jitter              = 0x0000000FU;
  block.last_sr_timestamp   = 0x55667788U;
  block.delay_since_last_sr = 0x00010000U;

  auto packet = generator.build_rr(block);
  ASSERT_EQ(packet.size(), 32u);
  EXPECT_EQ(packet[0], 0x81);
  EXPECT_EQ(packet[1], 201);
  EXPECT_EQ(packet[2], 0);
  EXPECT_EQ(packet[3], 7);

  /// Cumulative lost (-3 in signed 24-bit = 0xFFFFFD)
  block.cumulative_lost = 0xFFFFFDU;
  packet                = generator.build_rr(block);
  EXPECT_EQ(packet[13], 0xFF);
  EXPECT_EQ(packet[14], 0xFF);
  EXPECT_EQ(packet[15], 0xFD);

  auto parsed = RtcpReportGenerator::parse(packet);
  EXPECT_TRUE(parsed.valid);
  ASSERT_TRUE(parsed.first_rr.has_value());
  EXPECT_EQ(parsed.sender_ssrc, 0x11223344u);
  EXPECT_EQ(parsed.first_rr->fraction_lost, 5u);
  EXPECT_EQ(parsed.first_rr->highest_seq_recv, 0x00010004u);
  EXPECT_EQ(parsed.first_rr->jitter, 15u);
  EXPECT_EQ(parsed.first_rr->last_sr_timestamp, 0x55667788u);
  EXPECT_EQ(parsed.first_rr->delay_since_last_sr, 0x00010000u);
}

TEST(RtcpReportGeneratorTest, BuildsAndParsesSrPliAndBye) {
  RtcpReportGenerator gen(0x52495654U);

  auto pli = gen.build_pli(0xCAFEBABEU);
  ASSERT_EQ(pli.size(), 12u);
  EXPECT_EQ(pli[0], 0x81); //< V=2, FMT=1 (PLI)
  EXPECT_EQ(pli[1], 206);  //< PSFB
  auto p_pli = RtcpReportGenerator::parse(pli);
  EXPECT_TRUE(p_pli.valid);
  EXPECT_EQ(p_pli.packet_type, kRtcpPtPsfb);
  EXPECT_EQ(p_pli.rc_or_fmt, 1u);

  SenderReportInfo sr_in{};
  sr_in.ntp_sec             = 0x83AA7E80U;
  sr_in.ntp_frac            = 0x80000000U;
  sr_in.rtp_timestamp       = 90000U;
  sr_in.sender_packet_count = 100U;
  sr_in.sender_octet_count  = 120000U;
  auto sr                   = gen.build_sr(sr_in);
  ASSERT_EQ(sr.size(), 28u);
  auto p_sr = RtcpReportGenerator::parse(sr);
  EXPECT_TRUE(p_sr.valid);
  ASSERT_TRUE(p_sr.sr_info.has_value());
  EXPECT_EQ(p_sr.sr_info->rtp_timestamp, 90000U);
  EXPECT_EQ(p_sr.sr_info->sender_packet_count, 100U);
  EXPECT_EQ(p_sr.sr_info->sender_octet_count, 120000U);

  auto bye   = RtcpReportGenerator::build_bye(0x52495654U);
  auto p_bye = RtcpReportGenerator::parse(bye);
  EXPECT_TRUE(p_bye.valid);
  EXPECT_EQ(p_bye.packet_type, kRtcpPtBye);
  EXPECT_EQ(p_bye.sender_ssrc, 0x52495654U);
}

/// ============================================================================
/// RtpSenderContext Tests (Mode A & Mode B Header Rewriting)
/// ============================================================================

TEST(RtpSenderContextTest, PassThroughPreservesWireValues) {
  rivet::memory::NumaArena arena(4, 256);
  auto wire = RtpBuilder().seq(500).timestamp(90000).ssrc(0xCAFE1234).payload({0x65, 0x88}).build();
  auto *slab = arena.allocate();
  std::memcpy(slab->payload_data, wire.data(), wire.size());
  auto pkt = PacketRef::adopt(slab, 0, static_cast<uint32_t>(wire.size()), 0, 90000, 500, 0, 0);

  RtpSenderContext ctx(SenderMode::TransparentPassThrough, 0x11112222);
  uint8_t          hdr[32]{};
  EXPECT_EQ(ctx.build_rewritten_rtp_header(pkt, hdr, sizeof(hdr)), 0u);
  EXPECT_EQ(ctx.last_seq_out(), 500u);
  EXPECT_EQ(ctx.last_ts_out(), 90000u);
}

TEST(RtpSenderContextTest, SessionOriginTranslationPreservesBFrameOffsetsAndRewritesHeader) {
  rivet::memory::NumaArena arena(8, 256);
  RtpSenderContext         ctx(SenderMode::SessionOriginTranslation, 0 /* CSPRNG SSRC */);
  EXPECT_NE(ctx.get_ssrc_out(), 0u);

  ctx.anchor_initial(1000, 90000);
  EXPECT_TRUE(ctx.is_anchored());

  auto make_pkt = [&](uint16_t seq, uint32_t ts) {
    auto  wire = RtpBuilder().seq(seq).timestamp(ts).ssrc(0x99999999).payload({0x41, 0x01}).build();
    auto *slab = arena.allocate();
    std::memcpy(slab->payload_data, wire.data(), wire.size());
    return PacketRef::adopt(slab, 0, static_cast<uint32_t>(wire.size()), 0, ts, seq, 0, 0);
  };

  /// Frame 0 (IDR): seq 1000, ts 90000
  auto p0 = make_pkt(1000, 90000);
  ctx.prepare_forward(p0);
  const uint16_t seq0 = ctx.last_seq_out();
  const uint32_t ts0  = ctx.last_ts_out();
  EXPECT_EQ(seq0, ctx.first_seq_out());
  EXPECT_EQ(ts0, ctx.first_ts_out());

  /// Frame 1 (P): seq 1001, ts +6000 (out-of-order PTS ahead of B-frame)
  auto p1 = make_pkt(1001, 96000);
  ctx.prepare_forward(p1);
  EXPECT_EQ(ctx.last_seq_out(), static_cast<uint16_t>(ctx.first_seq_out() + 1U));
  EXPECT_EQ(static_cast<int32_t>(ctx.last_ts_out() - ts0), 6000);

  /// Frame 2 (B-frame with non-monotonic PTS < Frame 1!): seq 1002, ts 93000
  auto    p2 = make_pkt(1002, 93000);
  uint8_t hdr_buf[128]{};
  size_t  hlen = ctx.build_rewritten_rtp_header(p2, hdr_buf, sizeof(hdr_buf));
  ASSERT_EQ(hlen, 12u);
  EXPECT_EQ(ctx.last_seq_out(), static_cast<uint16_t>(ctx.first_seq_out() + 2U));
  EXPECT_EQ(static_cast<int32_t>(ctx.last_ts_out() - ts0), 3000);

  /// Verify non-mutating header buffer rewrite leaves shared slab untouched!
  const uint16_t wire_seq_out = static_cast<uint16_t>((hdr_buf[2] << 8U) | hdr_buf[3]);
  EXPECT_EQ(wire_seq_out, ctx.last_seq_out());
  const auto *orig_bytes = reinterpret_cast<const uint8_t *>(p2.data());
  EXPECT_EQ(static_cast<uint16_t>((orig_bytes[2] << 8U) | orig_bytes[3]), 1002u);
}
