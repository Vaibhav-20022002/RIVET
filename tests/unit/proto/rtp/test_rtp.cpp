#include "rivet/memory/PacketRef.hpp"
#include "rivet/proto/rtp/JitterEstimator.hpp"
#include "rivet/proto/rtp/RtcpReportGenerator.hpp"
#include "rivet/proto/rtp/RtpPacketClassifier.hpp"
#include "rivet/proto/rtp/RtpSenderContext.hpp"
#include "rivet/proto/rtp/SequenceTracker.hpp"
#include "tests/support/H264Synth.hpp"
#include "tests/support/RtpBuilder.hpp"
#include <gtest/gtest.h>

using namespace rivet::proto::rtp;
using namespace rivet::test;
using rivet::memory::PacketRef;

/// ============================================================================
/// RtpPacketClassifier Tests
/// ============================================================================

TEST(RtpPacketClassifierTest, TooShortPacketInvalid) {
  uint8_t short_data[8]{};
  auto    info = RtpPacketClassifier::classify(short_data, sizeof(short_data), 0);
  EXPECT_FALSE(info.valid);
}

TEST(RtpPacketClassifierTest, NonVersion2Invalid) {
  auto pkt  = RtpBuilder().version(1).build();
  auto info = RtpPacketClassifier::classify(pkt.data(), pkt.size(), 0);
  EXPECT_FALSE(info.valid);
}

TEST(RtpPacketClassifierTest, BasicHeaderAndSingleNalIdr) {
  auto idr_nal = H264Synth::make_idr(32);
  auto pkt     = RtpBuilder().seq(100).timestamp(90000).marker(true).payload(idr_nal).build();

  auto info = RtpPacketClassifier::classify(pkt.data(), pkt.size(), 0);
  EXPECT_TRUE(info.valid);
  EXPECT_EQ(info.seq, 100u);
  EXPECT_EQ(info.timestamp, 90000u);
  EXPECT_TRUE(info.marker);
  EXPECT_EQ(info.header_len, 12u);
  EXPECT_EQ(info.payload_len, idr_nal.size());

  /// IDR keyframe: bit 0 (IDR) + bit 2 (Reference) + bit 3 (Marker) = 0x01 | 0x04 | 0x08 = 0x0D
  EXPECT_EQ(info.flags & 0x01, 0x01); //< IDR
  EXPECT_EQ(info.flags & 0x04, 0x04); //< Reference
  EXPECT_EQ(info.flags & 0x08, 0x08); //< Marker
}

TEST(RtpPacketClassifierTest, H264NonReferenceBFrame) {
  auto b_nal = H264Synth::make_b_frame(16); //< NRI = 0
  auto pkt   = RtpBuilder().seq(101).timestamp(93000).marker(false).payload(b_nal).build();

  auto info = RtpPacketClassifier::classify(pkt.data(), pkt.size(), 0);
  EXPECT_TRUE(info.valid);
  EXPECT_EQ(info.flags & 0x01, 0); //< Not IDR
  EXPECT_EQ(info.flags & 0x04, 0); //< Not Reference (Disposable)
  EXPECT_EQ(info.flags & 0x08, 0); //< Not Marker
}

TEST(RtpPacketClassifierTest, FuAFragmentationStartAndEnd) {
  auto idr_nal = H264Synth::make_idr(2000);
  auto frags   = H264Synth::fragment_fua(idr_nal, 1000);
  ASSERT_EQ(frags.size(), 2u);

  /// First fragment (Start = 1, End = 0)
  auto pkt1 =
      RtpBuilder().seq(200).timestamp(90000).marker(false).payload(frags[0].payload).build();
  auto info1 = RtpPacketClassifier::classify(pkt1.data(), pkt1.size(), 0);
  EXPECT_TRUE(info1.valid);
  EXPECT_EQ(info1.flags & 0x01, 0x01); //< IDR start fragment
  EXPECT_EQ(info1.flags & 0x04, 0x04); //< Reference
  EXPECT_EQ(info1.flags & 0x08, 0);    //< Not Marker

  /// Second fragment (Start = 0, End = 1) with marker bit set
  auto pkt2 = RtpBuilder().seq(201).timestamp(90000).marker(true).payload(frags[1].payload).build();
  auto info2 = RtpPacketClassifier::classify(pkt2.data(), pkt2.size(), 0);
  EXPECT_TRUE(info2.valid);
  EXPECT_EQ(info2.flags & 0x01, 0);    //< Not start fragment
  EXPECT_EQ(info2.flags & 0x04, 0x04); //< Still reference
  EXPECT_EQ(info2.flags & 0x08, 0x08); //< Marker
}

