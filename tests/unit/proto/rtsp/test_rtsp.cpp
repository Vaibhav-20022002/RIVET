#include "rivet/proto/rtsp/RtspControlSession.hpp"
#include "rivet/proto/rtsp/RtspStreamFramer.hpp"
#include "rivet/proto/rtsp/SdpNegotiator.hpp"
#include <gtest/gtest.h>

using namespace rivet::proto::rtsp;

/// ============================================================================
/// RtspControlSession Tests
/// ============================================================================

TEST(RtspControlSessionTest, OptionsRequest) {
  RtspControlSession session;
  std::string_view   req  = "OPTIONS rtsps://localhost:8554/live/test RTSP/1.0\r\nCSeq: 1\r\n\r\n";
  std::string        resp = session.handle_request(req);

  EXPECT_TRUE(resp.starts_with("RTSP/1.0 200 OK\r\n"));
  EXPECT_TRUE(resp.find("CSeq: 1\r\n") != std::string::npos);
  EXPECT_TRUE(resp.find("Public: ") != std::string::npos);
}

TEST(RtspControlSessionTest, AnnounceAndDescribeSdpExchange) {
  RtspControlSession session;
  std::string        sdp_body     = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=Test Stream\r\nt=0 0\r\n";
  std::string        announce_req = "ANNOUNCE rtsps://localhost:8554/live/test RTSP/1.0\r\n"
                                    "CSeq: 2\r\n"
                                    "Content-Type: application/sdp\r\n"
                                    "Content-Length: " +
      std::to_string(sdp_body.size()) + "\r\n\r\n" + sdp_body;

  std::string ann_resp = session.handle_request(announce_req);
  EXPECT_TRUE(ann_resp.starts_with("RTSP/1.0 200 OK\r\n"));
  EXPECT_TRUE(ann_resp.find("CSeq: 2\r\n") != std::string::npos);
  EXPECT_EQ(session.get_announced_sdp(), sdp_body);

  /// Subsequent DESCRIBE returns the announced SDP
  std::string desc_req  = "DESCRIBE rtsps://localhost:8554/live/test RTSP/1.0\r\nCSeq: 3\r\n\r\n";
  std::string desc_resp = session.handle_request(desc_req);
  EXPECT_TRUE(desc_resp.starts_with("RTSP/1.0 200 OK\r\n"));
  EXPECT_TRUE(desc_resp.find(sdp_body) != std::string::npos);
}

TEST(RtspControlSessionTest, SetupTcpInterleavedMode) {
  RtspControlSession session(20000, 20001);
  std::string        setup_req = "SETUP rtsps://localhost:8554/live/test/streamid=0 RTSP/1.0\r\n"
                                 "CSeq: 4\r\n"
                                 "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n";

  std::string resp = session.handle_request(setup_req);
  EXPECT_TRUE(resp.starts_with("RTSP/1.0 200 OK\r\n"));
  EXPECT_TRUE(session.is_interleaved_mode());
  EXPECT_EQ(session.get_interleaved_channel(), 0u);
  EXPECT_EQ(session.get_state(), RtspState::Ready);
}

TEST(RtspControlSessionTest, SetupUdpDedicatedPortsMode) {
  RtspControlSession session(20000, 20001);
  std::string        setup_req = "SETUP rtsps://localhost:8554/live/test/streamid=0 RTSP/1.0\r\n"
                                 "CSeq: 5\r\n"
                                 "Transport: RTP/AVP/UDP;unicast;client_port=5004-5005\r\n\r\n";

  std::string resp = session.handle_request(setup_req);
  EXPECT_TRUE(resp.starts_with("RTSP/1.0 200 OK\r\n"));
  EXPECT_FALSE(session.is_interleaved_mode());
  EXPECT_EQ(session.get_client_rtp_port(), 5004u);
  EXPECT_EQ(session.get_client_rtcp_port(), 5005u);
  EXPECT_EQ(session.get_state(), RtspState::Ready);
}

TEST(RtspControlSessionTest, PlayRecordAndTeardownLifecycle) {
  RtspControlSession session;
  session.handle_request("SETUP rtsps://localhost:8554/live/test RTSP/1.0\r\nCSeq: 1\r\n\r\n");

  std::string play_resp =
      session.handle_request("PLAY rtsps://localhost:8554/live/test RTSP/1.0\r\nCSeq: 2\r\n\r\n");
  EXPECT_TRUE(play_resp.starts_with("RTSP/1.0 200 OK\r\n"));
  EXPECT_EQ(session.get_state(), RtspState::Playing);

  std::string td_resp = session.handle_request(
      "TEARDOWN rtsps://localhost:8554/live/test RTSP/1.0\r\nCSeq: 3\r\n\r\n");
  EXPECT_TRUE(td_resp.starts_with("RTSP/1.0 200 OK\r\n"));
  EXPECT_EQ(session.get_state(), RtspState::Init);
}

