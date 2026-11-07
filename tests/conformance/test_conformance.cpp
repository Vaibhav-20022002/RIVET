#include "rivet/proto/rtp/JitterEstimator.hpp"
#include "rivet/proto/rtp/RtcpReportGenerator.hpp"
#include "rivet/proto/rtp/SequenceTracker.hpp"
#include "rivet/proto/rtsp/RtspControlSession.hpp"
#include "tests/support/RtspClient.hpp"
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

  void announce_h264_stream() {
    const std::string sdp = "v=0\r\n"
                            "o=- 0 0 IN IP4 127.0.0.1\r\n"
                            "s=ConformanceStream\r\n"
                            "t=0 0\r\n"
                            "m=video 0 RTP/AVP 96\r\n"
                            "a=rtpmap:96 H264/90000\r\n"
                            "a=control:streamid=0\r\n";
    const std::string req = "ANNOUNCE rtsp://<localhost:8554/live/test RTSP/1.0\r\n"
                            "CSeq: 1\r\n"
                            "Content-Type: application/sdp\r\n"
                            "Content-Length: " +
        std::to_string(sdp.size()) + "\r\n\r\n" + sdp;
    const std::string resp = session.handle_request(req);
    ASSERT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
  }
};

TEST_F(RtspConformanceTest, OptionsReturnsPublicMethodsAndCSeq) {
  std::string_view req = "OPTIONS rtsp://<localhost:8554/live/test RTSP/1.0\r\n"
                         "CSeq: 1\r\n"
                         "User-Agent: ConformanceClient\r\n\r\n";

  std::string resp = session.handle_request(req);
  EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
  EXPECT_NE(resp.find("CSeq: 1"), std::string::npos);
  EXPECT_NE(
      resp.find(
          "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, ANNOUNCE, RECORD, GET_PARAMETER"),
      std::string::npos);
  EXPECT_EQ(resp.find("PAUSE"), std::string::npos);
  EXPECT_EQ(session.get_state(), RtspState::Init);
}

TEST_F(RtspConformanceTest, DescribeReturns404BeforeAnnounceAndValidSdpAfterAnnounce) {
  /// DESCRIBE before ANNOUNCE returns 404 Not Found (never fake H.264 SDP)
  std::string_view req = "DESCRIBE rtsp://<localhost:8554/live/test RTSP/1.0\r\n"
                         "CSeq: 2\r\n"
                         "Accept: application/sdp\r\n\r\n";

  std::string resp_before = session.handle_request(req);
  EXPECT_NE(resp_before.find("RTSP/1.0 404 Not Found"), std::string::npos);

  /// After ANNOUNCE, DESCRIBE returns 200 OK with RFC 4566 `c=IN IP4 0.0.0.0`
  announce_h264_stream();
  std::string resp = session.handle_request(req);
  EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
  EXPECT_NE(resp.find("CSeq: 2"), std::string::npos);
  EXPECT_NE(resp.find("Content-Type: application/sdp"), std::string::npos);
  EXPECT_NE(resp.find("Content-Length:"), std::string::npos);
  EXPECT_NE(resp.find("c=IN IP4 0.0.0.0"), std::string::npos);
  EXPECT_NE(resp.find("m=video 0 RTP/AVP 96"), std::string::npos);
}

TEST_F(RtspConformanceTest, SetupDedicatedUdpPortsTransitionsToReady) {
  announce_h264_stream();
  std::string_view req = "SETUP rtsp://<localhost:8554/live/test/streamid=0 RTSP/1.0\r\n"
                         "CSeq: 3\r\n"
                         "Transport: RTP/AVP;unicast;client_port=5000-5001\r\n\r\n";

  std::string resp = session.handle_request(req);
  EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
  EXPECT_NE(resp.find("CSeq: 3"), std::string::npos);
  EXPECT_NE(
      resp.find("Transport: RTP/AVP/UDP;unicast;client_port=5000-5001;server_port=20000-20001"),
      std::string::npos);
  EXPECT_NE(resp.find("Session:"), std::string::npos);
  EXPECT_FALSE(RtspTestClient::extract_session_header(resp).empty());
  EXPECT_EQ(session.get_state(), RtspState::Ready);
  EXPECT_FALSE(session.is_interleaved_mode());
  EXPECT_EQ(session.get_client_rtp_port(), 5000);
  EXPECT_EQ(session.get_client_rtcp_port(), 5001);
}