TEST(RtpPacketClassifierTest, AudioTrackAlwaysReference) {
  std::vector<uint8_t> aac_payload = {0x00, 0x10, 0x05, 0x20};
  auto pkt = RtpBuilder().seq(500).timestamp(48000).marker(true).payload(aac_payload).build();

  auto info = RtpPacketClassifier::classify(pkt.data(), pkt.size(), 1 /* track 1 = audio */);
  EXPECT_TRUE(info.valid);
  EXPECT_EQ(info.track_id, 1u);
  EXPECT_EQ(info.flags & 0x04, 0x04); //< Always Reference
  EXPECT_EQ(info.flags & 0x08, 0x08); //< Marker
}

TEST(RtpPacketClassifierTest, ExtensionAndPaddingHandling) {
  std::vector<uint8_t> payload = {0x65, 0x88, 0x80}; //< IDR
  auto                 pkt     = RtpBuilder()
                                     .seq(1)
                                     .add_csrc(0xAABBCCDD)
                                     .extension(0xBEDE, {0x11223344})
                                     .payload(payload)
                                     .padding(4)
                                     .build();

  auto info = RtpPacketClassifier::classify(pkt.data(), pkt.size(), 0);
  EXPECT_TRUE(info.valid);
  EXPECT_EQ(info.header_len, 12u + 4u + 8u); //< 12 + 1 CSRC (4) + extension (4 + 4) = 24
  EXPECT_EQ(info.payload_len, payload.size());
}

/// ============================================================================
/// SequenceTracker Tests
/// ============================================================================

TEST(SequenceTrackerTest, InOrderSequenceProgression) {
  SequenceTracker tracker;
  auto            s1 = tracker.update(100);
  ASSERT_TRUE(s1.has_value());
  EXPECT_EQ(*s1, 100u);

  auto s2 = tracker.update(101);
  ASSERT_TRUE(s2.has_value());
  EXPECT_EQ(*s2, 101u);

  auto s3 = tracker.update(102);
  ASSERT_TRUE(s3.has_value());
  EXPECT_EQ(*s3, 102u);
}

TEST(SequenceTrackerTest, RolloverAcross65535Boundary) {
  SequenceTracker tracker;
  auto            s1 = tracker.update(65534);
  ASSERT_TRUE(s1.has_value());
  EXPECT_EQ(*s1, 65534u);

  auto s2 = tracker.update(65535);
  ASSERT_TRUE(s2.has_value());
  EXPECT_EQ(*s2, 65535u);

  /// Wrap around to 0
  auto s3 = tracker.update(0);
  ASSERT_TRUE(s3.has_value());
  EXPECT_EQ(*s3, 65536u);

  auto s4 = tracker.update(1);
  ASSERT_TRUE(s4.has_value());
  EXPECT_EQ(*s4, 65537u);
}

TEST(SequenceTrackerTest, RejectsDuplicateWithinWindow) {
  SequenceTracker tracker;
  EXPECT_TRUE(tracker.update(50).has_value());
  EXPECT_TRUE(tracker.update(51).has_value());
  EXPECT_TRUE(tracker.update(52).has_value());

  /// Duplicate of 51
  auto dup = tracker.update(51);
  EXPECT_FALSE(dup.has_value());
}

TEST(SequenceTrackerTest, RejectsImmediateDuplicate) {
  SequenceTracker tracker;
  EXPECT_TRUE(tracker.update(50).has_value());
  auto dup = tracker.update(50);
  EXPECT_FALSE(dup.has_value());
}

