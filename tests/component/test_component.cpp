#include "rivet/config/RivetConfig.hpp"
#include "rivet/core/RivetController.hpp"
#include "tests/support/ArenaLeakGuard.hpp"
#include "tests/support/Eventually.hpp"
#include "tests/support/H264Synth.hpp"
#include "tests/support/HevcSynth.hpp"
#include "tests/support/RtpBuilder.hpp"
#include "tests/support/RtspClient.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace rivet::core;
using namespace rivet::config;
using namespace rivet::test;

namespace {

std::string make_h264_sdp() {
  return "v=0\r\n"
         "o=- 0 0 IN IP4 127.0.0.1\r\n"
         "s=RIVET H264 Stream\r\n"
         "c=IN IP4 0.0.0.0\r\n"
         "t=0 0\r\n"
         "m=video 0 RTP/AVP 96\r\n"
         "a=rtpmap:96 H264/90000\r\n"
         "a=control:streamid=0\r\n";
}

std::string make_hevc_sdp() {
  return "v=0\r\n"
         "o=- 0 0 IN IP4 127.0.0.1\r\n"
         "s=RIVET HEVC Stream\r\n"
         "c=IN IP4 0.0.0.0\r\n"
         "t=0 0\r\n"
         "m=video 0 RTP/AVP 96\r\n"
         "a=rtpmap:96 H265/90000\r\n"
         "a=control:streamid=0\r\n";
}

bool start_tcp_publisher(RtspTestClient &pub,
    uint16_t                             port,
    const std::string                   &stream_path,
    const std::string                   &sdp) {
  if (!pub.connect(port)) return false;
  const std::string base = "rtsp://<127.0.0.1:" + std::to_string(port) + "/" + stream_path;
  if (pub.request("OPTIONS", base).status_code != 200) return false;
  if (pub.request("ANNOUNCE", base, {}, sdp).status_code != 200) return false;
  if (pub.request("SETUP",
             base + "/streamid=0",
             "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n")
          .status_code != 200) {
    return false;
  }
  return pub.request("RECORD", base).status_code == 200;
}

} //< namespace

class ComponentPipelineTest : public ::testing::Test {
protected:
  void SetUp() override {
    config.rtsp_port           = 0; //< OS-assigned ephemeral port
    config.base_rtp_port       = 0;
    config.base_rtcp_port      = 0;
    config.stream_name         = "live/pipeline";
    config.egress_shards       = 1;
    config.numa_capacity       = 512;
    config.numa_slab_size      = 2048;
    config.watchdog_timeout_ms = 5000;
    config.execution_mode      = ExecutionMode::InProcess;
  }

  RivetConfig config;
};

/// ============================================================================
/// 1. Full Publisher -> RIVET -> Viewer Loopback (TCP Interleaved & UDP, H.264 & HEVC)
/// ============================================================================