TEST_F(RtspConformanceTest, SetupInterleavedTcpModeParsesChannel) {
  announce_h264_stream();
  std::string_view req = "SETUP rtsp://<localhost:8554/live/test/streamid=0 RTSP/1.0\r\n"
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
  session.set_announced_sdp("v=0\r\n"
                            "o=- 0 0 IN IP4 127.0.0.1\r\n"
                            "s=ConformanceStream\r\n"
                            "t=0 0\r\n"
                            "m=video 0 RTP/AVP 96\r\n"
                            "a=rtpmap:96 H264/90000\r\n"
                            "a=control:streamid=0\r\n");
  session.set_play_rtp_info(
      {PlayTrackRtpInfo{0, "rtsp://<localhost:8554/live/test/streamid=0", true, 42, 90000}});

  /// First SETUP and dynamically extract crypto-random `Session:` token
  std::string_view  setup_req  = "SETUP rtsp://<localhost:8554/live/test/streamid=0 RTSP/1.0\r\n"
                                 "CSeq: 10\r\n"
                                 "Transport: RTP/AVP;unicast;client_port=6000-6001\r\n\r\n";
  const std::string setup_resp = session.handle_request(setup_req);
  const std::string sid        = RtspTestClient::extract_session_header(setup_resp);
  ASSERT_FALSE(sid.empty());

  const std::string play_req = "PLAY rtsp://<localhost:8554/live/test RTSP/1.0\r\n"
                               "CSeq: 11\r\n"
                               "Session: " +
      sid + "\r\n\r\n";

  std::string resp = session.handle_request(play_req);
  EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
  EXPECT_NE(resp.find("Range: npt=0.000-"), std::string::npos);
  EXPECT_NE(
      resp.find("RTP-Info: url=rtsp://<localhost:8554/live/test/streamid=0;seq=42;rtptime=90000"),
      std::string::npos);
  EXPECT_EQ(session.get_state(), RtspState::Playing);
}

TEST_F(RtspConformanceTest, RecordTransitionsToRecording) {
  announce_h264_stream();
  std::string_view setup_req =
      "SETUP rtsp://<localhost:8554/live/test/streamid=0 RTSP/1.0\r\n"
      "CSeq: 20\r\n"
      "Transport: RTP/AVP;unicast;client_port=7000-7001;mode=record\r\n\r\n";
  const std::string setup_resp = session.handle_request(setup_req);
  const std::string sid        = RtspTestClient::extract_session_header(setup_resp);
  ASSERT_FALSE(sid.empty());

  const std::string record_req = "RECORD rtsp://<localhost:8554/live/test RTSP/1.0\r\n"
                                 "CSeq: 21\r\n"
                                 "Session: " +
      sid + "\r\n\r\n";

  std::string resp = session.handle_request(record_req);
  EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
  EXPECT_EQ(session.get_state(), RtspState::Recording);
  EXPECT_TRUE(session.is_record_mode());
}

TEST_F(RtspConformanceTest, TeardownTransitionsToInit) {
  announce_h264_stream();
  std::string_view  setup_req  = "SETUP rtsp://<localhost:8554/live/test/streamid=0 RTSP/1.0\r\n"
                                 "CSeq: 30\r\n"
                                 "Transport: RTP/AVP;unicast;client_port=8000-8001\r\n\r\n";
  const std::string setup_resp = session.handle_request(setup_req);
  const std::string sid        = RtspTestClient::extract_session_header(setup_resp);
  ASSERT_FALSE(sid.empty());

  const std::string teardown_req = "TEARDOWN rtsp://<localhost:8554/live/test RTSP/1.0\r\n"
                                   "CSeq: 31\r\n"
                                   "Session: " +
      sid + "\r\n\r\n";

  std::string resp = session.handle_request(teardown_req);
  EXPECT_NE(resp.find("RTSP/1.0 200 OK"), std::string::npos);
  EXPECT_EQ(session.get_state(), RtspState::Init);
}

/// ============================================================================
/// RFC 3550 RTP & RTCP Conformance
/// ============================================================================

TEST(RtpConformanceTest, SequenceRolloverAndConsecutiveProbation) {
  SequenceTracker tracker;

  /// RFC 3550 Appendix A.1 (`MIN_SEQUENTIAL = 2`):
  /// Initial packet starts 2-packet probation (`std::nullopt`)
  EXPECT_FALSE(tracker.update(65534).has_value());

  /// Second consecutive packet validates the source and clears probation!
  auto p1 = tracker.update(65535);
  ASSERT_TRUE(p1.has_value());
  EXPECT_EQ(*p1, 65535ULL);

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
  EXPECT_FALSE(tracker.update(100).has_value());
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
  EXPECT_FALSE(tracker.update(10).has_value());
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