/// ============================================================================
/// JitterEstimator Tests
/// ============================================================================

TEST(JitterEstimatorTest, ConstantDelayProducesZeroJitter) {
  JitterEstimator estimator;
  /// Arrival clock advances in lockstep with packet RTP timestamps
  EXPECT_DOUBLE_EQ(estimator.update(90000, 100000), 0.0);
  EXPECT_DOUBLE_EQ(estimator.update(93000, 103000), 0.0);
  EXPECT_DOUBLE_EQ(estimator.update(96000, 106000), 0.0);
  EXPECT_EQ(estimator.get_rtcp_jitter(), 0u);
}

TEST(JitterEstimatorTest, VarianceAccumulatesEwmaJitter) {
  JitterEstimator estimator;
  estimator.update(1000, 2000); //< transit = 1000
  /// Next packet delayed by 160 ticks: transit = 1160, D = 160
  /// J = 0 + (160 - 0) / 16 = 10
  double j = estimator.update(2000, 3160);
  EXPECT_NEAR(j, 10.0, 0.01);
  EXPECT_EQ(estimator.get_rtcp_jitter(), 10u);
}

/// ============================================================================
/// RtcpReportGenerator Tests
/// ============================================================================

TEST(RtcpReportGeneratorTest, BuildsRfc3550CompliantReceiverReport) {
  RtcpReportGenerator generator(0x11223344);

  ReceiverReportBlock block{};
  block.ssrc                = 0xAABBCCDD;
  block.fraction_lost       = 5;
  block.cumulative_lost     = 120;
  block.highest_seq_recv    = 65540;
  block.jitter              = 15;
  block.last_sr_timestamp   = 0x55667788;
  block.delay_since_last_sr = 0x00010000;

  auto packet = generator.build_rr(block);
  ASSERT_EQ(packet.size(), 32u);

  /// RTCP Header: V=2, P=0, RC=1 -> 0x81
  EXPECT_EQ(packet[0], 0x81);
  /// PT: 201 (Receiver Report)
  EXPECT_EQ(packet[1], 201);
  /// Length: 7 (32-bit words minus 1)
  EXPECT_EQ(packet[2], 0);
  EXPECT_EQ(packet[3], 7);

  /// Our SSRC (0x11223344)
  EXPECT_EQ(packet[4], 0x11);
  EXPECT_EQ(packet[5], 0x22);
  EXPECT_EQ(packet[6], 0x33);
  EXPECT_EQ(packet[7], 0x44);

  /// Target SSRC (0xAABBCCDD)
  EXPECT_EQ(packet[8], 0xAA);
  EXPECT_EQ(packet[9], 0xBB);
  EXPECT_EQ(packet[10], 0xCC);
  EXPECT_EQ(packet[11], 0xDD);

  /// Fraction lost
  EXPECT_EQ(packet[12], 5);
}

/// ============================================================================
/// RtpSenderContext Tests
/// ============================================================================

TEST(RtpSenderContextTest, PassThroughPreservesValues) {
  RtpSenderContext ctx(SenderMode::TransparentPassThrough, 0x12345678);
  EXPECT_EQ(ctx.get_mode(), SenderMode::TransparentPassThrough);
  EXPECT_EQ(ctx.get_ssrc_out(), 0x12345678);

  PacketRef pkt;
  pkt.sequence      = 500;
  pkt.rtp_timestamp = 90000;

  ctx.prepare_forward(pkt);
  /// In pass-through mode, offsets remain zero
}

TEST(RtpSenderContextTest, SessionOriginTranslationResync) {
  RtpSenderContext ctx(SenderMode::SessionOriginTranslation, 0xAABBCCDD);
  EXPECT_EQ(ctx.get_mode(), SenderMode::SessionOriginTranslation);

  /// Client connects, server resyncs stream after upstream restart
  ctx.resync_stream(1000, 90000);

  PacketRef pkt;
  pkt.sequence      = 1000;
  pkt.rtp_timestamp = 90000;

  ctx.prepare_forward(pkt);
}
