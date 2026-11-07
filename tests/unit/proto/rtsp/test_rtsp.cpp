#include "rivet/media/Base64.hpp"
#include "rivet/media/StreamDescriptor.hpp"
#include "rivet/proto/rtsp/RtspControlSession.hpp"
#include "rivet/proto/rtsp/RtspDigestAuth.hpp"
#include "rivet/proto/rtsp/RtspStreamFramer.hpp"
#include "rivet/proto/rtsp/SdpNegotiator.hpp"
#include "tests/support/HevcSynth.hpp"
#include "tests/support/RtspClient.hpp"
#include <gtest/gtest.h>
#include <unordered_set>

using namespace rivet::proto::rtsp;

namespace {
std::string valid_h264_sdp() {
  return "v=0\r\n"
         "o=- 0 0 IN IP4 127.0.0.1\r\n"
         "s=Test Stream\r\n"
         "c=IN IP4 0.0.0.0\r\n"
         "t=0 0\r\n"
         "m=video 0 RTP/AVP 96\r\n"
         "a=rtpmap:96 H264/90000\r\n"
         "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z0LAH9gPRA==,aM48gA==\r\n"
         "a=control:streamid=0\r\n";
}
} //< namespace

/// ============================================================================
/// RtspControlSession Tests (RFC 2326 / RFC 7826)
/// ============================================================================

TEST(RtspControlSessionTest, OptionsRequestExcludesUnimplementedPause) {
  RtspControlSession session;
  std::string_view   req  = "OPTIONS rtsp://<localhost:8554/live/test RTSP/1.0\r\nCSeq: 1\r\n\r\n";
  std::string        resp = session.handle_request(req);

  EXPECT_TRUE(resp.starts_with("RTSP/1.0 200 OK\r\n"));
  EXPECT_NE(resp.find("CSeq: 1\r\n"), std::string::npos);
  EXPECT_NE(resp.find("Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, ANNOUNCE, RECORD, "
                      "GET_PARAMETER"),
      std::string::npos);
  EXPECT_EQ(resp.find("PAUSE"), std::string::npos);
}

TEST(RtspControlSessionTest, AnnounceWithoutMediaLineReturns400) {
  RtspControlSession session;
  std::string        no_media_sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=Empty\r\nt=0 0\r\n";
  std::string        ann_req      = "ANNOUNCE rtsp://<localhost:8554/live/test RTSP/1.0\r\n"
                                    "CSeq: 2\r\n"
                                    "Content-Type: application/sdp\r\n"
                                    "Content-Length: " +
      std::to_string(no_media_sdp.size()) + "\r\n\r\n" + no_media_sdp;

  std::string resp = session.handle_request(ann_req);
  EXPECT_TRUE(resp.starts_with("RTSP/1.0 400 Bad Request\r\n"));
}

TEST(RtspControlSessionTest, DescribeBeforeAnnounceReturns404) {
  RtspControlSession session;
  std::string        resp = session.handle_request(
      "DESCRIBE rtsp://<localhost:8554/live/test RTSP/1.0\r\nCSeq: 1\r\n\r\n");
  EXPECT_TRUE(resp.starts_with("RTSP/1.0 404 Not Found\r\n"));
}

TEST(RtspControlSessionTest, AnnounceAndDeclarativeDescribeSdpExchange) {
  RtspControlSession session;
  const std::string  sdp          = valid_h264_sdp();
  std::string        announce_req = "ANNOUNCE rtsp://<localhost:8554/live/test RTSP/1.0\r\n"
                                    "CSeq: 2\r\n"
                                    "Content-Type: application/sdp\r\n"
                                    "Content-Length: " +
      std::to_string(sdp.size()) + "\r\n\r\n" + sdp;

  std::string ann_resp = session.handle_request(announce_req);
  EXPECT_TRUE(ann_resp.starts_with("RTSP/1.0 200 OK\r\n"));
  EXPECT_NE(ann_resp.find("CSeq: 2\r\n"), std::string::npos);

  std::string desc_resp = session.handle_request(
      "DESCRIBE rtsp://<localhost:8554/live/test RTSP/1.0\r\nCSeq: 3\r\n\r\n");
  EXPECT_TRUE(desc_resp.starts_with("RTSP/1.0 200 OK\r\n"));
  EXPECT_NE(desc_resp.find("c=IN IP4 0.0.0.0\r\n"), std::string::npos);
  EXPECT_NE(desc_resp.find("a=rtpmap:96 H264/90000\r\n"), std::string::npos);
  /// SPS Z0LAH9gPRA== decodes to 67 42 C0 1F -> profile-level-id=42c01f (not hardcoded 42e01f!)
  EXPECT_NE(desc_resp.find("profile-level-id=42c01f"), std::string::npos);
  EXPECT_NE(desc_resp.find("a=control:trackID=0\r\n"), std::string::npos);
}

