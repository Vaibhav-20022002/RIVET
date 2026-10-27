#pragma once

#include "rivet/config/RivetConfig.hpp"
#include "rivet/core/Probes.hpp"
#include "rivet/core/RivetWorker.hpp"
#include "rivet/core/StreamRegistry.hpp"
#include "rivet/core/SubscriberSession.hpp"
#include "rivet/memory/NumaArena.hpp"
#include "rivet/memory/PacketRef.hpp"
#include "rivet/proto/rtp/RtpPacketClassifier.hpp"
#include "rivet/proto/rtsp/RtspControlSession.hpp"
#include "rivet/proto/rtsp/RtspStreamFramer.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace rivet::core {

/// Resilience Tier categorization for stream recovery and failover
enum class ResilienceTier : uint8_t {
  Tier1_UpstreamReconnect, //< Upstream link drop: worker survives, downstream timeline preserved
  Tier2_GracefulHandoff,   //< Live migration: quiesce old worker, route new joins to new worker
  Tier3_HardCrashRestart   //< Fatal worker crash: sub-50ms respawn
};

/// RivetController is the master supervisor process controller.
/// Manages TCP RTSP control plane, UDP RTP ingest, dynamic path routing, and multi-tier worker
/// resilience.
class RivetController {
  uint16_t rtsp_port{8554};
  uint16_t base_rtp_port{20000};
  uint16_t base_rtcp_port{20001};

  uint16_t bound_rtsp_port{0};
  uint16_t bound_video_port{0};
  uint16_t bound_audio_port{0};

  /// Fixed-slab memory arena for network packet reception.
  /// MUST be declared before active_workers and registry so that on destruction,
  /// all workers, queues, and packet logs are destroyed and buffers recycled BEFORE the arena!
  memory::NumaArena arena{4096, 2048};

  StreamRegistry registry;

  /// Track active workers by stream_id
  std::unordered_map<std::string, std::shared_ptr<RivetWorker>> active_workers;
  mutable std::mutex                                            workers_mutex;

  std::atomic<bool> is_running{false};

  /// Watchdog timeout limit in milliseconds
  uint32_t watchdog_timeout_ms{5000};

  /// Sockets
  int tcp_server_socket{-1};
  int udp_video_socket{-1};
  int udp_audio_socket{-1};

  /// Background listener threads
  std::thread tcp_thread;
  std::thread udp_thread;

  /// Monotonic subscriber ID generator
  std::atomic<uint64_t> next_subscriber_id{1001};

  struct ClientContext {
    int                             fd{-1};
    std::string                     ip{"127.0.0.1"};
    proto::rtsp::RtspControlSession rtsp_session;
    std::string                     recv_buf;
    bool                            is_publisher{false};
  };

  std::string startup_policy{"FreshnessFirst"};

public:
  explicit RivetController(uint16_t port       = 8554,
      uint32_t                      timeout_ms = 5000,
      std::string                   policy     = "FreshnessFirst",
      uint16_t                      rtp_port   = 20000,
      uint16_t                      rtcp_port  = 20001,
      size_t                        arena_cap  = 4096,
      size_t                        arena_slab = 2048)
      : rtsp_port(port)
      , base_rtp_port(rtp_port)
      , base_rtcp_port(rtcp_port)
      , arena(arena_cap, arena_slab)
      , watchdog_timeout_ms(timeout_ms)
      , startup_policy(std::move(policy)) {}

  explicit RivetController(const config::RivetConfig &cfg)
      : rtsp_port(cfg.rtsp_port)
      , base_rtp_port(cfg.base_rtp_port)
      , base_rtcp_port(cfg.base_rtcp_port)
      , arena(cfg.numa_capacity, cfg.numa_slab_size)
      , watchdog_timeout_ms(cfg.watchdog_timeout_ms)
      , startup_policy(cfg.startup_policy) {}

  ~RivetController() { stop(); }

  /// Non-copyable, non-movable
  RivetController(const RivetController &)            = delete;
  RivetController &operator=(const RivetController &) = delete;
  RivetController(RivetController &&)                 = delete;
  RivetController &operator=(RivetController &&)      = delete;

