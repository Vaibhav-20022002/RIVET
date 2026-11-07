#include "rivet/config/RivetConfig.hpp"
#include "rivet/core/RivetController.hpp"
#include "tests/support/Eventually.hpp"
#include "tests/support/H264Synth.hpp"
#include "tests/support/RtpBuilder.hpp"
#include "tests/support/RtspClient.hpp"
#include <chrono>
#include <csignal>
#include <gtest/gtest.h>
#include <sys/types.h>
#include <unistd.h>

using namespace rivet::core;
using namespace rivet::config;
using namespace rivet::test;

namespace {

std::string make_mp_sdp(std::string_view title) {
  return "v=0\r\n"
         "o=- 0 0 IN IP4 127.0.0.1\r\n"
         "s=" +
      std::string(title) +
      "\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "t=0 0\r\n"
      "m=video 0 RTP/AVP 96\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=control:streamid=0\r\n";
}

bool publish_stream(RtspTestClient &pub, uint16_t port, const std::string &path) {
  if (!pub.connect(port)) return false;
  const std::string url = "rtsp://127.0.0.1:" + std::to_string(port) + "/" + path;
  if (pub.request("ANNOUNCE", url, {}, make_mp_sdp(path)).status_code != 200) return false;
  if (pub.request("SETUP",
             url + "/streamid=0",
             "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n")
          .status_code != 200) {
    return false;
  }
  if (pub.request("RECORD", url).status_code != 200) return false;

  /// Send SPS + IDR to exit 2-packet probation
  if (!pub.send_interleaved(
          0, RtpBuilder().seq(1).timestamp(90000).payload(H264Synth::make_sps(24)).build())) {
    return false;
  }
  return pub.send_interleaved(0,
      RtpBuilder().seq(2).timestamp(90000).marker(true).payload(H264Synth::make_idr(128)).build());
}

} //< namespace

/// ============================================================================
/// Process-per-Input-Stream Isolation & Fault Containment
/// ============================================================================

TEST(MultiProcessIsolationTest, ConcurrentStreamsIsolatedAcrossSigkillAndSub50msRecovery) {
  RivetConfig cfg;
  cfg.rtsp_port      = 0;
  cfg.base_rtp_port  = 0;
  cfg.base_rtcp_port = 0;
  cfg.egress_shards  = 1;
  cfg.numa_capacity  = 512;
  cfg.numa_slab_size = 2048;
  cfg.execution_mode = ExecutionMode::MultiProcess;

  RivetController controller(cfg);
  ASSERT_TRUE(controller.start());
  const uint16_t port = controller.get_bound_rtsp_port();
  ASSERT_GT(port, 0u);

  /// 1. Launch two independent streams `/live/cam_a` and `/live/cam_b`
  RtspTestClient pub_a;
  RtspTestClient pub_b;
  ASSERT_TRUE(publish_stream(pub_a, port, "live/cam_a"));
  ASSERT_TRUE(publish_stream(pub_b, port, "live/cam_b"));

  ASSERT_TRUE(eventually([&] {
    const auto ma = controller.query_stream_metrics("live/cam_a");
    const auto mb = controller.query_stream_metrics("live/cam_b");
    return ma.has_value() && ma->ingest_packets >= 2 && mb.has_value() && mb->ingest_packets >= 2;
  }));

  /// Verify distinct OS child processes (`pid_a != pid_b != supervisor getpid()`)!
  const pid_t sup_pid = ::getpid();
  const pid_t pid_a   = controller.worker_pid("live/cam_a");
  const pid_t pid_b   = controller.worker_pid("live/cam_b");
  ASSERT_GT(pid_a, 0);
  ASSERT_GT(pid_b, 0);
  EXPECT_NE(pid_a, sup_pid);
  EXPECT_NE(pid_b, sup_pid);
  EXPECT_NE(pid_a, pid_b);

  /// 2. Unknown stream returns `404 Not Found` even while 2 streams are active!
  RtspTestClient probe;
  ASSERT_TRUE(probe.connect(port));
  const std::string unknown_url = "rtsp://127.0.0.1:" + std::to_string(port) + "/live/unknown";
  EXPECT_EQ(probe.request("DESCRIBE", unknown_url).status_code, 404);
  probe.close_all();

  /// 3. Attach a live viewer to `/live/cam_b`
  RtspTestClient viewer_b;
  ASSERT_TRUE(viewer_b.connect(port));
  const std::string url_b = "rtsp://127.0.0.1:" + std::to_string(port) + "/live/cam_b";
  ASSERT_EQ(
      viewer_b
          .request(
              "SETUP", url_b + "/streamid=0", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
          .status_code,
      200);
  ASSERT_EQ(viewer_b.request("PLAY", url_b).status_code, 200);
  /// Drain initial fast-start burst (`[SPS, IDR]` = 2 RTP packets) on `/live/cam_b`
  size_t fast_start_rtp = 0;
  while (fast_start_rtp < 2) {
    auto got = viewer_b.recv_interleaved(2 - fast_start_rtp, 1500);
    if (got.empty()) break;
    for (const auto &f : got) {
      if (f.channel == 0) ++fast_start_rtp;
    }
  }
  ASSERT_EQ(fast_start_rtp, 2u);

  /// 4. Hard crash `/live/cam_a` worker with `SIGKILL`!
  const auto crash_t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(::kill(pid_a, SIGKILL), 0);

  /// Simultaneously stream 20 live frames on `/live/cam_b` and verify ZERO packet loss!
  for (uint16_t s = 3; s < 23; ++s) {
    const auto pkt = RtpBuilder()
                         .seq(s)
                         .timestamp(90000U + s * 3000U)
                         .marker(true)
                         .payload(H264Synth::make_p_frame(80))
                         .build();
    ASSERT_TRUE(pub_b.send_interleaved(0, pkt));
  }

  /// Wait for UDS EOF detection & cleanup of `/live/cam_a` (`< 50 ms` on Release, scaled on
  /// sanitizers)
  ASSERT_TRUE(eventually(
      [&] { return !controller.has_stream("live/cam_a"); }, std::chrono::milliseconds(50)));
  const auto detect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - crash_t0)
                             .count();
  EXPECT_LE(detect_ms, 50 * timeout_scale_factor());
  EXPECT_GE(controller.total_restarts(), 1u);

  /// Verify `/live/cam_b` remained on the exact same `pid_b` and delivered all 20 frames intact!
  EXPECT_EQ(controller.worker_pid("live/cam_b"), pid_b);
  size_t rx_b_count = 0;
  while (rx_b_count < 20) {
    auto got = viewer_b.recv_interleaved(20 - rx_b_count, 1500);
    if (got.empty()) break;
    for (const auto &f : got) {
      if (f.channel == 0) ++rx_b_count;
    }
  }
  EXPECT_EQ(rx_b_count, 20u);

  /// 5. `/live/cam_a` immediately recovers by re-announcing into a fresh child worker PID!
  pub_a.close_all();
  RtspTestClient pub_a_recovered;
  ASSERT_TRUE(publish_stream(pub_a_recovered, port, "live/cam_a"));
  ASSERT_TRUE(eventually([&] {
    const auto ma = controller.query_stream_metrics("live/cam_a");
    return ma.has_value() && ma->ingest_packets >= 2;
  }));

  const pid_t new_pid_a = controller.worker_pid("live/cam_a");
  EXPECT_GT(new_pid_a, 0);
  EXPECT_NE(new_pid_a, pid_a);
  EXPECT_NE(new_pid_a, pid_b);

  pub_a_recovered.close_all();
  viewer_b.close_all();
  pub_b.close_all();
  controller.stop();
}