TEST(RtspControlSessionTest, SetupTcpInterleavedMode) {
  RtspControlSession session(20000, 20001);
  session.set_announced_sdp(valid_h264_sdp());

  std::string setup_req = "SETUP rtsp://<localhost:8554/live/test/trackID=0 RTSP/1.0\r\n"
                          "CSeq: 4\r\n"
                          "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n";

  std::string resp = session.handle_request(setup_req);
  EXPECT_TRUE(resp.starts_with("RTSP/1.0 200 OK\r\n"));
  EXPECT_NE(resp.find("Transport: RTP/AVP/TCP;unicast;interleaved=0-1"), std::string::npos);
  EXPECT_NE(resp.find("Session: "), std::string::npos);
  EXPECT_EQ(session.get_session_id().size(), 16u);
  EXPECT_TRUE(session.is_interleaved_mode());
  EXPECT_EQ(session.get_interleaved_channel(), 0u);
  EXPECT_EQ(session.get_state(), RtspState::Ready);
}

TEST(RtspControlSessionTest, SetupUdpDedicatedPortsMode) {
  RtspControlSession session(20000, 20001);
  session.set_announced_sdp(valid_h264_sdp());

  std::string setup_req = "SETUP rtsp://<localhost:8554/live/test/trackID=0 RTSP/1.0\r\n"
                          "CSeq: 5\r\n"
                          "Transport: RTP/AVP/UDP;unicast;client_port=5004-5005\r\n\r\n";

  std::string resp = session.handle_request(setup_req);
  EXPECT_TRUE(resp.starts_with("RTSP/1.0 200 OK\r\n"));
  EXPECT_NE(resp.find("server_port=20000-20001"), std::string::npos);
  EXPECT_FALSE(session.is_interleaved_mode());
  EXPECT_EQ(session.get_client_rtp_port(), 5004u);
  EXPECT_EQ(session.get_client_rtcp_port(), 5005u);
  EXPECT_EQ(session.get_state(), RtspState::Ready);
}