  [[nodiscard]] uint16_t get_port() const noexcept { return rtsp_port; }
  [[nodiscard]] uint16_t get_bound_rtsp_port() const noexcept { return bound_rtsp_port; }
  [[nodiscard]] uint16_t get_bound_video_port() const noexcept { return bound_video_port; }
  [[nodiscard]] uint16_t get_bound_audio_port() const noexcept { return bound_audio_port; }
  [[nodiscard]] uint32_t get_watchdog_timeout_ms() const noexcept { return watchdog_timeout_ms; }
  void                   set_watchdog_timeout_ms(uint32_t ms) noexcept { watchdog_timeout_ms = ms; }
  [[nodiscard]] const std::string &get_startup_policy() const noexcept { return startup_policy; }
  void set_startup_policy(std::string policy) noexcept { startup_policy = std::move(policy); }
  [[nodiscard]] StreamRegistry          &get_registry() noexcept { return registry; }
  [[nodiscard]] const StreamRegistry    &get_registry() const noexcept { return registry; }
  [[nodiscard]] memory::NumaArena       &get_arena() noexcept { return arena; }
  [[nodiscard]] const memory::NumaArena &get_arena() const noexcept { return arena; }

  /// Spawns and registers a new RivetWorker for an input stream
  std::shared_ptr<RivetWorker> spawn_worker(const std::string &stream_id,
      uint32_t                                                 egress_shards = 2) {
    auto worker =
        std::make_shared<RivetWorker>(stream_id, egress_shards, base_rtp_port, base_rtcp_port);
    worker->set_udp_sockets(udp_video_socket, udp_audio_socket);
    worker->run();

    {
      std::lock_guard<std::mutex> lk(workers_mutex);
      active_workers[stream_id] = worker;
    }
    registry.register_stream(stream_id, worker);

    std::cout << "[RivetController] Spawned RivetWorker for stream '" << stream_id << "' with "
              << egress_shards << " egress shards.\n";

    return worker;
  }

  /// Tier 2: Quiesces and drains an existing worker while spawning its replacement
  std::shared_ptr<RivetWorker> quiesce_and_drain(const std::string &stream_id) {
    std::shared_ptr<RivetWorker> old_worker;
    {
      std::lock_guard<std::mutex> lk(workers_mutex);
      auto                        it = active_workers.find(stream_id);
      if (it != active_workers.end()) {
        std::cout << "[RivetController] Tier 2 Graceful Handoff initiated for '" << stream_id
                  << "'...\n";
        old_worker = it->second;
        active_workers.erase(it);
      }
    }

    if (old_worker) {
      old_worker->stop();
      registry.unregister_stream(stream_id);
    }

    return spawn_worker(stream_id);
  }

  /// Tier 3: Hard crash restart
  std::shared_ptr<RivetWorker> restart_on_crash(const std::string &stream_id) {
    std::cout << "[RivetController] Tier 3 Hard Crash Recovery: Respawning worker for '"
              << stream_id << "' in sub-50ms...\n";

    {
      std::lock_guard<std::mutex> lk(workers_mutex);
      active_workers.erase(stream_id);
    }
    registry.unregister_stream(stream_id);

    return spawn_worker(stream_id);
  }

  /// Supervises all active streams, performing watchdog health checks
  void supervise() {
    std::lock_guard<std::mutex> lk(workers_mutex);
    for (auto it = active_workers.begin(); it != active_workers.end();) {
      if (!it->second->is_active()) {
        std::cout << "[RivetController] Watchdog detected inactive worker for '" << it->first
                  << "'. Restarting...\n";
        it = active_workers.erase(it);
      } else {
        ++it;
      }
    }
  }

