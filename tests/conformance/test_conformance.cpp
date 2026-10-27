#include "rivet/proto/rtp/JitterEstimator.hpp"
#include "rivet/proto/rtp/RtcpReportGenerator.hpp"
#include "rivet/proto/rtp/RtpPacketClassifier.hpp"
#include "rivet/proto/rtp/SequenceTracker.hpp"
#include "rivet/proto/rtsp/RtspControlSession.hpp"
#include "rivet/proto/rtsp/RtspStreamFramer.hpp"
#include "tests/support/H264Synth.hpp"
#include "tests/support/RtpBuilder.hpp"
#include <gtest/gtest.h>

using namespace rivet::proto::rtp;
using namespace rivet::proto::rtsp;
using namespace rivet::test;

/// ============================================================================
/// RFC 7826 / 2326 RTSP State Machine Conformance
/// ============================================================================

class RtspConformanceTest : public ::testing::Test {
protected:
  RtspControlSession session{20000, 20001};
};

TEST_F(RtspConformanceTest, OptionsReturnsPublicMethodsAndCSeq) {
  std::string_view req = "OPTIONS rtsps://localhost:8554/live/test RTSP/1.0\r\n"
                         "CSeq: 1\r\n"
                         "User-Agent: ConformanceClient\r\n\r\n";

  std::string resp = session.handle_request(req);
  EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
  EXPECT_NE(resp.find("CSeq: 1"), std::string::npos);
  EXPECT_NE(resp.find("Public: OPTIONS, DESCRIBE, SETUP"), std::string::npos);
  EXPECT_EQ(session.get_state(), RtspState::Init);
}

TEST_F(RtspConformanceTest, DescribeReturnsSdpAndContentType) {
  std::string_view req = "DESCRIBE rtsps://localhost:8554/live/test RTSP/1.0\r\n"
                         "CSeq: 2\r\n"
                         "Accept: application/sdp\r\n\r\n";

  std::string resp = session.handle_request(req);
  EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
  EXPECT_NE(resp.find("CSeq: 2"), std::string::npos);
  EXPECT_NE(resp.find("Content-Type: application/sdp"), std::string::npos);
  EXPECT_NE(resp.find("Content-Length:"), std::string::npos);
  EXPECT_NE(resp.find("m=video"), std::string::npos);
}

TEST_F(RtspConformanceTest, SetupDedicatedUdpPortsTransitionsToReady) {
  std::string_view req = "SETUP rtsps://localhost:8554/live/test/streamid=0 RTSP/1.0\r\n"
                         "CSeq: 3\r\n"
                         "Transport: RTP/AVP;unicast;client_port=5000-5001\r\n\r\n";

  std::string resp = session.handle_request(req);
  EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
  EXPECT_NE(resp.find("CSeq: 3"), std::string::npos);
  EXPECT_NE(
      resp.find("Transport: RTP/AVP/UDP;unicast;client_port=5000-5001;server_port=20000-20001"),
      std::string::npos);
  EXPECT_NE(resp.find("Session:"), std::string::npos);
  EXPECT_EQ(session.get_state(), RtspState::Ready);
  EXPECT_FALSE(session.is_interleaved_mode());
  EXPECT_EQ(session.get_client_rtp_port(), 5000);
  EXPECT_EQ(session.get_client_rtcp_port(), 5001);
}

TEST_F(RtspConformanceTest, SetupInterleavedTcpModeParsesChannel) {
  std::string_view req = "SETUP rtsps://localhost:8554/live/test/streamid=0 RTSP/1.0\r\n"
                         "CSeq: 4\r\n"
                         "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n";

  std::string resp = session.handle_request(req);
  EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
  EXPECT_NE(resp.find("Transport: RTP/AVP/TCP;unicast;interleaved=0-1"), std::string::npos);
  EXPECT_TRUE(session.is_interleaved_mode());
  EXPECT_EQ(session.get_interleaved_channel(), 0);
  EXPECT_EQ(session.get_state(), RtspState::Ready);
}

TEST_F(RtspConformanceTest, PlayTransitionsToPlayingWithRtpInfo) {
  /// First SETUP
  std::string_view setup_req = "SETUP rtsps://localhost:8554/live/test/streamid=0 RTSP/1.0\r\n"
                               "CSeq: 10\r\n"
                               "Transport: RTP/AVP;unicast;client_port=6000-6001\r\n\r\n";
  session.handle_request(setup_req);

  std::string_view play_req = "PLAY rtsps://localhost:8554/live/test RTSP/1.0\r\n"
                              "CSeq: 11\r\n"
                              "Session: RIVET_SESSION_1001\r\n\r\n";

  std::string resp = session.handle_request(play_req);
  EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
  EXPECT_NE(resp.find("Range: npt=0.000-"), std::string::npos);
  EXPECT_EQ(session.get_state(), RtspState::Playing);
}

TEST_F(RtspConformanceTest, RecordTransitionsToRecording) {
  std::string_view setup_req =
      "SETUP rtsps://localhost:8554/live/test/streamid=0 RTSP/1.0\r\n"
      "CSeq: 20\r\n"
      "Transport: RTP/AVP;unicast;client_port=7000-7001;mode=record\r\n\r\n";
  session.handle_request(setup_req);

  std::string_view record_req = "RECORD rtsps://localhost:8554/live/test RTSP/1.0\r\n"
                                "CSeq: 21\r\n"
                                "Session: RIVET_SESSION_1001\r\n\r\n";

  std::string resp = session.handle_request(record_req);
  EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
  EXPECT_EQ(session.get_state(), RtspState::Recording);
  EXPECT_TRUE(session.is_record_mode());
}