TEST(RtspControlSessionTest, StrictStateValidationAndDynamicSessionLifecycle) {
  RtspControlSession session;
  session.set_announced_sdp(valid_h264_sdp());

  /// SETUP without Transport -> 461 Unsupported Transport
  std::string bad_setup =
      session.handle_request("SETUP rtsp://<localhost:8554/live/test/trackID=0 RTSP/1.0\r\n"
                             "CSeq: 1\r\n\r\n");
  EXPECT_TRUE(bad_setup.starts_with("RTSP/1.0 461 Unsupported Transport\r\n"));

  /// PLAY before SETUP -> 454 Session Not Found
  std::string bad_play = session.handle_request(
      "PLAY rtsp://<localhost:8554/live/test RTSP/1.0\r\nCSeq: 2\r\nSession: deadbeef\r\n\r\n");
  EXPECT_TRUE(bad_play.starts_with("RTSP/1.0 454 Session Not Found\r\n"));

  /// Valid SETUP -> extract dynamic CSPRNG Session ID
  std::string setup_resp =
      session.handle_request("SETUP rtsp://<localhost:8554/live/test/trackID=0 RTSP/1.0\r\n"
                             "CSeq: 3\r\n"
                             "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
  ASSERT_TRUE(setup_resp.starts_with("RTSP/1.0 200 OK\r\n"));
  const std::string sid = rivet::test::RtspTestClient::extract_session_header(setup_resp);
  ASSERT_EQ(sid.size(), 16u);

  /// PLAY with wrong Session -> 454
  std::string wrong_sid =
      session.handle_request("PLAY rtsp://<localhost:8554/live/test RTSP/1.0\r\nCSeq: 4\r\nSession: "
                             "0000000000000000\r\n\r\n");
  EXPECT_TRUE(wrong_sid.starts_with("RTSP/1.0 454 Session Not Found\r\n"));

  /// PLAY with negotiated Session -> 200 OK + RTP-Info
  std::string play_resp = session.handle_request(
      "PLAY rtsp://<localhost:8554/live/test RTSP/1.0\r\nCSeq: 5\r\nSession: " + sid + "\r\n\r\n");
  EXPECT_TRUE(play_resp.starts_with("RTSP/1.0 200 OK\r\n"));
  EXPECT_NE(play_resp.find("RTP-Info: "), std::string::npos);
  EXPECT_EQ(session.get_state(), RtspState::Playing);

  /// GET_PARAMETER keepalive -> 200 OK
  std::string gp_resp = session.handle_request(
      "GET_PARAMETER rtsp://<localhost:8554/live/test RTSP/1.0\r\nCSeq: 6\r\nSession: " + sid +
      "\r\n\r\n");
  EXPECT_TRUE(gp_resp.starts_with("RTSP/1.0 200 OK\r\n"));

  /// TEARDOWN with negotiated Session -> 200 OK + Init
  std::string td_resp = session.handle_request(
      "TEARDOWN rtsp://<localhost:8554/live/test RTSP/1.0\r\nCSeq: 7\r\nSession: " + sid +
      "\r\n\r\n");
  EXPECT_TRUE(td_resp.starts_with("RTSP/1.0 200 OK\r\n"));
  EXPECT_EQ(session.get_state(), RtspState::Init);
}

TEST(RtspControlSessionTest, UnknownAndPauseMethodsReturn501) {
  RtspControlSession session;
  std::string resp = session.handle_request("INVALID /live/test RTSP/1.0\r\nCSeq: 9\r\n\r\n");
  EXPECT_TRUE(resp.starts_with("RTSP/1.0 501 Not Implemented\r\n"));
  EXPECT_NE(resp.find("CSeq: 9\r\n"), std::string::npos);

  std::string pause_resp = session.handle_request("PAUSE /live/test RTSP/1.0\r\nCSeq: 10\r\n\r\n");
  EXPECT_TRUE(pause_resp.starts_with("RTSP/1.0 501 Not Implemented\r\n"));
}

TEST(RtspControlSessionTest, DigestAuthenticationChallengeAndVerify) {
  RtspControlSession session;
  session.set_publish_credentials("publisher", "s3cr3t!");

  const std::string sdp  = valid_h264_sdp();
  std::string       ann1 = "ANNOUNCE rtsp://<localhost:8554/live/secure RTSP/1.0\r\n"
                           "CSeq: 1\r\n"
                           "Content-Type: application/sdp\r\n"
                           "Content-Length: " +
      std::to_string(sdp.size()) + "\r\n\r\n" + sdp;

  std::string resp1 = session.handle_request(ann1);
  ASSERT_TRUE(resp1.starts_with("RTSP/1.0 401 Unauthorized\r\n"));
  const std::string nonce = rivet::test::RtspTestClient::extract_digest_nonce(resp1);
  ASSERT_FALSE(nonce.empty());

  const std::string auth_hdr = rivet::test::RtspTestClient::make_digest_auth_header(
      "ANNOUNCE", "rtsp://<localhost:8554/live/secure", "RIVET", nonce, "publisher", "s3cr3t!");
  std::string ann2 = "ANNOUNCE rtsp://<localhost:8554/live/secure RTSP/1.0\r\n"
                     "CSeq: 2\r\n" +
      auth_hdr +
      "Content-Type: application/sdp\r\n"
      "Content-Length: " +
      std::to_string(sdp.size()) + "\r\n\r\n" + sdp;

  std::string resp2 = session.handle_request(ann2);
  EXPECT_TRUE(resp2.starts_with("RTSP/1.0 200 OK\r\n"));
}

TEST(RtspControlSessionTest, CsprngIdsAreUnique) {
  std::unordered_set<std::string> ids;
  for (int i = 0; i < 64; ++i) {
    auto id = generate_random_hex_id();
    EXPECT_EQ(id.size(), 16u);
    EXPECT_TRUE(ids.insert(id).second);
  }
}

/// ============================================================================
/// SdpNegotiator & StreamDescriptor Security Tests
/// ============================================================================

TEST(SdpNegotiatorTest, SynthesizeH264SdpDerivesRealProfileLevelId) {
  SdpNegotiator negotiator;
  std::string   sdp = negotiator.synthesize_client_sdp(false, "Z0LAH9gPRA==", "aM48gA==");
  EXPECT_NE(sdp.find("c=IN IP4 0.0.0.0\r\n"), std::string::npos);
  EXPECT_NE(sdp.find("m=video 0 RTP/AVP 96\r\n"), std::string::npos);
  EXPECT_NE(sdp.find("a=rtpmap:96 H264/90000\r\n"), std::string::npos);
  EXPECT_NE(sdp.find("profile-level-id=42c01f"), std::string::npos);
  EXPECT_NE(sdp.find("sprop-parameter-sets=Z0LAH9gPRA==,aM48gA=="), std::string::npos);
}

TEST(SdpNegotiatorTest, SynthesizeHevcSdpDerivesRealPtlParameters) {
  const auto vps_b64 =
      rivet::media::base64::encode(rivet::test::HevcSynth::make_vps(0, 1, 1, 0, 120));
  const auto sps_b64 =
      rivet::media::base64::encode(rivet::test::HevcSynth::make_sps(0, 0, 1, 0, 0, 1, 120));
  const auto pps_b64 = rivet::media::base64::encode(rivet::test::HevcSynth::make_pps(0, 0));

  SdpNegotiator negotiator;
  std::string   sdp = negotiator.synthesize_client_sdp(true, sps_b64, pps_b64, vps_b64);
  EXPECT_NE(sdp.find("c=IN IP4 0.0.0.0\r\n"), std::string::npos);
  EXPECT_NE(sdp.find("a=rtpmap:96 H265/90000\r\n"), std::string::npos);
  EXPECT_NE(sdp.find("profile-space=0;profile-id=1;tier-flag=0;level-id=120"), std::string::npos);
  EXPECT_NE(sdp.find("sprop-vps=" + vps_b64), std::string::npos);
  EXPECT_NE(sdp.find("sprop-sps=" + sps_b64), std::string::npos);
  EXPECT_NE(sdp.find("sprop-pps=" + pps_b64), std::string::npos);
}

TEST(SdpNegotiatorTest, ParseCameraSdpRejectsSubstringSpoof) {
  SdpNegotiator negotiator;
  EXPECT_TRUE(negotiator.parse_camera_sdp("v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
                                          "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"));
  EXPECT_TRUE(negotiator.parse_camera_sdp("v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
                                          "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H265/90000\r\n"));
  /// VP8 track with `a=x-note:H264/90000` comment MUST NOT pass as H.264!
  EXPECT_FALSE(negotiator.parse_camera_sdp("v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
                                           "m=video 0 RTP/AVP 96\r\na=rtpmap:96 VP8/90000\r\n"
                                           "a=x-note:H264/90000\r\n"));
}

TEST(StreamDescriptorTest, RejectsControlCharsAndNonSrstTxMode) {
  /// Embedded NUL / control char in SDP rejected
  std::string bad_ctrl = "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
                         "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H265/90000\r\n";
  bad_ctrl.push_back('\x07');
  bad_ctrl.append("\r\n");
  EXPECT_EQ(rivet::media::parse_announce_sdp(bad_ctrl).error, rivet::media::SdpError::Malformed);

  /// RFC 7798 MRST / MRMT rejected with UnsupportedTxMode (415)
  std::string mrst_sdp = "v=0\r\no=- 1 1 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n"
                         "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H265/90000\r\n"
                         "a=fmtp:96 tx-mode=MRST\r\n";
  EXPECT_EQ(
      rivet::media::parse_announce_sdp(mrst_sdp).error, rivet::media::SdpError::UnsupportedTxMode);
}

/// ============================================================================
/// RtspStreamFramer & RtspReadBuffer Security Tests
/// ============================================================================

TEST(RtspStreamFramerTest, ExtractInterleavedFrame) {
  std::string buffer;
  buffer.push_back('$');
  buffer.push_back(0);
  buffer.push_back(0);
  buffer.push_back(4);
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
  buffer.append("PARTIAL");

  auto frame = RtspStreamFramer::next_frame(buffer);
  EXPECT_EQ(frame.type, FramedMessageType::None);
  EXPECT_EQ(buffer.size(), 11u);
}

TEST(RtspStreamFramerTest, OversizedInterleavedFrameAboveCallerCeilingReturnsError) {
  std::string buffer;
  buffer.push_back('$');
  buffer.push_back(0);
  buffer.push_back(0x10); //< 4096 bytes > custom caller ceiling 2048
  buffer.push_back(0x00);
  buffer.append(4096, 'A');

  auto frame = RtspStreamFramer::next_frame(buffer, 2048);
  EXPECT_EQ(frame.type, FramedMessageType::Error);
  EXPECT_TRUE(buffer.empty());
}

TEST(RtspStreamFramerTest, JumboInterleaved60KBAllowedByDefault) {
  RtspReadBuffer rbuf(128 * 1024);
  uint8_t        hdr[4] = {static_cast<uint8_t>('$'),
      0,
      static_cast<uint8_t>((60000U >> 8U) & 0xFFU),
      static_cast<uint8_t>(60000U & 0xFFU)};
  ASSERT_TRUE(rbuf.append(hdr, 4));
  std::vector<uint8_t> payload(60000, 0x5A);
  ASSERT_TRUE(rbuf.append(payload.data(), payload.size()));

  auto view = rbuf.next_view();
  ASSERT_EQ(view.type, FramedMessageType::Interleaved);
  EXPECT_EQ(view.channel, 0u);
  EXPECT_EQ(view.bytes.size(), 60000u);
  EXPECT_EQ(view.wire_bytes, 60004u);
  rbuf.consume(view.wire_bytes);
  EXPECT_TRUE(rbuf.empty());
}

TEST(RtspStreamFramerTest, NegativeContentLength43ByteLivelockReproducerIsError) {
  /// Exact 43-byte negative Content-Length reproducer
  std::string reproducer = "OPTIONS * RTSP/1.0\r\nContent-Length: -43\r\n\r\n";
  ASSERT_EQ(reproducer.size(), 43u);

  auto frame = RtspStreamFramer::next_frame(reproducer);
  EXPECT_EQ(frame.type, FramedMessageType::Error);
  EXPECT_TRUE(reproducer.empty());

  /// Also test wrapping negative values and non-digit garbage
  std::string plus_sign = "OPTIONS * RTSP/1.0\r\nContent-Length: +5\r\n\r\n12345";
  EXPECT_EQ(RtspStreamFramer::next_frame(plus_sign).type, FramedMessageType::Error);

  std::string trailing_junk = "OPTIONS * RTSP/1.0\r\nContent-Length: 5abc\r\n\r\n12345";
  EXPECT_EQ(RtspStreamFramer::next_frame(trailing_junk).type, FramedMessageType::Error);
}

TEST(RtspStreamFramerTest, HugeContentLengthRejectedImmediatelyBeforeBodyWait) {
  std::string huge = "ANNOUNCE rtsp://<h/s RTSP/1.0\r\nContent-Length: 99999999999\r\n\r\n";
  EXPECT_EQ(RtspStreamFramer::next_frame(huge).type, FramedMessageType::Error);

  std::string over_16k = "ANNOUNCE rtsp://<h/s RTSP/1.0\r\nContent-Length: 20000\r\n\r\n";
  EXPECT_EQ(RtspStreamFramer::next_frame(over_16k).type, FramedMessageType::Error);
}

TEST(RtspStreamFramerTest, CaseInsensitiveContentLengthIgnoresFakeHeaderPrefix) {
  /// `X-Content-Length: 9999` must NOT be treated as `Content-Length`!
  std::string req   = "OPTIONS * RTSP/1.0\r\n"
                      "X-Content-Length: 9999\r\n"
                      "content-LENGTH:5\r\n\r\n"
                      "HELLO";
  auto        frame = RtspStreamFramer::next_frame(req);
  ASSERT_EQ(frame.type, FramedMessageType::RtspRequest);
  EXPECT_TRUE(frame.data.ends_with("HELLO"));
  EXPECT_TRUE(req.empty());
}

TEST(RtspStreamFramerTest, SplitByteByByteWithCrPaddingIncrementalScan) {
  RtspReadBuffer rbuf(16384);
  std::string    wire = "OPTIONS rtsp://<127.0.0.1/live RTSP/1.0\r\n"
                        "X-Pad: \r\r\r\r\r\r\r\r\r\r\r\n"
                        "CSeq: 7\r\n\r\n";

  for (size_t i = 0; i + 1 < wire.size(); ++i) {
    ASSERT_TRUE(rbuf.append(&wire[i], 1));
    auto v = rbuf.next_view();
    EXPECT_EQ(v.type, FramedMessageType::None);
  }
  ASSERT_TRUE(rbuf.append(&wire.back(), 1));
  auto v = rbuf.next_view();
  ASSERT_EQ(v.type, FramedMessageType::RtspRequest);
  EXPECT_EQ(v.wire_bytes, wire.size());
}

TEST(RtspStreamFramerTest, SlowlorisDefenseRejectsUnboundedHeader) {
  std::string buffer(RtspStreamFramer::MAX_HEADER_SIZE + 100, 'X');
  auto        frame = RtspStreamFramer::next_frame(buffer);
  EXPECT_EQ(frame.type, FramedMessageType::Error);
  EXPECT_TRUE(buffer.empty());
}