  /// Starts the master supervisor event loops for TCP and UDP
  void start() {
    if (is_running.exchange(true, std::memory_order_acq_rel)) {
      return;
    }

    /// 1. Initialize TCP listening socket on rtsp_port
    tcp_server_socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_server_socket >= 0) {
      int opt = 1;
      ::setsockopt(tcp_server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
      sockaddr_in addr{};
      addr.sin_family      = AF_INET;
      addr.sin_port        = htons(rtsp_port);
      addr.sin_addr.s_addr = INADDR_ANY;

      if (::bind(tcp_server_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[RivetController] Failed to bind TCP port " << rtsp_port << ": "
                  << std::strerror(errno) << "\n";
        ::close(tcp_server_socket);
        tcp_server_socket = -1;
        is_running.store(false, std::memory_order_release);
        return;
      } else if (::listen(tcp_server_socket, 128) < 0) {
        std::cerr << "[RivetController] Failed to listen on TCP port: " << std::strerror(errno)
                  << "\n";
        ::close(tcp_server_socket);
        tcp_server_socket = -1;
        is_running.store(false, std::memory_order_release);
        return;
      } else {
        sockaddr_in baddr{};
        socklen_t   blen = sizeof(baddr);
        if (::getsockname(tcp_server_socket, reinterpret_cast<sockaddr *>(&baddr), &blen) == 0) {
          bound_rtsp_port = ntohs(baddr.sin_port);
        } else {
          bound_rtsp_port = rtsp_port;
        }
        std::cout << "[RivetController] RTSP control plane listening on TCP port "
                  << bound_rtsp_port << "\n";
      }
    }

    /// 2. Initialize UDP ingest sockets for RTP (video on base_rtp_port, audio on base_rtp_port + 2)
    udp_video_socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_video_socket >= 0) {
      int opt = 1;
      ::setsockopt(udp_video_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
      int buf_size = 4 * 1024 * 1024;
      ::setsockopt(udp_video_socket, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
      ::setsockopt(udp_video_socket, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
      sockaddr_in addr{};
      addr.sin_family      = AF_INET;
      addr.sin_port        = htons(base_rtp_port);
      addr.sin_addr.s_addr = INADDR_ANY;
      if (::bind(udp_video_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
        sockaddr_in baddr{};
        socklen_t   blen = sizeof(baddr);
        if (::getsockname(udp_video_socket, reinterpret_cast<sockaddr *>(&baddr), &blen) == 0) {
          bound_video_port = ntohs(baddr.sin_port);
        } else {
          bound_video_port = base_rtp_port;
        }
        std::cout << "[RivetController] Ingest RTP video socket bound to UDP port "
                  << bound_video_port << "\n";
      }
    }

    udp_audio_socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_audio_socket >= 0) {
      int opt = 1;
      ::setsockopt(udp_audio_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
      int buf_size = 4 * 1024 * 1024;
      ::setsockopt(udp_audio_socket, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
      ::setsockopt(udp_audio_socket, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
      sockaddr_in addr{};
      addr.sin_family      = AF_INET;
      addr.sin_port        = htons(static_cast<uint16_t>(base_rtp_port + 2));
      addr.sin_addr.s_addr = INADDR_ANY;
      if (::bind(udp_audio_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
        sockaddr_in baddr{};
        socklen_t   blen = sizeof(baddr);
        if (::getsockname(udp_audio_socket, reinterpret_cast<sockaddr *>(&baddr), &blen) == 0) {
          bound_audio_port = ntohs(baddr.sin_port);
        } else {
          bound_audio_port = static_cast<uint16_t>(base_rtp_port + 2);
        }
        std::cout << "[RivetController] Ingest RTP audio socket bound to UDP port "
                  << bound_audio_port << "\n";
      }
    }

    {
      std::lock_guard<std::mutex> lk(workers_mutex);
      for (auto &pair : active_workers) {
        pair.second->set_udp_sockets(udp_video_socket, udp_audio_socket);
      }
    }

    /// 3. Start listener threads
    tcp_thread = std::thread(&RivetController::run_tcp_listener, this);
    udp_thread = std::thread(&RivetController::run_udp_receiver, this);
  }

  /// Stops all supervised workers and shuts down the controller
  void stop() noexcept {
    if (is_running.exchange(false, std::memory_order_acq_rel)) {
      if (tcp_server_socket >= 0) {
        ::close(tcp_server_socket);
        tcp_server_socket = -1;
      }
      if (udp_video_socket >= 0) {
        ::close(udp_video_socket);
        udp_video_socket = -1;
      }
      if (udp_audio_socket >= 0) {
        ::close(udp_audio_socket);
        udp_audio_socket = -1;
      }

      if (tcp_thread.joinable()) {
        tcp_thread.join();
      }
      if (udp_thread.joinable()) {
        udp_thread.join();
      }

      {
        std::lock_guard<std::mutex> lk(workers_mutex);
        for (auto &pair : active_workers) {
          pair.second->stop();
        }
        active_workers.clear();
      }
      registry.clear();
      std::cout << "[RivetController] All workers stopped cleanly. Supervisor shut down.\n";
    }
  }

  [[nodiscard]] bool active() const noexcept { return is_running.load(std::memory_order_acquire); }

  [[nodiscard]] size_t worker_count() const noexcept {
    std::lock_guard<std::mutex> lk(workers_mutex);
    return active_workers.size();
  }

private:
  /// TCP RTSP Event Loop
  void run_tcp_listener() {
    std::unordered_map<int, ClientContext> clients;

    while (is_running.load(std::memory_order_acquire)) {
      std::vector<pollfd> fds;
      fds.reserve(clients.size() + 1);

      if (tcp_server_socket >= 0) {
        pollfd pfd{};
        pfd.fd     = tcp_server_socket;
        pfd.events = POLLIN;
        fds.push_back(pfd);
      }

      for (const auto &pair : clients) {
        pollfd pfd{};
        pfd.fd     = pair.first;
        pfd.events = POLLIN;
        fds.push_back(pfd);
      }

      int ret = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 100);
      if (ret <= 0) {
        continue;
      }

      size_t fd_idx = 0;

      /// Check for new incoming connections on tcp_server_socket
      if (tcp_server_socket >= 0 && fd_idx < fds.size() && (fds[fd_idx].revents & POLLIN)) {
        sockaddr_in caddr{};
        socklen_t   clen = sizeof(caddr);
        int client_fd    = ::accept(tcp_server_socket, reinterpret_cast<sockaddr *>(&caddr), &clen);

        if (client_fd >= 0) {
          int nodelay = 1;
          ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
          int sockbuf = 4 * 1024 * 1024;
          ::setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sockbuf, sizeof(sockbuf));
          ::setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &sockbuf, sizeof(sockbuf));

          char ip_buf[INET_ADDRSTRLEN] = "127.0.0.1";
          ::inet_ntop(AF_INET, &caddr.sin_addr, ip_buf, sizeof(ip_buf));

          std::string client_ip = ip_buf;
          /// Docker Desktop for Mac/Windows NAT traversal:
          /// The gateway IP (e.g. 192.168.65.1) is not directly reachable for outbound UDP,
          /// whereas host.docker.internal (e.g. 192.168.65.254) routes UDP directly to the host!
          static const std::string host_docker_internal = []() -> std::string {
            addrinfo hints{}, *res = nullptr;
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            if (::getaddrinfo("host.docker.internal", nullptr, &hints, &res) == 0 && res) {
              char  buf[INET_ADDRSTRLEN] = {0};
              auto *sin                  = reinterpret_cast<sockaddr_in *>(res->ai_addr);
              ::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
              ::freeaddrinfo(res);
              return std::string(buf);
            }
            return "";
          }();

          if (!host_docker_internal.empty() && client_ip.rfind("192.168.65.", 0) == 0) {
            client_ip = host_docker_internal;
          }

          ClientContext ctx;
          ctx.fd = client_fd;
          ctx.ip = client_ip;
          ctx.rtsp_session.set_base_ports(base_rtp_port, base_rtcp_port);
          clients[client_fd] = std::move(ctx);
        }
        ++fd_idx;
      } else if (tcp_server_socket >= 0) {
        ++fd_idx;
      }

      /// Check for data from connected RTSP clients
      std::vector<int> dead_clients;

      while (fd_idx < fds.size()) {
        const auto &pfd = fds[fd_idx++];
        if ((pfd.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
          continue;
        }

        auto it = clients.find(pfd.fd);
        if (it == clients.end()) {
          continue;
        }

        auto &ctx = it->second;
        char  read_buf[4096];
        auto  n = ::recv(ctx.fd, read_buf, sizeof(read_buf), 0);

        if (n <= 0) {
          ::close(ctx.fd);
          dead_clients.push_back(ctx.fd);
          continue;
        }

        ctx.recv_buf.append(read_buf, static_cast<size_t>(n));

        while (!ctx.recv_buf.empty()) {
          auto frame =
              proto::rtsp::RtspStreamFramer::next_frame(ctx.recv_buf, arena.get_slab_size());
          if (frame.type == proto::rtsp::FramedMessageType::None) {
            break;
          }
          if (frame.type == proto::rtsp::FramedMessageType::Error) {
            ::close(ctx.fd);
            dead_clients.push_back(ctx.fd);
            break;
          }

          /// 1. Interleaved TCP RTP data
          if (frame.type == proto::rtsp::FramedMessageType::Interleaved) {
            uint8_t     track       = (frame.channel >= 2) ? 1 : 0;
            const auto *payload_ptr = reinterpret_cast<const uint8_t *>(frame.data.data());
            auto        rtp_info =
                proto::rtp::RtpPacketClassifier::classify(payload_ptr, frame.length, track);
            if (rtp_info.valid) {
              auto *slab = arena.allocate();
              if (slab) {
                std::memcpy(slab->payload_data, payload_ptr, frame.length);
                uint64_t now_ns =
                    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                            .count());
                RIVET_PROBE(IngestPacketReceived, now_ns, rtp_info.seq, track);

                auto pkt = memory::PacketRef::adopt(slab,
                    0,
                    frame.length,
                    now_ns,
                    rtp_info.timestamp,
                    rtp_info.seq,
                    track,
                    rtp_info.flags);

                std::shared_ptr<RivetWorker> worker;
                {
                  std::lock_guard<std::mutex> lk(workers_mutex);
                  if (!active_workers.empty()) {
                    worker = active_workers.begin()->second;
                  }
                }

                if (worker) {
                  worker->process_incoming_packet(std::move(pkt));
                }
              }
            }
            continue;
          }

          /// 2. Text RTSP Request
          if (frame.type == proto::rtsp::FramedMessageType::RtspRequest) {
            std::string_view req_view = frame.data;
            std::string      req_url;
            auto             sp1 = req_view.find(' ');
            if (sp1 != std::string_view::npos) {
              auto sp2 = req_view.find(' ', sp1 + 1);
              if (sp2 != std::string_view::npos) {
                req_url = std::string(req_view.substr(sp1 + 1, sp2 - (sp1 + 1)));
              }
            }

            std::shared_ptr<RivetWorker> worker = registry.route(req_url);
            if (!worker) {
              std::lock_guard<std::mutex> lk(workers_mutex);
              if (!active_workers.empty()) {
                worker = active_workers.begin()->second;
              }
            }

            std::cout << "[RTSP] " << req_view.substr(0, req_view.find("\r\n")) << " (" << ctx.ip
                      << ")\n";

            if (req_view.starts_with("ANNOUNCE") || req_view.starts_with("RECORD")) {
              ctx.is_publisher = true;
            } else if (req_view.starts_with("DESCRIBE") || req_view.starts_with("PLAY")) {
              ctx.is_publisher = false;
              if (worker && ctx.rtsp_session.get_announced_sdp().empty()) {
                ctx.rtsp_session.set_announced_sdp(worker->get_sdp());
              }
            }

            std::string resp = ctx.rtsp_session.handle_request(req_view);

            if (req_view.starts_with("ANNOUNCE") && worker) {
              worker->set_sdp(ctx.rtsp_session.get_announced_sdp());
            }

            if (req_view.starts_with("SETUP")) {
              if (!ctx.is_publisher && worker) {
                auto     trans = ctx.rtsp_session.is_interleaved_mode()
                    ? SubscriberTransport::TcpInterleaved
                    : SubscriberTransport::UdpDedicatedPorts;
                uint64_t sid   = next_subscriber_id.fetch_add(1, std::memory_order_relaxed);
                auto     sub   = std::make_unique<SubscriberSession>(sid,
                    ctx.fd,
                    trans,
                    ctx.ip,
                    ctx.rtsp_session.get_client_rtp_port(),
                    ctx.rtsp_session.get_client_rtcp_port(),
                    0x12345678,
                    proto::rtp::SenderMode::TransparentPassThrough);

                if (startup_policy == "FreshnessFirst") {
                  sub->set_startup_policy(std::make_unique<media::FreshnessFirstPolicy>());
                } else {
                  sub->set_startup_policy(std::make_unique<media::FastPictureFirstPolicy>());
                }

                uint8_t track_id = 0;
                if (req_view.find("streamid=1") != std::string_view::npos ||
                    req_view.find("trackID=2") != std::string_view::npos ||
                    req_view.find("track2") != std::string_view::npos) {
                  track_id = 1;
                }
                sub->set_track_id(track_id);

                if (ctx.rtsp_session.is_interleaved_mode()) {
                  sub->set_tcp_channel(ctx.rtsp_session.get_interleaved_channel());
                }

                std::cout << "[RTSP] Registered subscriber #" << sid << " for track "
                          << static_cast<int>(track_id)
                          << " (client_port: " << ctx.rtsp_session.get_client_rtp_port() << ")\n";

                worker->assign_subscriber(std::move(sub));
              }
            }

            ::send(ctx.fd, resp.data(), resp.size(), 0);
          }
        }
      }

      for (int fd : dead_clients) {
        clients.erase(fd);
      }
    }

    for (auto &pair : clients) {
      ::close(pair.first);
    }
    clients.clear();
  }

  /// UDP RTP Ingest Loop
  void run_udp_receiver() {
    pollfd pfd[2];
    pfd[0].fd      = udp_video_socket;
    pfd[0].events  = POLLIN;
    pfd[0].revents = 0;

    pfd[1].fd      = udp_audio_socket;
    pfd[1].events  = POLLIN;
    pfd[1].revents = 0;

    uint8_t buffer[2048];

    while (is_running.load(std::memory_order_acquire)) {
      int ret = ::poll(pfd, 2, 50);
      if (ret <= 0) {
        continue;
      }

      for (int i = 0; i < 2; ++i) {
        if ((pfd[i].revents & POLLIN) && pfd[i].fd >= 0) {
          while (true) {
            sockaddr_in src_addr{};
            socklen_t   src_len = sizeof(src_addr);
            auto        bytes   = ::recvfrom(pfd[i].fd,
                buffer,
                sizeof(buffer),
                MSG_DONTWAIT,
                reinterpret_cast<sockaddr *>(&src_addr),
                &src_len);

            if (bytes <= 0) {
              break; //< Drained all packets currently in kernel buffer
            }

            auto rtp_info = proto::rtp::RtpPacketClassifier::classify(
                buffer, static_cast<size_t>(bytes), static_cast<uint8_t>(i));
            if (rtp_info.valid) {
              auto *slab = arena.allocate();
              if (slab) {
                std::memcpy(slab->payload_data, buffer, static_cast<size_t>(bytes));
                uint64_t now_ns =
                    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                            .count());
                RIVET_PROBE(IngestPacketReceived, now_ns, rtp_info.seq, rtp_info.track_id);

                auto pkt = memory::PacketRef::adopt(slab,
                    0,
                    static_cast<uint32_t>(bytes),
                    now_ns,
                    rtp_info.timestamp,
                    rtp_info.seq,
                    rtp_info.track_id,
                    rtp_info.flags);

                std::shared_ptr<RivetWorker> worker;
                {
                  std::lock_guard<std::mutex> lk(workers_mutex);
                  if (!active_workers.empty()) {
                    worker = active_workers.begin()->second;
                  }
                }

                if (worker) {
                  worker->process_incoming_packet(std::move(pkt));
                }
              }
            }
          }
        }
      }
    }
  }
};

} //< namespace rivet::core
