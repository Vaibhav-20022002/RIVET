#pragma once

#include "rivet/config/RivetConfig.hpp"
#include "rivet/core/Log.hpp"
#include "rivet/core/Metrics.hpp"
#include "rivet/core/RivetWorker.hpp"
#include "rivet/core/StreamRegistry.hpp"
#include "rivet/core/WorkerIpc.hpp"
#include "rivet/media/StreamDescriptor.hpp"
#include "rivet/memory/NumaArena.hpp"
#include "rivet/net/EventPoller.hpp"
#include "rivet/net/SocketUtils.hpp"
#include "rivet/proto/rtsp/RtspControlSession.hpp"
#include "rivet/proto/rtsp/RtspDigestAuth.hpp"
#include "rivet/proto/rtsp/RtspStreamFramer.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rivet::core {

/// Resilience Tier categorization for stream recovery and failover
enum class ResilienceTier : uint8_t {
  Tier1_UpstreamReconnect, //< Upstream link drop: worker survives, downstream timeline preserved
  Tier2_GracefulHandoff,   //< Live migration: quiesce old worker, route new joins to new worker
  Tier3_HardCrashRestart   //< Fatal worker crash: sub-50ms detection, cleanup & respawn
};

/// RivetController is the master supervisor process controller.
/// Accepts incoming TCP RTSP connections on a single non-blocking `EventPoller` loop, validates
/// pre-handoff framing / Digest Auth / SDP, spawns isolated `RivetWorker` OS processes (`fork` +
/// `execv("--worker")`) or background `InProcess` workers, and hands off publisher/viewer sockets
/// via `AF_UNIX` `SCM_RIGHTS` with zero steady-state IPC copies.
class RivetController {
  /// Retained first for legacy `get_arena()` / `ArenaLeakGuard` destruction-order safety.
  memory::NumaArena arena;

  config::RivetConfig config_;
  net::EventPoller    poller_;

  int                   tcp_server_socket{-1};
  std::atomic<uint16_t> bound_rtsp_port{0};
  std::atomic<uint16_t> bound_video_port{0};
  std::atomic<uint16_t> bound_audio_port{0};

  std::atomic<bool>     is_running{false};
  std::atomic<uint64_t> next_stream_uid_{1};
  std::atomic<uint32_t> next_worker_slot_{0};
  std::atomic<uint64_t> worker_restarts_{0};
  std::atomic<uint64_t> unauthorized_drops_{0};

  struct SupervisedStream {
    uint64_t                     uid{0};
    std::string                  stream_id;
    uint32_t                     egress_shards{2};
    bool                         pre_spawned{false};
    int                          uds_fd{-1};
    pid_t                        child_pid{-1};
    std::shared_ptr<RivetWorker> inproc_worker;

    std::atomic<bool>     alive{false};
    std::atomic<bool>     quiescing{false};
    std::atomic<bool>     has_publisher{false};
    std::atomic<bool>     is_recording{false};
    std::atomic<uint32_t> config_generation{0};
    std::atomic<uint16_t> video_rtp_port{0};
    std::atomic<uint16_t> video_rtcp_port{0};
    std::atomic<uint16_t> audio_rtp_port{0};
    std::atomic<uint16_t> audio_rtcp_port{0};

    std::mutex              uds_write_mu;
    std::mutex              metrics_mu;
    std::condition_variable metrics_cv;
    uint64_t                metrics_seq{0};
    MetricsSnapshot         latest_metrics{};
  };

  BasicStreamRegistry<std::shared_ptr<SupervisedStream>> stream_table_;
  StreamRegistry                                         registry; //< Synced for InProcess workers

  mutable std::mutex                                                 streams_mutex_;
  std::unordered_map<std::string, std::shared_ptr<SupervisedStream>> active_streams_;
  std::unordered_map<uint64_t, std::shared_ptr<SupervisedStream>>    tag_to_stream_;
  std::vector<std::shared_ptr<SupervisedStream>>                     draining_streams_;
  std::vector<pid_t>                                                 pending_reap_pids_;

  /// Pre-handoff RTSP TCP client state (owned exclusively by `supervisor_thread_`)
  struct PendingClient {
    int                         fd{-1};
    std::string                 ip{"127.0.0.1"};
    uint64_t                    connected_at_ns{0};
    uint64_t                    last_activity_ns{0};
    std::string                 auth_nonce;
    proto::rtsp::RtspReadBuffer read_buf{proto::rtsp::RtspReadBuffer::DEFAULT_CAPACITY};
    std::string                 outbox;
    size_t                      outbox_sent{0};
    bool                        close_after_flush{false};

    [[nodiscard]] bool has_pending_outbox() const noexcept { return outbox_sent < outbox.size(); }
  };

  std::unordered_map<int, PendingClient> pending_clients_;
  std::thread                            supervisor_thread_;

  static constexpr uint64_t kTagListen     = 1ULL;
  static constexpr uint64_t kTagClientBase = 0x1000'0000ULL;
  static constexpr uint64_t kTagWorkerBase = 0x2000'0000ULL;

  [[nodiscard]] static uint64_t make_client_tag(int fd) noexcept {
    return kTagClientBase + static_cast<uint64_t>(static_cast<uint32_t>(fd));
  }

  [[nodiscard]] static int client_fd_from_tag(uint64_t tag) noexcept {
    return static_cast<int>(tag - kTagClientBase);
  }

  [[nodiscard]] static uint64_t make_worker_tag(uint64_t uid) noexcept {
    return kTagWorkerBase + uid;
  }