TEST_F(RtspConformanceTest, TeardownTransitionsToInit) {
  std::string_view setup_req = "SETUP rtsps://localhost:8554/live/test/streamid=0 RTSP/1.0\r\n"
                               "CSeq: 30\r\n"
                               "Transport: RTP/AVP;unicast;client_port=8000-8001\r\n\r\n";
  session.handle_request(setup_req);

  std::string_view teardown_req = "TEARDOWN rtsps://localhost:8554/live/test RTSP/1.0\r\n"
                                  "CSeq: 31\r\n"
                                  "Session: RIVET_SESSION_1001\r\n\r\n";

  std::string resp = session.handle_request(teardown_req);
  EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
  EXPECT_EQ(session.get_state(), RtspState::Init);
}

/// ============================================================================
/// RFC 3550 RTP & RTCP Conformance
/// ============================================================================

TEST(RtpConformanceTest, SequenceRolloverAndConsecutiveProbation) {
  SequenceTracker tracker;

  /// Initial packet starts probation
  EXPECT_TRUE(tracker.update(65534).has_value());

  /// Second in-order packet clears probation
  EXPECT_TRUE(tracker.update(65535).has_value());

  /// Rollover across 65535 -> 0
  auto r0 = tracker.update(0);
  ASSERT_TRUE(r0.has_value());
  EXPECT_EQ(*r0, 65536ULL);

  /// Rollover continued to 1
  auto r1 = tracker.update(1);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(*r1, 65537ULL);
}

TEST(RtpConformanceTest, MisorderWithinAllowedWindowAccepted) {
  SequenceTracker tracker;
  EXPECT_TRUE(tracker.update(100).has_value());
  EXPECT_TRUE(tracker.update(101).has_value());

  /// Packet 103 arrives ahead of 102
  auto s103 = tracker.update(103);
  ASSERT_TRUE(s103.has_value());
  EXPECT_EQ(*s103, 103ULL);

  /// Late packet 102 arrives within window
  auto s102 = tracker.update(102);
  ASSERT_TRUE(s102.has_value());
  EXPECT_EQ(*s102, 102ULL);

  /// Duplicate of late packet 102 is rejected
  auto dup102 = tracker.update(102);
  EXPECT_FALSE(dup102.has_value());
}

TEST(RtpConformanceTest, LargeSequenceJumpRequiresConsecutiveSync) {
  SequenceTracker tracker;
  EXPECT_TRUE(tracker.update(10).has_value());
  EXPECT_TRUE(tracker.update(11).has_value());

  /// Jump by 10000 (exceeds MAX_DROPOUT = 3000)
  auto jump1 = tracker.update(10011);
  EXPECT_FALSE(jump1.has_value()); //< First unexpected jump packet rejected

  /// Second sequential packet confirming source restart/jump
  auto jump2 = tracker.update(10012);
  EXPECT_TRUE(jump2.has_value()); //< Now accepted and resynced
}

TEST(RtpConformanceTest, JitterEstimationMatchesRfcFormula) {
  JitterEstimator estimator;

  /// Packet 1 arrives at T=10000 with RTP ts=90000
  double j1 = estimator.update(90000, 10000);
  EXPECT_DOUBLE_EQ(j1, 0.0);

  /// Packet 2 delayed by 160 timestamp units (arrival clock advanced by 160 more than RTP clock)
  /// D = (10000 + 3000 + 160 - (90000 + 3000)) - (10000 - 90000) = 160
  /// J = J + (|D| - J) / 16 = 0 + (160 - 0) / 16 = 10.0
  double j2 = estimator.update(93000, 13160);
  EXPECT_DOUBLE_EQ(j2, 10.0);
}

TEST(RtpConformanceTest, RtcpReceiverReportFormat) {
  RtcpReportGenerator gen(0x11223344);

  ReceiverReportBlock block;
  block.ssrc                = 0xAABBCCDD;
  block.fraction_lost       = 5;
  block.cumulative_lost     = 120;
  block.highest_seq_recv    = 65540;
  block.jitter              = 15;
  block.last_sr_timestamp   = 0x12345678;
  block.delay_since_last_sr = 250;

  auto rr = gen.build_rr(block);
  EXPECT_EQ(rr.size(), 32u);
  /// Version 2, PT 201
  EXPECT_EQ(rr[0], 0x81);
  EXPECT_EQ(rr[1], 201);
  EXPECT_EQ(rr[2], 0);
  EXPECT_EQ(rr[3], 7); //< Length = 7 words
  /// Reporter SSRC
  EXPECT_EQ(rr[4], 0x11);
  EXPECT_EQ(rr[5], 0x22);
  EXPECT_EQ(rr[6], 0x33);
  EXPECT_EQ(rr[7], 0x44);
  /// Target SSRC
  EXPECT_EQ(rr[8], 0xAA);
  EXPECT_EQ(rr[9], 0xBB);
  EXPECT_EQ(rr[10], 0xCC);
  EXPECT_EQ(rr[11], 0xDD);
  /// Fraction lost
  EXPECT_EQ(rr[12], 5);
}