TEST(RtspControlSessionTest, InvalidMethodReturns400) {
  RtspControlSession session;
  std::string resp = session.handle_request("INVALID /live/test RTSP/1.0\r\nCSeq: 9\r\n\r\n");
  EXPECT_TRUE(resp.starts_with("RTSP/1.0 400 Bad Request\r\n"));
  EXPECT_TRUE(resp.find("CSeq: 9\r\n") != std::string::npos);
}

/// ============================================================================
/// SdpNegotiator Tests
/// ============================================================================

TEST(SdpNegotiatorTest, SynthesizeH264Sdp) {
  SdpNegotiator negotiator;
  std::string   sdp = negotiator.synthesize_client_sdp(false, "Z0LAH9gPRA==", "aM48gA==");
  EXPECT_TRUE(sdp.find("m=video 0 RTP/AVP 96\r\n") != std::string::npos);
  EXPECT_TRUE(sdp.find("a=rtpmap:96 H264/90000\r\n") != std::string::npos);
  EXPECT_TRUE(sdp.find("sprop-parameter-sets=Z0LAH9gPRA==,aM48gA==") != std::string::npos);
}

TEST(SdpNegotiatorTest, SynthesizeHevcSdp) {
  SdpNegotiator negotiator;
  std::string   sdp = negotiator.synthesize_client_sdp(true, "sps==", "pps==", "vps==");
  EXPECT_TRUE(sdp.find("a=rtpmap:96 H265/90000\r\n") != std::string::npos);
  EXPECT_TRUE(sdp.find("sprop-vps=vps==") != std::string::npos);
}

TEST(SdpNegotiatorTest, ParseCameraSdp) {
  SdpNegotiator negotiator;
  EXPECT_TRUE(negotiator.parse_camera_sdp("m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"));
  EXPECT_TRUE(negotiator.parse_camera_sdp("m=video 0 RTP/AVP 96\r\na=rtpmap:96 H265/90000\r\n"));
  EXPECT_TRUE(negotiator.parse_camera_sdp("m=video 0 RTP/AVP 96\r\na=rtpmap:96 HEVC/90000\r\n"));
  EXPECT_FALSE(negotiator.parse_camera_sdp("m=video 0 RTP/AVP 96\r\na=rtpmap:96 VP8/90000\r\n"));
}

/// ============================================================================
/// RtspStreamFramer Tests
/// ============================================================================

TEST(RtspStreamFramerTest, ExtractInterleavedFrame) {
  std::string buffer;
  buffer.push_back('$');
  buffer.push_back(0); //< channel 0
  buffer.push_back(0); //< length hi
  buffer.push_back(4); //< length lo = 4
  buffer.append("TEST");

  auto frame = RtspStreamFramer::next_frame(buffer);
  EXPECT_EQ(frame.type, FramedMessageType::Interleaved);
  EXPECT_EQ(frame.channel, 0u);
  EXPECT_EQ(frame.length, 4u);
  EXPECT_EQ(frame.data, "TEST");
  EXPECT_TRUE(buffer.empty());
}

TEST(RtspStreamFramerTest, IncompleteInterleavedFrameWaits) {
  std::string buffer;
  buffer.push_back('$');
  buffer.push_back(0);
  buffer.push_back(0);
  buffer.push_back(10);
  buffer.append("PARTIAL"); //< only 7 of 10 bytes

  auto frame = RtspStreamFramer::next_frame(buffer);
  EXPECT_EQ(frame.type, FramedMessageType::None);
  EXPECT_EQ(buffer.size(), 11u); //< Buffer retained
}

TEST(RtspStreamFramerTest, OversizedInterleavedFrameReturnsError) {
  std::string buffer;
  buffer.push_back('$');
  buffer.push_back(0);
  buffer.push_back(0x10); //< length = 4096 bytes > max 2048
  buffer.push_back(0x00);
  buffer.append(4096, 'A');

  auto frame = RtspStreamFramer::next_frame(buffer, 2048);
  EXPECT_EQ(frame.type, FramedMessageType::Error);
  EXPECT_TRUE(buffer.empty()); //< Dropped corrupt stream
}

TEST(RtspStreamFramerTest, ExtractRtspTextRequestWithContentLength) {
  std::string body   = "v=0\r\n";
  std::string buffer = "ANNOUNCE rtsps://localhost/test RTSP/1.0\r\n"
                       "Content-Length: " +
      std::to_string(body.size()) + "\r\n\r\n" + body;

  auto frame = RtspStreamFramer::next_frame(buffer);
  EXPECT_EQ(frame.type, FramedMessageType::RtspRequest);
  EXPECT_EQ(frame.data,
      "ANNOUNCE rtsps://localhost/test RTSP/1.0\r\n"
      "Content-Length: " +
          std::to_string(body.size()) + "\r\n\r\n" + body);
  EXPECT_TRUE(buffer.empty());
}

TEST(RtspStreamFramerTest, SlowlorisDefenseRejectsUnboundedHeader) {
  std::string buffer(RtspStreamFramer::MAX_HEADER_SIZE + 100, 'X');
  auto        frame = RtspStreamFramer::next_frame(buffer);
  EXPECT_EQ(frame.type, FramedMessageType::Error);
  EXPECT_TRUE(buffer.empty());
}