  [[nodiscard]] static bool transport_requests_record(std::string_view transport) noexcept {
    size_t pos = 0;
    while (pos < transport.size()) {
      const size_t semi = transport.find(';', pos);
      const auto   tok  = media::sdp_detail::trim_ows(transport.substr(
          pos, (semi == std::string_view::npos) ? std::string_view::npos : (semi - pos)));
      pos               = (semi == std::string_view::npos) ? transport.size() : (semi + 1U);
      const auto eq     = tok.find('=');
      if (eq == std::string_view::npos) {
        continue;
      }
      const auto k = media::sdp_detail::trim_ows(tok.substr(0, eq));
      auto       v = media::sdp_detail::trim_ows(tok.substr(eq + 1U));
      if (v.size() >= 2U && v.front() == '"' && v.back() == '"') {
        v = v.substr(1U, v.size() - 2U);
      }
      if (media::sdp_detail::iequals(k, "mode") &&
          (media::sdp_detail::iequals(v, "record") || media::sdp_detail::iequals(v, "receive"))) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] static std::string format_rtsp_error(int code,
      std::string_view                                   reason,
      std::string_view                                   cseq,
      std::string_view                                   extra_headers = {}) {
    std::string r = "RTSP/1.0 " + std::to_string(code) + " ";
    r.append(reason);
    r += "\r\n";
    if (!cseq.empty()) {
      r += "CSeq: ";
      r.append(cseq);
      r += "\r\n";
    }
    if (!extra_headers.empty()) {
      r.append(extra_headers);
    }
    r += "Content-Length: 0\r\n\r\n";
    return r;
  }

  void try_reap_pid_nonblocking(pid_t pid) {
    if (pid <= 0) {
      return;
    }
    int         status = 0;
    const pid_t rc     = ::waitpid(pid, &status, WNOHANG);
    if (rc == 0) {
      std::lock_guard<std::mutex> lk(streams_mutex_);
      pending_reap_pids_.push_back(pid);
    }
  }

  void sweep_pending_reap_pids() noexcept {
    std::lock_guard<std::mutex> lk(streams_mutex_);
    for (auto it = pending_reap_pids_.begin(); it != pending_reap_pids_.end();) {
      int         status = 0;
      const pid_t rc     = ::waitpid(*it, &status, WNOHANG);
      if (rc == *it || (rc < 0 && errno == ECHILD)) {
        it = pending_reap_pids_.erase(it);
      } else {
        ++it;
      }
    }
  }

  static void wait_or_kill_child(pid_t pid, uint32_t grace_ms = 500) noexcept {
    if (pid <= 0) {
      return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(grace_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      int         status = 0;
      const pid_t rc     = ::waitpid(pid, &status, WNOHANG);
      if (rc == pid || (rc < 0 && errno == ECHILD)) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    (void)::kill(pid, SIGKILL);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }

  void shutdown_supervised_stream(const std::shared_ptr<SupervisedStream> &st) noexcept {
    if (!st) {
      return;
    }
    st->alive.store(false, std::memory_order_release);
    {
      std::lock_guard<std::mutex> mlk(st->metrics_mu);
      st->metrics_cv.notify_all();
    }

    int fd_to_close = -1;
    {
      std::lock_guard<std::mutex> wlk(st->uds_write_mu);
      if (st->uds_fd >= 0) {
        poller_.remove(st->uds_fd);
        (void)ipc::send_ipc_message(st->uds_fd, ipc::IpcMsgType::Shutdown);
        fd_to_close = st->uds_fd;
        st->uds_fd  = -1;
      }
    }
    if (fd_to_close >= 0) {
      ::close(fd_to_close);
    }

    if (st->inproc_worker) {
      st->inproc_worker->stop();
      st->inproc_worker.reset();
    }
    if (st->child_pid > 0) {
      wait_or_kill_child(st->child_pid, 500);
      st->child_pid = -1;
    }
  }

  /// Launches either an isolated `--worker` child OS process (`fork` + `execv`) or an `InProcess`
  /// background `RivetWorker`, waits for `WorkerReady` over the UDS socketpair, and registers it.
  std::shared_ptr<SupervisedStream> spawn_supervised_stream(std::string_view raw_stream_id,
      uint32_t                                                               egress_shards,
      bool                                                                   pre_spawned) {
    const std::string norm_id = StreamRegistry::extract_stream_path(raw_stream_id);
    if (norm_id.empty()) {
      return nullptr;
    }

    /// Return existing alive stream if already active
    {
      std::lock_guard<std::mutex> lk(streams_mutex_);
      auto                        it = active_streams_.find(norm_id);
      if (it != active_streams_.end() && it->second &&
          it->second->alive.load(std::memory_order_acquire)) {
        if (pre_spawned) {
          it->second->pre_spawned = true;
        }
        return it->second;
      }
    }

    int sv[2] = {-1, -1};
    if (!ipc::create_uds_pair(sv)) {
      RIVET_LOG_ERROR("RivetController", "Failed to create AF_UNIX UDS socketpair");
      return nullptr;
    }

    config::RivetConfig worker_cfg = config_;
    worker_cfg.stream_name         = norm_id;
    worker_cfg.egress_shards       = egress_shards;
    if (config_.base_rtp_port != 0) {
      const uint32_t slot      = next_worker_slot_.fetch_add(1, std::memory_order_relaxed);
      const uint32_t port_calc = static_cast<uint32_t>(config_.base_rtp_port) + (slot * 32U);
      if (port_calc + 30U < 65535U) {
        worker_cfg.base_rtp_port  = static_cast<uint16_t>(port_calc);
        worker_cfg.base_rtcp_port = static_cast<uint16_t>(port_calc + 1U);
      } else {
        worker_cfg.base_rtp_port  = 0;
        worker_cfg.base_rtcp_port = 0;
      }
    } else {
      worker_cfg.base_rtp_port  = 0;
      worker_cfg.base_rtcp_port = 0;
    }

    auto st           = std::make_shared<SupervisedStream>();
    st->uid           = next_stream_uid_.fetch_add(1, std::memory_order_relaxed);
    st->stream_id     = norm_id;
    st->egress_shards = egress_shards;
    st->pre_spawned   = pre_spawned;
    st->uds_fd        = sv[0];

    if (config_.execution_mode == config::ExecutionMode::InProcess) {
      st->inproc_worker = std::make_shared<RivetWorker>(norm_id, worker_cfg);
      st->inproc_worker->start_background(sv[1]); //< Worker takes ownership of sv[1]
      sv[1] = -1;
    } else {
      std::string exe_path = config_.worker_executable.empty() ? net::resolve_self_executable()
                                                               : config_.worker_executable;
      if (exe_path.empty()) {
        RIVET_LOG_ERROR("RivetController", "Failed to resolve worker executable path");
        ::close(sv[0]);
        ::close(sv[1]);
        return nullptr;
      }

      /// Pre-build all argv strings BEFORE `fork()` so the child performs ZERO heap allocations
      /// between `fork()` and `execv()` (async-signal-safe multi-threaded fork discipline).
      std::vector<std::string> args_storage;
      args_storage.reserve(32);
      args_storage.push_back(exe_path);
      args_storage.push_back("--worker");
      args_storage.push_back("--uds-fd");
      args_storage.push_back(std::to_string(sv[1]));
      args_storage.push_back("--stream");
      args_storage.push_back(norm_id);
      args_storage.push_back("--shards");
      args_storage.push_back(std::to_string(worker_cfg.egress_shards));
      args_storage.push_back("--rtp-port");
      args_storage.push_back(std::to_string(worker_cfg.base_rtp_port));
      args_storage.push_back("--rtcp-port");
      args_storage.push_back(std::to_string(worker_cfg.base_rtcp_port));
      args_storage.push_back("--startup-policy");
      args_storage.push_back(worker_cfg.startup_policy);
      args_storage.push_back("--numa-capacity");
      args_storage.push_back(std::to_string(worker_cfg.numa_capacity));
      args_storage.push_back("--retention-bytes");
      args_storage.push_back(std::to_string(worker_cfg.retention_budget_bytes));
      args_storage.push_back("--burst-bytes");
      args_storage.push_back(std::to_string(worker_cfg.burst_budget_bytes));
      args_storage.push_back("--sub-queue-bytes");
      args_storage.push_back(std::to_string(worker_cfg.max_subscriber_queue_bytes));
      args_storage.push_back("--client-timeout-ms");
      args_storage.push_back(std::to_string(worker_cfg.client_timeout_ms));
      args_storage.push_back("--join-open-irap");
      args_storage.push_back(worker_cfg.join_on_open_irap ? "1" : "0");
      args_storage.push_back("--rlimit-mb");
      args_storage.push_back(std::to_string(worker_cfg.worker_rlimit_as_mb));
      if (!worker_cfg.publish_user.empty()) {
        args_storage.push_back("--pub-user");
        args_storage.push_back(worker_cfg.publish_user);
        args_storage.push_back("--pub-pass");
        args_storage.push_back(worker_cfg.publish_password);
      }

      std::vector<char *> argv_ptrs;
      argv_ptrs.reserve(args_storage.size() + 1);
      for (auto &s : args_storage) {
        argv_ptrs.push_back(s.data());
      }
      argv_ptrs.push_back(nullptr);

      const int   child_uds_fd = sv[1];
      const char *exe_cstr     = exe_path.c_str();
      const pid_t pid          = ::fork();
      if (pid < 0) {
        RIVET_LOG_ERROR(
            "RivetController", "fork() failed for stream '", norm_id, "': ", std::strerror(errno));
        ::close(sv[0]);
        ::close(sv[1]);
        return nullptr;
      }
      if (pid == 0) {
        /// Child OS process: clear FD_CLOEXEC ONLY on `child_uds_fd`, then `execv` immediately.
        (void)net::clear_cloexec(child_uds_fd);
        ::execv(exe_cstr, argv_ptrs.data());
        ::_exit(127);
      }

      /// Parent supervisor: close child's end of socketpair immediately
      ::close(sv[1]);
      sv[1]         = -1;
      st->child_pid = pid;
    }

    /// Wait for `WorkerReady` handshake on `st->uds_fd` before exposing `st->uds_fd` to `poller_`
    bool ready_ok = false;
    if (ipc::detail::wait_fd_ready(st->uds_fd, POLLIN, 3000)) {
      ipc::IpcMsgHeader    hdr{};
      std::vector<uint8_t> payload;
      int                  extra_fd = -1;
      const auto           rc       = ipc::recv_ipc_message(st->uds_fd, hdr, payload, extra_fd);
      if (extra_fd >= 0) {
        ::close(extra_fd);
      }
      if (rc == ipc::IpcRecvStatus::Ok &&
          static_cast<ipc::IpcMsgType>(hdr.type) == ipc::IpcMsgType::WorkerReady &&
          payload.size() >= sizeof(ipc::WorkerReadyPayload)) {
        ipc::WorkerReadyPayload rdy{};
        std::memcpy(&rdy, payload.data(), sizeof(rdy));
        st->video_rtp_port.store(rdy.video_rtp_port, std::memory_order_relaxed);
        st->video_rtcp_port.store(rdy.video_rtcp_port, std::memory_order_relaxed);
        st->audio_rtp_port.store(rdy.audio_rtp_port, std::memory_order_relaxed);
        st->audio_rtcp_port.store(rdy.audio_rtcp_port, std::memory_order_relaxed);
        uint16_t expected_zero = 0;
        (void)bound_video_port.compare_exchange_strong(expected_zero, rdy.video_rtp_port);
        expected_zero = 0;
        (void)bound_audio_port.compare_exchange_strong(expected_zero, rdy.audio_rtp_port);
        ready_ok = true;
      }
    }

    if (!ready_ok) {
      RIVET_LOG_ERROR("RivetController", "WorkerReady handshake failed for '", norm_id, "'");
      shutdown_supervised_stream(st);
      return nullptr;
    }

    st->alive.store(true, std::memory_order_release);
    const uint64_t wtag = make_worker_tag(st->uid);

    {
      std::lock_guard<std::mutex> lk(streams_mutex_);
      active_streams_[norm_id] = st;
      tag_to_stream_[wtag]     = st;
    }
    stream_table_.insert_or_replace(norm_id, st);
    if (st->inproc_worker) {
      registry.insert_or_replace(norm_id, st->inproc_worker);
    }

    (void)poller_.add_or_mod(st->uds_fd, net::kPollRead, wtag);
    poller_.wake();

    RIVET_LOG_INFO("RivetController",
        "Spawned RivetWorker for '",
        norm_id,
        "' (pid=",
        st->child_pid,
        ", shards=",
        egress_shards,
        ", rtp=",
        st->video_rtp_port.load(std::memory_order_relaxed),
        ")");
    return st;
  }

  /// Processes inbound UDS messages from a worker; returns `false` on worker crash / UDS EOF.
  bool drain_worker_uds_messages(SupervisedStream &st) {
    if (st.uds_fd < 0) {
      return false;
    }
    while (true) {
      ipc::IpcMsgHeader    hdr{};
      std::vector<uint8_t> payload;
      int                  passed_fd = -1;
      const auto           rc        = ipc::recv_ipc_message(st.uds_fd, hdr, payload, passed_fd);
      if (passed_fd >= 0) {
        ::close(passed_fd);
      }
      if (rc == ipc::IpcRecvStatus::WouldBlock) {
        return true;
      }
      if (rc == ipc::IpcRecvStatus::EofOrError) {
        return false;
      }

      const auto mtype = static_cast<ipc::IpcMsgType>(hdr.type);
      if (mtype == ipc::IpcMsgType::PublisherStateChanged &&
          payload.size() >= sizeof(ipc::PublisherStatePayload)) {
        ipc::PublisherStatePayload pst{};
        std::memcpy(&pst, payload.data(), sizeof(pst));
        st.has_publisher.store(pst.has_publisher != 0, std::memory_order_release);
        st.is_recording.store(pst.is_recording != 0, std::memory_order_release);
        st.config_generation.store(pst.config_generation, std::memory_order_release);
      } else if (mtype == ipc::IpcMsgType::MetricsReply &&
          payload.size() >= sizeof(MetricsSnapshot)) {
        MetricsSnapshot snap{};
        std::memcpy(&snap, payload.data(), sizeof(snap));
        {
          std::lock_guard<std::mutex> mlk(st.metrics_mu);
          st.latest_metrics = snap;
          ++st.metrics_seq;
        }
        st.metrics_cv.notify_all();
      }
    }
  }

  void handle_worker_disconnect_or_crash(uint64_t wtag) {
    std::shared_ptr<SupervisedStream> dead;
    bool                              should_respawn = false;
    std::string                       respawn_id;
    uint32_t                          respawn_shards = 2;

    {
      std::lock_guard<std::mutex> lk(streams_mutex_);
      auto                        tit = tag_to_stream_.find(wtag);
      if (tit == tag_to_stream_.end()) {
        return;
      }
      dead = tit->second;
      tag_to_stream_.erase(tit);

      if (dead) {
        auto ait = active_streams_.find(dead->stream_id);
        if (ait != active_streams_.end() && ait->second == dead) {
          active_streams_.erase(ait);
          should_respawn = dead->pre_spawned && !dead->quiescing.load(std::memory_order_acquire) &&
              is_running.load(std::memory_order_acquire);
          respawn_id     = dead->stream_id;
          respawn_shards = dead->egress_shards;
        } else {
          draining_streams_.erase(
              std::remove(draining_streams_.begin(), draining_streams_.end(), dead),
              draining_streams_.end());
        }
      }
    }

    if (!dead) {
      return;
    }

    const bool was_quiescing = dead->quiescing.load(std::memory_order_acquire);
    dead->alive.store(false, std::memory_order_release);
    {
      std::lock_guard<std::mutex> mlk(dead->metrics_mu);
      dead->metrics_cv.notify_all();
    }

    if (!was_quiescing) {
      stream_table_.unregister_stream_with(dead->stream_id, [](auto &) {});
      if (dead->inproc_worker) {
        registry.unregister_stream_with(dead->stream_id, [](auto &) {});
      }
    }

    int fd_to_close = -1;
    {
      std::lock_guard<std::mutex> wlk(dead->uds_write_mu);
      if (dead->uds_fd >= 0) {
        poller_.remove(dead->uds_fd);
        fd_to_close  = dead->uds_fd;
        dead->uds_fd = -1;
      }
    }
    if (fd_to_close >= 0) {
      ::close(fd_to_close);
    }

    if (dead->inproc_worker) {
      dead->inproc_worker->stop();
      dead->inproc_worker.reset();
    }
    if (dead->child_pid > 0) {
      try_reap_pid_nonblocking(dead->child_pid);
      dead->child_pid = -1;
    }

    if (!was_quiescing) {
      worker_restarts_.fetch_add(1, std::memory_order_relaxed);
      RIVET_LOG_WARN("RivetController",
          "Tier 3: Worker for '",
          dead->stream_id,
          "' exited unexpectedly; cleaned up");
    }

    if (should_respawn && !respawn_id.empty()) {
      (void)spawn_supervised_stream(respawn_id, respawn_shards, true);
    }
  }

  void close_pending_client(int fd) noexcept {
    auto it = pending_clients_.find(fd);
    if (it == pending_clients_.end()) {
      return;
    }
    poller_.remove(fd);
    ::close(fd);
    pending_clients_.erase(it);
  }

  void queue_pending_client_reply(PendingClient &c, std::string_view reply) {
    if (c.outbox_sent == c.outbox.size()) {
      c.outbox.clear();
      c.outbox_sent = 0;
    }
    c.outbox.append(reply);
  }

  /// Flushes `PendingClient::outbox`. Returns `false` if connection suffered a fatal send error.
  bool flush_pending_client(PendingClient &c) noexcept {
    while (c.outbox_sent < c.outbox.size()) {
      const ssize_t n = ::send(c.fd,
          c.outbox.data() + c.outbox_sent,
          c.outbox.size() - c.outbox_sent,
          net::nonblocking_send_flags());
      if (n > 0) {
        c.outbox_sent += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        (void)poller_.add_or_mod(c.fd, net::kPollRead | net::kPollWrite, make_client_tag(c.fd));
        return true;
      }
      return false;
    }
    c.outbox.clear();
    c.outbox_sent = 0;
    (void)poller_.add_or_mod(c.fd, net::kPollRead, make_client_tag(c.fd));
    return true;
  }

  void accept_new_tcp_clients(uint64_t now_ns) {
    if (tcp_server_socket < 0) {
      return;
    }
    while (true) {
      sockaddr_in caddr{};
      socklen_t   clen = sizeof(caddr);
      const int   client_fd =
          ::accept(tcp_server_socket, reinterpret_cast<sockaddr *>(&caddr), &clen);
      if (client_fd < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }

      /// Non-blocking + `FD_CLOEXEC` + `TCP_NODELAY` + `SO_NOSIGPIPE` (NO DNS!)
      (void)net::set_nonblocking_cloexec(client_fd);
      net::disable_sigpipe_on_socket(client_fd);
      int nodelay = 1;
      (void)::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

      char ip_buf[INET_ADDRSTRLEN] = "127.0.0.1";
      (void)::inet_ntop(AF_INET, &caddr.sin_addr, ip_buf, sizeof(ip_buf));

      PendingClient ctx;
      ctx.fd               = client_fd;
      ctx.ip               = ip_buf;
      ctx.connected_at_ns  = now_ns;
      ctx.last_activity_ns = now_ns;
      ctx.auth_nonce       = proto::rtsp::generate_random_hex_id();

      pending_clients_.emplace(client_fd, std::move(ctx));
      (void)poller_.add_or_mod(client_fd, net::kPollRead, make_client_tag(client_fd));
    }
  }

  bool handoff_client_to_worker(PendingClient &c,
      SupervisedStream                        &st,
      ipc::IpcMsgType                          handoff_type,
      std::string_view                         initial_bytes,
      std::string_view                         auth_nonce = {}) {
    /// Ensure any queued pre-handoff reply (e.g. pipelined `OPTIONS 200 OK`) hits the socket first
    if (!flush_pending_client(c)) {
      return false;
    }

    const auto payload = ipc::encode_handoff_payload(c.ip, initial_bytes, auth_nonce);
    bool       ok      = false;
    {
      std::lock_guard<std::mutex> wlk(st.uds_write_mu);
      if (st.uds_fd >= 0 && st.alive.load(std::memory_order_acquire)) {
        ok = ipc::send_ipc_message(
            st.uds_fd, handoff_type, payload.data(), static_cast<uint32_t>(payload.size()), c.fd);
      }
    }
    return ok;
  }

  void handle_pending_client_readable(int fd, uint64_t now_ns) {
    auto it = pending_clients_.find(fd);
    if (it == pending_clients_.end()) {
      return;
    }
    PendingClient &c = it->second;

    while (true) {
      auto span = c.read_buf.writable_span(8192);
      if (span.empty()) {
        close_pending_client(fd);
        return;
      }
      const ssize_t n = ::recv(fd, span.data(), span.size(), MSG_DONTWAIT);
      if (n > 0) {
        c.last_activity_ns = now_ns;
        c.read_buf.commit_write(static_cast<size_t>(n));
        continue;
      }
      if (n == 0) {
        close_pending_client(fd);
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      close_pending_client(fd);
      return;
    }

    while (!c.read_buf.empty()) {
      const auto view = c.read_buf.next_view();
      if (view.type == proto::rtsp::FramedMessageType::None) {
        if (c.read_buf.readable_bytes() >= c.read_buf.capacity()) {
          close_pending_client(fd);
          return;
        }
        break;
      }
      if (view.type == proto::rtsp::FramedMessageType::Error) {
        /// Malformed framing / negative or oversized Content-Length -> drop!
        close_pending_client(fd);
        return;
      }
      if (view.type == proto::rtsp::FramedMessageType::Interleaved) {
        /// Binary `$` frame before RTSP handshake -> drop unauthorized packet!
        unauthorized_drops_.fetch_add(1, std::memory_order_relaxed);
        c.read_buf.consume(view.wire_bytes);
        continue;
      }

      /// Text RTSP Request
      const auto req = proto::rtsp::parse_rtsp_request_text(view.as_text());
      if (!req.valid) {
        c.read_buf.consume(view.wire_bytes);
        queue_pending_client_reply(c, format_rtsp_error(400, "Bad Request", req.cseq));
        c.close_after_flush = true;
        break;
      }

      if (req.method == "OPTIONS") {
        c.read_buf.consume(view.wire_bytes);
        std::string resp = "RTSP/1.0 200 OK\r\nCSeq: " + std::string(req.cseq) +
            "\r\nPublic: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, ANNOUNCE, RECORD, "
            "GET_PARAMETER\r\n\r\n";
        queue_pending_client_reply(c, resp);
        continue;
      }

      if (req.method == "GET_PARAMETER") {
        c.read_buf.consume(view.wire_bytes);
        if (!req.session.empty()) {
          queue_pending_client_reply(c, format_rtsp_error(454, "Session Not Found", req.cseq));
        } else {
          queue_pending_client_reply(
              c, "RTSP/1.0 200 OK\r\nCSeq: " + std::string(req.cseq) + "\r\n\r\n");
        }
        continue;
      }

      if (req.method == "ANNOUNCE") {
        /// 1. Opt-in RFC 2617 Digest Authentication BEFORE spawning any worker
        if (!config_.publish_user.empty()) {
          if (!proto::rtsp::verify_rtsp_digest_auth(req.authorization,
                  "ANNOUNCE",
                  "RIVET",
                  c.auth_nonce,
                  config_.publish_user,
                  config_.publish_password)) {
            c.read_buf.consume(view.wire_bytes);
            const std::string www =
                "WWW-Authenticate: Digest realm=\"RIVET\", nonce=\"" + c.auth_nonce + "\"\r\n";
            queue_pending_client_reply(c, format_rtsp_error(401, "Unauthorized", req.cseq, www));
            continue; //< Keep connection alive for CSeq+1 authenticated retry!
          }
        }

        /// 2. Validate SDP size, syntax, CRLF safety & codec/tx-mode BEFORE spawning worker
        if (req.body.size() > media::StreamDescriptor::MAX_SDP_BYTES) {
          c.read_buf.consume(view.wire_bytes);
          queue_pending_client_reply(
              c, format_rtsp_error(413, "Request Entity Too Large", req.cseq));
          continue;
        }
        const auto sdp_res = media::parse_announce_sdp(req.body);
        if (sdp_res.error == media::SdpError::TooLarge) {
          c.read_buf.consume(view.wire_bytes);
          queue_pending_client_reply(
              c, format_rtsp_error(413, "Request Entity Too Large", req.cseq));
          continue;
        }
        if (sdp_res.error == media::SdpError::UnsupportedTxMode ||
            sdp_res.error == media::SdpError::TooManyTracks) {
          c.read_buf.consume(view.wire_bytes);
          queue_pending_client_reply(c, format_rtsp_error(415, "Unsupported Media Type", req.cseq));
          continue;
        }
        if (sdp_res.error != media::SdpError::None || !sdp_res.descriptor.has_value()) {
          c.read_buf.consume(view.wire_bytes);
          queue_pending_client_reply(c, format_rtsp_error(400, "Bad Request", req.cseq));
          continue;
        }

        const std::string stream_path = StreamRegistry::extract_stream_path(req.uri);
        if (stream_path.empty()) {
          c.read_buf.consume(view.wire_bytes);
          queue_pending_client_reply(c, format_rtsp_error(400, "Bad Request", req.cseq));
          continue;
        }

        /// 3. Route to existing worker or spawn isolated worker process dynamically
        auto st = stream_table_.route(stream_path);
        if (!st || !st->alive.load(std::memory_order_acquire)) {
          st = spawn_supervised_stream(stream_path, config_.egress_shards, false);
        }
        if (!st || !st->alive.load(std::memory_order_acquire)) {
          c.read_buf.consume(view.wire_bytes);
          queue_pending_client_reply(c, format_rtsp_error(503, "Service Unavailable", req.cseq));
          c.close_after_flush = true;
          break;
        }

        /// Pass `client_fd` + unconsumed `ANNOUNCE` (+ any pipelined requests) via `SCM_RIGHTS`
        const auto       rem_span = c.read_buf.readable_span();
        std::string_view rem_view(reinterpret_cast<const char *>(rem_span.data()), rem_span.size());
        (void)handoff_client_to_worker(
            c, *st, ipc::IpcMsgType::HandoffPublisher, rem_view, c.auth_nonce);

        /// Controller immediately relinquishes `fd` ownership
        close_pending_client(fd);
        return;
      }

      if (req.method == "DESCRIBE" || req.method == "SETUP") {
        if (req.method == "SETUP" && transport_requests_record(req.transport)) {
          /// `SETUP mode=record` without prior `ANNOUNCE` -> 455
          c.read_buf.consume(view.wire_bytes);
          queue_pending_client_reply(
              c, format_rtsp_error(455, "Method Not Valid in This State", req.cseq));
          continue;
        }

        /// Exact stream lookup (never falls back to `routes.begin()`)
        const std::string stream_path = StreamRegistry::extract_stream_path(req.uri);
        auto              st          = stream_table_.route(stream_path);
        if (!st || !st->alive.load(std::memory_order_acquire)) {
          c.read_buf.consume(view.wire_bytes);
          queue_pending_client_reply(c, format_rtsp_error(404, "Not Found", req.cseq));
          continue;
        }

        const auto       rem_span = c.read_buf.readable_span();
        std::string_view rem_view(reinterpret_cast<const char *>(rem_span.data()), rem_span.size());
        (void)handoff_client_to_worker(c, *st, ipc::IpcMsgType::HandoffViewer, rem_view);
        close_pending_client(fd);
        return;
      }

      if (req.method == "PLAY" || req.method == "RECORD" || req.method == "TEARDOWN") {
        c.read_buf.consume(view.wire_bytes);
        if (!req.session.empty()) {
          queue_pending_client_reply(c, format_rtsp_error(454, "Session Not Found", req.cseq));
        } else {
          queue_pending_client_reply(
              c, format_rtsp_error(455, "Method Not Valid in This State", req.cseq));
        }
        continue;
      }

      /// Unknown / unimplemented RTSP method (including `PAUSE`) -> `501 Not Implemented`
      c.read_buf.consume(view.wire_bytes);
      queue_pending_client_reply(c, format_rtsp_error(501, "Not Implemented", req.cseq));
    }

    auto still_it = pending_clients_.find(fd);
    if (still_it != pending_clients_.end()) {
      if (!flush_pending_client(still_it->second)) {
        close_pending_client(fd);
        return;
      }
      if (still_it->second.close_after_flush && !still_it->second.has_pending_outbox()) {
        close_pending_client(fd);
      }
    }
  }

  void sweep_idle_pending_clients(uint64_t now_ns) {
    const uint32_t cap_ms     = (config_.client_timeout_ms > 0)
        ? std::min<uint32_t>(5000U, config_.client_timeout_ms)
        : 5000U;
    const uint64_t timeout_ns = static_cast<uint64_t>(cap_ms) * 1'000'000ULL;

    std::vector<int> expired;
    for (const auto &kv : pending_clients_) {
      if (now_ns > kv.second.last_activity_ns &&
          (now_ns - kv.second.last_activity_ns) > timeout_ns) {
        expired.push_back(kv.first);
      }
    }
    for (int fd : expired) {
      close_pending_client(fd);
    }
  }

  void run_supervisor_loop() {
    std::array<net::PollEvent, 128> events{};

    while (is_running.load(std::memory_order_acquire)) {
      const size_t   n      = poller_.wait(events, 10);
      const uint64_t now_ns = poller_.now_ns();

      /// Pass 1: Drain worker UDS notifications & detect worker crashes first
      for (size_t i = 0; i < n; ++i) {
        const uint64_t tag = events[i].tag;
        if (tag < kTagWorkerBase) {
          continue;
        }
        std::shared_ptr<SupervisedStream> st;
        {
          std::lock_guard<std::mutex> lk(streams_mutex_);
          auto                        it = tag_to_stream_.find(tag);
          if (it != tag_to_stream_.end()) {
            st = it->second;
          }
        }
        if (!st) {
          continue;
        }
        bool ok = true;
        if ((events[i].flags & net::kPollRead) != 0) {
          ok = drain_worker_uds_messages(*st);
        }
        if (!ok || (events[i].flags & (net::kPollHup | net::kPollError)) != 0) {
          if (!drain_worker_uds_messages(*st)) {
            handle_worker_disconnect_or_crash(tag);
          }
        }
      }

      /// Pass 2: Accept new TCP connections & process pre-handoff RTSP requests
      for (size_t i = 0; i < n; ++i) {
        const uint64_t tag = events[i].tag;
        const uint32_t fl  = events[i].flags;

        if (tag == kTagListen) {
          accept_new_tcp_clients(now_ns);
        } else if (tag >= kTagClientBase && tag < kTagWorkerBase) {
          const int cfd = client_fd_from_tag(tag);
          if ((fl & net::kPollWrite) != 0) {
            auto it = pending_clients_.find(cfd);
            if (it != pending_clients_.end()) {
              if (!flush_pending_client(it->second)) {
                close_pending_client(cfd);
                continue;
              }
              if (it->second.close_after_flush && !it->second.has_pending_outbox()) {
                close_pending_client(cfd);
                continue;
              }
            }
          }
          if ((fl & (net::kPollRead | net::kPollHup | net::kPollError)) != 0) {
            handle_pending_client_readable(cfd, now_ns);
          }
        }
      }

      sweep_idle_pending_clients(now_ns);
      sweep_pending_reap_pids();
    }

    /// Clean up any unhanded pre-handoff TCP connections on supervisor loop exit
    for (auto &kv : pending_clients_) {
      poller_.remove(kv.first);
      ::close(kv.first);
    }
    pending_clients_.clear();
  }

public:
  explicit RivetController(uint16_t port       = 8554,
      uint32_t                      timeout_ms = 5000,
      std::string                   policy     = "FreshnessFirst",
      uint16_t                      rtp_port   = 20000,
      uint16_t                      rtcp_port  = 20001,
      size_t                        arena_cap  = 4096,
      size_t                        arena_slab = 2048)
      : arena(arena_cap, arena_slab) {
    config_.rtsp_port           = port;
    config_.watchdog_timeout_ms = timeout_ms;
    config_.startup_policy      = std::move(policy);
    config_.base_rtp_port       = rtp_port;
    config_.base_rtcp_port      = rtcp_port;
    config_.numa_capacity       = arena_cap;
    config_.numa_slab_size      = arena_slab;
  }

  explicit RivetController(const config::RivetConfig &cfg)
      : arena(cfg.numa_capacity > 0 ? cfg.numa_capacity : 64,
            cfg.numa_slab_size > 0 ? cfg.numa_slab_size : 2048)
      , config_(cfg) {}

  ~RivetController() { stop(); }

  RivetController(const RivetController &)            = delete;
  RivetController &operator=(const RivetController &) = delete;
  RivetController(RivetController &&)                 = delete;
  RivetController &operator=(RivetController &&)      = delete;

  [[nodiscard]] uint16_t get_port() const noexcept { return config_.rtsp_port; }
  [[nodiscard]] uint16_t get_bound_rtsp_port() const noexcept {
    return bound_rtsp_port.load(std::memory_order_acquire);
  }
  [[nodiscard]] uint16_t bound_port() const noexcept { return get_bound_rtsp_port(); }
  [[nodiscard]] uint16_t get_bound_video_port() const noexcept {
    return bound_video_port.load(std::memory_order_acquire);
  }
  [[nodiscard]] uint16_t get_bound_audio_port() const noexcept {
    return bound_audio_port.load(std::memory_order_acquire);
  }
  [[nodiscard]] uint32_t get_watchdog_timeout_ms() const noexcept {
    return config_.watchdog_timeout_ms;
  }
  void set_watchdog_timeout_ms(uint32_t ms) noexcept { config_.watchdog_timeout_ms = ms; }
  [[nodiscard]] const std::string &get_startup_policy() const noexcept {
    return config_.startup_policy;
  }
  void set_startup_policy(std::string policy) noexcept {
    config_.startup_policy = std::move(policy);
  }
  [[nodiscard]] const config::RivetConfig &get_config() const noexcept { return config_; }
  [[nodiscard]] StreamRegistry            &get_registry() noexcept { return registry; }
  [[nodiscard]] const StreamRegistry      &get_registry() const noexcept { return registry; }
  [[nodiscard]] memory::NumaArena         &get_arena() noexcept { return arena; }
  [[nodiscard]] const memory::NumaArena   &get_arena() const noexcept { return arena; }
  [[nodiscard]] uint64_t                   total_restarts() const noexcept {
    return worker_restarts_.load(std::memory_order_acquire);
  }
  [[nodiscard]] uint64_t unauthorized_drops() const noexcept {
    return unauthorized_drops_.load(std::memory_order_acquire);
  }

  /// Pre-spawns and registers a `RivetWorker` for `stream_id`.
  /// In `ExecutionMode::InProcess`, returns the live `std::shared_ptr<RivetWorker>`.
  /// In `ExecutionMode::MultiProcess`, spawns the isolated `--worker` child OS process.
  std::shared_ptr<RivetWorker> spawn_worker(const std::string &stream_id,
      uint32_t                                                 egress_shards = 2) {
    auto st = spawn_supervised_stream(stream_id, egress_shards, true);
    return st ? st->inproc_worker : nullptr;
  }

  /// Tier 2: Quiesces and drains an existing worker while spawning its live replacement (`§7.2`).
  std::shared_ptr<RivetWorker> quiesce_and_drain(const std::string &stream_id) {
    const std::string                 norm_id = StreamRegistry::extract_stream_path(stream_id);
    std::shared_ptr<SupervisedStream> old_st;
    uint32_t                          shards = config_.egress_shards;
    {
      std::lock_guard<std::mutex> lk(streams_mutex_);
      auto                        it = active_streams_.find(norm_id);
      if (it != active_streams_.end()) {
        old_st = it->second;
        shards = old_st->egress_shards;
        active_streams_.erase(it);
        if (old_st) {
          old_st->quiescing.store(true, std::memory_order_release);
          draining_streams_.push_back(old_st);
        }
      }
    }

    if (old_st) {
      stream_table_.unregister_stream_with(norm_id, [](auto &) {});
      if (old_st->inproc_worker) {
        registry.unregister_stream_with(norm_id, [](auto &) {});
      }
      std::lock_guard<std::mutex> wlk(old_st->uds_write_mu);
      if (old_st->uds_fd >= 0) {
        (void)ipc::send_ipc_message(old_st->uds_fd, ipc::IpcMsgType::QuiesceAndDrain);
      }
    }

    auto replacement = spawn_supervised_stream(norm_id, shards, true);
    return replacement ? replacement->inproc_worker : nullptr;
  }

  /// Tier 3: Hard crash / forced restart of a stream worker (`§7.3`).
  std::shared_ptr<RivetWorker> restart_on_crash(const std::string &stream_id) {
    const std::string                 norm_id = StreamRegistry::extract_stream_path(stream_id);
    std::shared_ptr<SupervisedStream> old_st;
    uint32_t                          shards = config_.egress_shards;
    {
      std::lock_guard<std::mutex> lk(streams_mutex_);
      auto                        it = active_streams_.find(norm_id);
      if (it != active_streams_.end()) {
        old_st = it->second;
        shards = old_st->egress_shards;
        active_streams_.erase(it);
        if (old_st) {
          tag_to_stream_.erase(make_worker_tag(old_st->uid));
        }
      }
    }
    stream_table_.unregister_stream_with(norm_id, [](auto &) {});
    registry.unregister_stream_with(norm_id, [](auto &) {});

    if (old_st) {
      shutdown_supervised_stream(old_st);
    }
    worker_restarts_.fetch_add(1, std::memory_order_relaxed);
    auto replacement = spawn_supervised_stream(norm_id, shards, true);
    return replacement ? replacement->inproc_worker : nullptr;
  }

  /// Watchdog / maintenance hook (supervision runs continuously inside `supervisor_thread_`).
  void supervise() { sweep_pending_reap_pids(); }

  /// Starts the master supervisor event loop (returns `false` on bind/listen failure).
  bool start() {
    if (is_running.exchange(true, std::memory_order_acq_rel)) {
      return true;
    }

    tcp_server_socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_server_socket < 0) {
      RIVET_LOG_ERROR("RivetController", "socket(AF_INET) failed: ", std::strerror(errno));
      is_running.store(false, std::memory_order_release);
      return false;
    }

    (void)net::set_nonblocking_cloexec(tcp_server_socket);
    int opt = 1;
    (void)::setsockopt(tcp_server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(config_.rtsp_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(tcp_server_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
      RIVET_LOG_ERROR("RivetController",
          "Failed to bind TCP port ",
          config_.rtsp_port,
          ": ",
          std::strerror(errno));
      ::close(tcp_server_socket);
      tcp_server_socket = -1;
      is_running.store(false, std::memory_order_release);
      return false;
    }

    if (::listen(tcp_server_socket, 256) < 0) {
      RIVET_LOG_ERROR("RivetController", "Failed to listen on TCP port: ", std::strerror(errno));
      ::close(tcp_server_socket);
      tcp_server_socket = -1;
      is_running.store(false, std::memory_order_release);
      return false;
    }

    sockaddr_in baddr{};
    socklen_t   blen = sizeof(baddr);
    if (::getsockname(tcp_server_socket, reinterpret_cast<sockaddr *>(&baddr), &blen) == 0) {
      bound_rtsp_port.store(ntohs(baddr.sin_port), std::memory_order_release);
    } else {
      bound_rtsp_port.store(config_.rtsp_port, std::memory_order_release);
    }

    (void)poller_.add_or_mod(tcp_server_socket, net::kPollRead, kTagListen);
    supervisor_thread_ = std::thread(&RivetController::run_supervisor_loop, this);

    RIVET_LOG_INFO("RivetController",
        "RTSP supervisor listening on TCP port ",
        get_bound_rtsp_port(),
        " (",
        (config_.execution_mode == config::ExecutionMode::MultiProcess) ? "MultiProcess"
                                                                        : "InProcess",
        " mode)");
    return true;
  }

  /// Wake-then-join-then-reap-then-close shutdown.
  void stop() noexcept {
    const bool was_running = is_running.exchange(false, std::memory_order_acq_rel);
    if (was_running) {
      poller_.wake();
      if (supervisor_thread_.joinable()) {
        supervisor_thread_.join();
      }
    }

    if (tcp_server_socket >= 0) {
      poller_.remove(tcp_server_socket);
      ::close(tcp_server_socket);
      tcp_server_socket = -1;
    }

    std::vector<std::shared_ptr<SupervisedStream>> to_stop;
    std::vector<pid_t>                             extra_pids;
    {
      std::lock_guard<std::mutex> lk(streams_mutex_);
      to_stop.reserve(active_streams_.size() + draining_streams_.size());
      for (auto &kv : active_streams_) {
        to_stop.push_back(kv.second);
      }
      for (auto &d : draining_streams_) {
        to_stop.push_back(d);
      }
      active_streams_.clear();
      draining_streams_.clear();
      tag_to_stream_.clear();
      extra_pids.swap(pending_reap_pids_);
    }

    stream_table_.clear_with([](auto &) {});
    registry.clear_with([](auto &) {});

    for (auto &st : to_stop) {
      shutdown_supervised_stream(st);
    }
    for (pid_t pid : extra_pids) {
      wait_or_kill_child(pid, 100);
    }
  }

  [[nodiscard]] bool active() const noexcept { return is_running.load(std::memory_order_acquire); }

  [[nodiscard]] size_t worker_count() const noexcept {
    std::lock_guard<std::mutex> lk(streams_mutex_);
    return active_streams_.size();
  }

  [[nodiscard]] bool has_stream(std::string_view stream_id) const {
    auto st = stream_table_.route(stream_id);
    return st != nullptr && st->alive.load(std::memory_order_acquire);
  }

  /// Returns child OS PID (`> 0`) in `MultiProcess` mode, `getpid()` in `InProcess` mode, or `-1`.
  [[nodiscard]] pid_t worker_pid(std::string_view stream_id) const {
    auto st = stream_table_.route(stream_id);
    if (!st || !st->alive.load(std::memory_order_acquire)) {
      return -1;
    }
    if (st->child_pid > 0) {
      return st->child_pid;
    }
    return st->inproc_worker ? ::getpid() : -1;
  }

  /// Queries a live `MetricsSnapshot` from the worker supervising `stream_id`.
  [[nodiscard]] std::optional<MetricsSnapshot> query_stream_metrics(std::string_view stream_id,
      uint32_t timeout_ms = 1000) {
    auto st = stream_table_.route(stream_id);
    if (!st || !st->alive.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    if (st->inproc_worker) {
      return st->inproc_worker->snapshot_metrics();
    }

    if (!is_running.load(std::memory_order_acquire)) {
      /// Fallback synchronous read when supervisor thread is not active
      std::lock_guard<std::mutex> wlk(st->uds_write_mu);
      if (st->uds_fd < 0 || !ipc::send_ipc_message(st->uds_fd, ipc::IpcMsgType::RequestMetrics)) {
        return std::nullopt;
      }
      if (!ipc::detail::wait_fd_ready(st->uds_fd, POLLIN, static_cast<int>(timeout_ms))) {
        return std::nullopt;
      }
      (void)drain_worker_uds_messages(*st);
      std::lock_guard<std::mutex> mlk(st->metrics_mu);
      return st->latest_metrics;
    }

    uint64_t prev_seq = 0;
    {
      std::lock_guard<std::mutex> mlk(st->metrics_mu);
      prev_seq = st->metrics_seq;
    }
    {
      std::lock_guard<std::mutex> wlk(st->uds_write_mu);
      if (st->uds_fd < 0 || !ipc::send_ipc_message(st->uds_fd, ipc::IpcMsgType::RequestMetrics)) {
        return std::nullopt;
      }
    }

    std::unique_lock<std::mutex> mlk(st->metrics_mu);
    const bool got = st->metrics_cv.wait_for(mlk, std::chrono::milliseconds(timeout_ms), [&] {
      return st->metrics_seq > prev_seq || !st->alive.load(std::memory_order_acquire);
    });
    if (!got || st->metrics_seq <= prev_seq) {
      return std::nullopt;
    }
    return st->latest_metrics;
  }
};

} //< namespace rivet::core