TEST_F(ComponentPipelineTest, FullPublishAndTcpViewerLoopbackWithPlayOrderingAndLifecycle) {
  RivetController controller(config);
  ArenaLeakGuard  guard(controller.get_arena());
  ASSERT_TRUE(controller.start());
  const uint16_t    port = controller.get_bound_rtsp_port();
  const std::string url  = "rtsp://<127.0.0.1:" + std::to_string(port) + "/live/pipeline";
  ASSERT_GT(port, 0u);

  /// 1. Publisher connects and publishes initial SPS + PPS + IDR (3 packets clears 2-pkt probation)
  RtspTestClient pub;
  ASSERT_TRUE(start_tcp_publisher(pub, port, "live/pipeline", make_h264_sdp()));

  const auto p_sps = RtpBuilder().seq(10).timestamp(90000).payload(H264Synth::make_sps(24)).build();
  const auto p_pps = RtpBuilder().seq(11).timestamp(90000).payload(H264Synth::make_pps(8)).build();
  const auto p_idr =
      RtpBuilder().seq(12).timestamp(90000).marker(true).payload(H264Synth::make_idr(96)).build();
  ASSERT_TRUE(pub.send_interleaved(0, p_sps));
  ASSERT_TRUE(pub.send_interleaved(0, p_pps));
  ASSERT_TRUE(pub.send_interleaved(0, p_idr));

  ASSERT_TRUE(eventually([&] {
    auto m = controller.query_stream_metrics("live/pipeline");
    return m.has_value() && m->ingest_packets >= 3;
  }));

  /// 2. Viewer connects: DESCRIBE -> SETUP (must NOT emit `$` frames yet!) -> PLAY
  RtspTestClient viewer;
  ASSERT_TRUE(viewer.connect(port));

  const auto desc = viewer.request("DESCRIBE", url, "Accept: application/sdp\r\n");
  EXPECT_EQ(desc.status_code, 200);
  EXPECT_NE(desc.body.find("c=IN IP4 0.0.0.0"), std::string::npos);
  EXPECT_NE(desc.body.find("sprop-parameter-sets="), std::string::npos);

  const auto setup = viewer.request(
      "SETUP", url + "/streamid=0", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
  ASSERT_EQ(setup.status_code, 200);
  EXPECT_FALSE(viewer.session_id().empty());
  /// SETUP must NOT send any media packets before PLAY!
  EXPECT_TRUE(viewer.recv_interleaved(1, /*timeout_ms*/ 60).empty());

  const auto play = viewer.request("PLAY", url);
  ASSERT_EQ(play.status_code, 200);
  /// `200 OK` + `RTP-Info:` MUST arrive on the wire BEFORE the first `$` frame!
  EXPECT_EQ(play.interleaved_frames_before_reply, 0u);
  EXPECT_NE(play.headers.find("RTP-Info:"), std::string::npos);

  /// FastPictureFirst burst + live P-frame delivered intact over TCP `$`
  const auto p_delta =
      RtpBuilder().seq(13).timestamp(93000).marker(true).payload(H264Synth::make_p_frame(64)).build();
  ASSERT_TRUE(pub.send_interleaved(0, p_delta));

  const auto rx_frames = viewer.recv_interleaved(2, 2000);
  ASSERT_GE(rx_frames.size(), 2u);
  EXPECT_EQ(rx_frames[0].channel, 0u);

  /// 3. Viewer TEARDOWN removes subscriber cleanly (`active_subscribers == 0`)
  const auto td = viewer.request("TEARDOWN", url);
  EXPECT_EQ(td.status_code, 200);
  viewer.close_all();

  EXPECT_TRUE(eventually([&] {
    auto m = controller.query_stream_metrics("live/pipeline");
    return m.has_value() && m->active_subscribers == 0;
  }));

  /// 4. Viewer abrupt TCP EOF (close without TEARDOWN) also drops `active_subscribers`
  /// to 0!
  RtspTestClient viewer2;
  ASSERT_TRUE(viewer2.connect(port));
  ASSERT_EQ(
      viewer2
          .request(
              "SETUP", url + "/streamid=0", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
          .status_code,
      200);
  ASSERT_EQ(viewer2.request("PLAY", url).status_code, 200);
  ASSERT_TRUE(eventually([&] {
    auto m = controller.query_stream_metrics("live/pipeline");
    return m.has_value() && m->active_subscribers == 1;
  }));

  viewer2.close_all(); //< Abrupt TCP EOF!
  EXPECT_TRUE(eventually([&] {
    auto m = controller.query_stream_metrics("live/pipeline");
    return m.has_value() && m->active_subscribers == 0;
  }));

  EXPECT_EQ(pub.request("TEARDOWN", url).status_code, 200);
  pub.close_all();
  controller.stop();
}

TEST_F(ComponentPipelineTest, DedicatedUdpPublishAndUdpViewerHevcLoopback) {
  config.execution_mode = ExecutionMode::MultiProcess;
  RivetController controller(config);
  ASSERT_TRUE(controller.start());
  const uint16_t    port = controller.get_bound_rtsp_port();
  const std::string url  = "rtsp://<127.0.0.1:" + std::to_string(port) + "/live/hevc_udp";

  /// 1. UDP Publisher announces HEVC and binds ephemeral client UDP port pair
  RtspTestClient pub;
  ASSERT_TRUE(pub.connect(port));
  ASSERT_TRUE(pub.bind_udp_ports());
  ASSERT_EQ(pub.request("ANNOUNCE", url, {}, make_hevc_sdp()).status_code, 200);

  const std::string pub_transport =
      "Transport: RTP/AVP/UDP;unicast;client_port=" + std::to_string(pub.client_rtp_port()) + "-" +
      std::to_string(pub.client_rtcp_port()) + ";mode=record\r\n";
  ASSERT_EQ(pub.request("SETUP", url + "/streamid=0", pub_transport).status_code, 200);
  ASSERT_GT(pub.server_rtp_port(), 0u);
  ASSERT_EQ(pub.request("RECORD", url).status_code, 200);

  /// Send HEVC AP(VPS+SPS+PPS) + IDR over UDP
  const auto ap  = HevcSynth::make_ap({HevcSynth::make_vps(),
      HevcSynth::make_sps(0, 0, 1, 0, 0, 1, 120, true),
      HevcSynth::make_pps()});
  const auto idr = HevcSynth::make_idr(96);
  ASSERT_TRUE(pub.send_udp_rtp(RtpBuilder().seq(1).timestamp(90000).payload(ap).build()));
  ASSERT_TRUE(
      pub.send_udp_rtp(RtpBuilder().seq(2).timestamp(90000).marker(true).payload(idr).build()));

  ASSERT_TRUE(eventually([&] {
    auto m = controller.query_stream_metrics("live/hevc_udp");
    return m.has_value() && m->ingest_packets >= 2;
  }));

  /// 2. UDP Viewer verifies HEVC SDP (`sprop-vps`, `sprop-sps`, `sprop-pps`, `profile-id`) &
  /// receives UDP RTP
  RtspTestClient viewer;
  ASSERT_TRUE(viewer.connect(port));
  ASSERT_TRUE(viewer.bind_udp_ports());

  const auto desc = viewer.request("DESCRIBE", url);
  ASSERT_EQ(desc.status_code, 200);
  EXPECT_NE(desc.body.find("H265/90000"), std::string::npos);
  EXPECT_NE(desc.body.find("sprop-vps="), std::string::npos);
  EXPECT_NE(desc.body.find("sprop-sps="), std::string::npos);
  EXPECT_NE(desc.body.find("sprop-pps="), std::string::npos);

  const std::string sub_transport =
      "Transport: RTP/AVP/UDP;unicast;client_port=" + std::to_string(viewer.client_rtp_port()) +
      "-" + std::to_string(viewer.client_rtcp_port()) + "\r\n";
  ASSERT_EQ(viewer.request("SETUP", url + "/streamid=0", sub_transport).status_code, 200);
  ASSERT_EQ(viewer.request("PLAY", url).status_code, 200);

  /// Send live HEVC TRAIL_R frame and verify UDP viewer receives burst + live packets
  ASSERT_TRUE(pub.send_udp_rtp(RtpBuilder()
          .seq(3)
          .timestamp(93000)
          .marker(true)
          .payload(HevcSynth::make_trail_r(64))
          .build()));

  const auto udp_pkts = viewer.recv_udp_rtp(2, 2000);
  EXPECT_GE(udp_pkts.size(), 2u);

  viewer.close_all();
  pub.close_all();
  controller.stop();
}

/// ============================================================================
/// 2. Security, Framing & Protocol Guards
/// ============================================================================

TEST_F(ComponentPipelineTest, PublisherSecurityGuardsAndDigestAuthChallenge) {
  config.publish_user     = "admin";
  config.publish_password = "secret_password_42";
  config.execution_mode   = ExecutionMode::MultiProcess;

  RivetController controller(config);
  ASSERT_TRUE(controller.start());
  const uint16_t    port = controller.get_bound_rtsp_port();
  const std::string url  = "rtsp://<127.0.0.1:" + std::to_string(port) + "/live/secure";

  RtspTestClient pub;
  ASSERT_TRUE(pub.connect(port));

  /// 1. Unauthenticated ANNOUNCE rejected with `401 Unauthorized` + `WWW-Authenticate: Digest`
  const auto r401 = pub.request("ANNOUNCE", url, {}, make_h264_sdp());
  ASSERT_EQ(r401.status_code, 401);
  const std::string nonce = RtspTestClient::extract_digest_nonce(r401.headers);
  ASSERT_FALSE(nonce.empty());

  /// 2. Retry ANNOUNCE with RFC 2617 Digest Auth -> `200 OK` across multi-process handoff!
  const auto auth_ann = RtspTestClient::make_digest_auth_header(
      "ANNOUNCE", url, "RIVET", nonce, "admin", "secret_password_42");
  ASSERT_EQ(pub.request("ANNOUNCE", url, auth_ann, make_h264_sdp()).status_code, 200);

  const std::string setup_url = url + "/streamid=0";
  const auto        auth_set  = RtspTestClient::make_digest_auth_header(
      "SETUP", setup_url, "RIVET", nonce, "admin", "secret_password_42");
  ASSERT_EQ(pub.request("SETUP",
                   setup_url,
                   auth_set + "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n")
                .status_code,
      200);

  /// 3. `$` interleaved media sent BEFORE `RECORD` is dropped as unauthorized!
  const auto early_pkt =
      RtpBuilder().seq(1).timestamp(90000).payload(H264Synth::make_idr(32)).build();
  ASSERT_TRUE(pub.send_interleaved(0, early_pkt));
  ASSERT_TRUE(eventually([&] {
    auto m = controller.query_stream_metrics("live/secure");
    return m.has_value() && m->unauthorized_packet_drops >= 1 && m->ingest_packets == 0;
  }));

  /// 4. Transition to `RECORD`, then verify unnegotiated channel (`ch=4`) is dropped
  ///    and RTCP SR on channel 1 (`PT=200`) is NEVER ingested as RTP!
  const auto auth_rec = RtspTestClient::make_digest_auth_header(
      "RECORD", url, "RIVET", nonce, "admin", "secret_password_42");
  ASSERT_EQ(pub.request("RECORD", url, auth_rec).status_code, 200);

  ASSERT_TRUE(pub.send_interleaved(4 /* unnegotiated! */, early_pkt));
  const std::vector<uint8_t> rtcp_sr = {0x80,
      200,
      0x00,
      0x06,
      0x12,
      0x34,
      0x56,
      0x78,
      0,
      0,
      0,
      1,
      0,
      0,
      0,
      2,
      0,
      0,
      0,
      3,
      0,
      0,
      0,
      4,
      0,
      0,
      0,
      5};
  ASSERT_TRUE(pub.send_interleaved(1 /* RTCP channel */, rtcp_sr));

  /// 5. Concurrent second publisher to the active stream is rejected (`455`)!
  RtspTestClient pub2;
  ASSERT_TRUE(pub2.connect(port));
  const auto r2_401 = pub2.request("ANNOUNCE", url, {}, make_h264_sdp());
  const auto nonce2 = RtspTestClient::extract_digest_nonce(r2_401.headers);
  const auto auth2  = RtspTestClient::make_digest_auth_header(
      "ANNOUNCE", url, "RIVET", nonce2, "admin", "secret_password_42");
  EXPECT_EQ(pub2.request("ANNOUNCE", url, auth2, make_h264_sdp()).status_code, 455);
  pub2.close_all();

  auto m = controller.query_stream_metrics("live/secure");
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->ingest_packets, 0u);
  EXPECT_GE(m->unauthorized_packet_drops, 2u);

  pub.close_all();
  controller.stop();
}

TEST_F(ComponentPipelineTest, NegativeContentLengthDoSPayloadDisconnectedImmediately) {
  /// 43-byte `Content-Length: -43` must disconnect attacker immediately without
  /// spinning or starving healthy clients!
  RivetController controller(config);
  ASSERT_TRUE(controller.start());
  const uint16_t port = controller.get_bound_rtsp_port();

  RtspTestClient attacker;
  ASSERT_TRUE(attacker.connect(port));
  /// Exact 43-byte request: header_end (43) + (-43) == 0 under signed wrap!
  const std::string_view exploit = "OPTIONS * RTSP/1.0\r\nContent-Length: -43\r\n\r\n";
  ASSERT_EQ(exploit.size(), 43u);
  ASSERT_TRUE(attacker.send_raw(exploit));
  EXPECT_TRUE(attacker.wait_for_eof(1500));

  /// Healthy client immediately afterwards receives 200 OK
  RtspTestClient healthy;
  ASSERT_TRUE(healthy.connect(port));
  EXPECT_EQ(healthy.request("OPTIONS", "rtsp://<127.0.0.1/live/pipeline").status_code, 200);
  healthy.close_all();

  controller.stop();
}

TEST_F(ComponentPipelineTest, Jumbo60KiBTcpInterleavedFrameDeliveredIntactAndDuplicateBindFails) {
  /// 60 KiB jumbo slab (> 2048 B default slab) + `start() == false` on port collision
  RivetController controller(config);
  ASSERT_TRUE(controller.start());
  const uint16_t    port = controller.get_bound_rtsp_port();
  const std::string url  = "rtsp://<127.0.0.1:" + std::to_string(port) + "/live/jumbo";

  /// Second controller attempting to bind the exact same TCP port fails cleanly
  RivetConfig dup_cfg = config;
  dup_cfg.rtsp_port   = port;
  RivetController dup_controller(dup_cfg);
  EXPECT_FALSE(dup_controller.start());

  RtspTestClient pub;
  ASSERT_TRUE(start_tcp_publisher(pub, port, "live/jumbo", make_h264_sdp()));

  RtspTestClient viewer;
  ASSERT_TRUE(viewer.connect(port));

  /// Prime stream with 1 small SPS packet so 2-packet probation exits when jumbo IDR arrives
  ASSERT_TRUE(pub.send_interleaved(
      0, RtpBuilder().seq(1).timestamp(90000).payload(H264Synth::make_sps(24)).build()));

  /// Build a 60,000-byte single-NAL IDR RTP packet (`> 2048` standard slab!)
  const auto jumbo_idr = H264Synth::make_idr(59980);
  const auto jumbo_rtp =
      RtpBuilder().seq(2).timestamp(90000).marker(true).payload(jumbo_idr).build();
  ASSERT_GT(jumbo_rtp.size(), 59990u);
  ASSERT_TRUE(pub.send_interleaved(0, jumbo_rtp));

  ASSERT_TRUE(eventually([&] {
    auto m = controller.query_stream_metrics("live/jumbo");
    return m.has_value() && m->ingest_packets >= 2;
  }));

  ASSERT_EQ(
      viewer
          .request(
              "SETUP", url + "/streamid=0", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
          .status_code,
      200);
  ASSERT_EQ(viewer.request("PLAY", url).status_code, 200);

  /// FastStartup delivers both packets of the retained RAP AU: [SPS (36B), 60 KiB IDR]
  const auto frames = viewer.recv_interleaved(2, 2500);
  ASSERT_GE(frames.size(), 2u);
  bool saw_60k = false;
  for (const auto &f : frames) {
    if (f.payload.size() == jumbo_rtp.size()) {
      ASSERT_GT(f.payload.size(), 12u);
      /// Compare full 59,980-byte H.264 IDR payload (RTP header seq/SSRC are session-translated)
      EXPECT_TRUE(std::equal(f.payload.begin() + 12, f.payload.end(), jumbo_idr.begin()));
      saw_60k = true;
    }
  }
  EXPECT_TRUE(saw_60k);

  viewer.close_all();
  pub.close_all();
  controller.stop();
}

/// ============================================================================
/// 3. Slow / Stalled TCP Viewer Non-Blocking Isolation
/// ============================================================================

TEST_F(ComponentPipelineTest, StalledSlowTcpViewerEvictedWithoutBlockingHealthyViewer) {
  config.max_subscriber_queue_bytes = 64 * 1024;
  config.max_reactor_egress_bytes   = 48 * 1024; //< Fair quota evicts laggiest viewer quickly
  config.egress_shards              = 1;         //< Both viewers share Shard 0!

  RivetController controller(config);
  ASSERT_TRUE(controller.start());
  const uint16_t    port = controller.get_bound_rtsp_port();
  const std::string url  = "rtsp://<127.0.0.1:" + std::to_string(port) + "/live/stall_test";

  RtspTestClient pub;
  ASSERT_TRUE(start_tcp_publisher(pub, port, "live/stall_test", make_h264_sdp()));
  ASSERT_TRUE(pub.send_interleaved(
      0, RtpBuilder().seq(1).timestamp(90000).payload(H264Synth::make_sps(24)).build()));
  ASSERT_TRUE(pub.send_interleaved(0,
      RtpBuilder().seq(2).timestamp(90000).marker(true).payload(H264Synth::make_idr(128)).build()));

  ASSERT_TRUE(eventually([&] {
    auto m = controller.query_stream_metrics("live/stall_test");
    return m.has_value() && m->ingest_packets >= 2;
  }));

  /// Connect healthy viewer + stalled slow viewer on the SAME EgressReactor shard
  RtspTestClient healthy;
  ASSERT_TRUE(healthy.connect(port));
  ASSERT_EQ(
      healthy
          .request(
              "SETUP", url + "/streamid=0", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
          .status_code,
      200);
  ASSERT_EQ(healthy.request("PLAY", url).status_code, 200);

  RtspTestClient stalled;
  ASSERT_TRUE(stalled.connect(port));
  int tiny_rcv = 4096;
  (void)::setsockopt(stalled.fd(), SOL_SOCKET, SO_RCVBUF, &tiny_rcv, sizeof(tiny_rcv));
  ASSERT_EQ(
      stalled
          .request(
              "SETUP", url + "/streamid=0", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
          .status_code,
      200);
  ASSERT_EQ(stalled.request("PLAY", url).status_code, 200);
  /// `stalled` NEVER reads again!

  /// Pump 320 x 1.4 KiB standard-slab frames (~450 KiB) + 48 x 4 KiB frames (~640 KiB total >
  /// macOS loopback kernel SO_SNDBUF+SO_RCVBUF + 48 KiB shard quota) while `healthy` drains!
  size_t     healthy_received = 0;
  uint16_t   seq              = 3;
  uint32_t   ts               = 93000;
  const auto payload          = H264Synth::make_idr(4000);
  for (int i = 0; i < 256; ++i) {
    const auto rtp = RtpBuilder().seq(seq++).timestamp(ts).marker(true).payload(payload).build();
    ts += 3000;
    ASSERT_TRUE(pub.send_interleaved(0, rtp));
    auto got = healthy.recv_interleaved(1, 20);
    healthy_received += got.size();
  }
  healthy_received += healthy.recv_interleaved(32, 400).size();

  /// Healthy viewer received live stream smoothly without stalling!
  EXPECT_GE(healthy_received, 220u);

  /// Stalled slow viewer was evicted (`slow_subscriber_disconnects >= 1`)!
  EXPECT_TRUE(eventually([&] {
    auto m = controller.query_stream_metrics("live/stall_test");
    return m.has_value() && m->slow_subscriber_disconnects >= 1 && m->active_subscribers == 1;
  }));
  EXPECT_TRUE(stalled.wait_for_eof(1500));

  stalled.close_all();
  healthy.close_all();
  pub.close_all();
  controller.stop();
}
