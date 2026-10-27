#include "rivet/config/RivetConfig.hpp"
#include "rivet/core/RivetController.hpp"
#include "tests/support/ArenaLeakGuard.hpp"
#include "tests/support/Eventually.hpp"
#include "tests/support/H264Synth.hpp"
#include "tests/support/RtpBuilder.hpp"
#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace rivet::core;
using namespace rivet::config;
using namespace rivet::test;

class ComponentPipelineTest : public ::testing::Test {
protected:
  void SetUp() override {
    config.rtsp_port           = 0; //< OS-assigned ephemeral port
    config.base_rtp_port       = 0;
    config.base_rtcp_port      = 0;
    config.stream_name         = "live/pipeline";
    config.egress_shards       = 1;
    config.numa_capacity       = 256;
    config.numa_slab_size      = 2048;
    config.watchdog_timeout_ms = 5000;
  }

  RivetConfig config;
};

static int connect_tcp(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

static std::string send_and_recv_rtsp(int fd, std::string_view req) {
  if (::send(fd, req.data(), req.size(), 0) < 0) {
    return "";
  }
  char buf[4096];
  auto n = ::recv(fd, buf, sizeof(buf) - 1, 0);
  if (n <= 0) return "";
  buf[n] = '\0';
  return std::string(buf, static_cast<size_t>(n));
}

TEST_F(ComponentPipelineTest, FullPublishAndInterleavedIngestLoopback) {
  RivetController controller(config);
  ArenaLeakGuard  guard(controller.get_arena());
  controller.start();
  uint16_t rtsp_port = controller.get_bound_rtsp_port();
  ASSERT_GT(rtsp_port, 0u);

  auto worker = controller.spawn_worker("live/pipeline", 1);
  ASSERT_NE(worker, nullptr);

  /// Background supervisor thread running event loops
  std::atomic<bool> keep_running{true};
  std::thread       worker_thread([&] {
    while (keep_running.load(std::memory_order_relaxed)) {
      controller.supervise();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  /// Connect TCP client
  int client_fd = connect_tcp(rtsp_port);
  ASSERT_GE(client_fd, 0);

  /// 1. OPTIONS
  std::string_view opt_req = "OPTIONS rtsp://127.0.0.1/live/pipeline RTSP/1.0\r\n"
                             "CSeq: 1\r\n\r\n";
  std::string      resp    = send_and_recv_rtsp(client_fd, opt_req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  /// 2. ANNOUNCE
  std::string sdp     = "v=0\r\n"
                        "o=- 0 0 IN IP4 127.0.0.1\r\n"
                        "s=RIVET Stream\r\n"
                        "t=0 0\r\n"
                        "m=video 0 RTP/AVP 96\r\n"
                        "a=rtpmap:96 H264/90000\r\n"
                        "a=control:streamid=0\r\n";
  std::string ann_req = "ANNOUNCE rtsp://127.0.0.1/live/pipeline RTSP/1.0\r\n"
                        "CSeq: 2\r\n"
                        "Content-Type: application/sdp\r\n"
                        "Content-Length: " +
      std::to_string(sdp.size()) + "\r\n\r\n" + sdp;
  resp = send_and_recv_rtsp(client_fd, ann_req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  /// 3. SETUP (Interleaved TCP on channel 0-1)
  std::string_view setup_req = "SETUP rtsp://127.0.0.1/live/pipeline/streamid=0 RTSP/1.0\r\n"
                               "CSeq: 3\r\n"
                               "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n\r\n";
  resp                       = send_and_recv_rtsp(client_fd, setup_req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  /// 4. RECORD
  std::string_view rec_req = "RECORD rtsp://127.0.0.1/live/pipeline RTSP/1.0\r\n"
                             "CSeq: 4\r\n"
                             "Session: RIVET_SESSION_1001\r\n\r\n";
  resp                     = send_and_recv_rtsp(client_fd, rec_req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  /// 5. Send an interleaved TCP packet: '$' (0x24) + channel (0x00) + length (16-bit) + RTP
  auto idr     = H264Synth::make_idr(64);
  auto rtp_pkt = RtpBuilder().seq(1).timestamp(90000).marker(true).payload(idr).build();

  std::vector<uint8_t> interleaved_buf;
  interleaved_buf.push_back('$');
  interleaved_buf.push_back(0); //< Channel 0
  uint16_t plen = htons(static_cast<uint16_t>(rtp_pkt.size()));
  interleaved_buf.push_back(reinterpret_cast<uint8_t *>(&plen)[0]);
  interleaved_buf.push_back(reinterpret_cast<uint8_t *>(&plen)[1]);
  interleaved_buf.insert(interleaved_buf.end(), rtp_pkt.begin(), rtp_pkt.end());

  ssize_t sent = ::send(client_fd, interleaved_buf.data(), interleaved_buf.size(), 0);
  EXPECT_EQ(sent, static_cast<ssize_t>(interleaved_buf.size()));

  /// 6. Wait for packet to be processed by supervisor
  EXPECT_TRUE(eventually([&] { return worker->get_ingest().get_packet_log().size() >= 1; },
      std::chrono::milliseconds(1000)));

  /// 7. TEARDOWN
  std::string_view teardown_req = "TEARDOWN rtsp://127.0.0.1/live/pipeline RTSP/1.0\r\n"
                                  "CSeq: 5\r\n"
                                  "Session: RIVET_SESSION_1001\r\n\r\n";
  resp                          = send_and_recv_rtsp(client_fd, teardown_req);
  EXPECT_NE(resp.find("200 OK"), std::string::npos);

  ::close(client_fd);

  /// Quiesce and stop
  keep_running.store(false, std::memory_order_relaxed);
  worker_thread.join();
  worker->stop();
  controller.stop();
}
