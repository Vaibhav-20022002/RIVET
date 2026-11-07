#pragma once

#include "rivet/config/RivetConfig.hpp"
#include "rivet/core/Log.hpp"
#include "rivet/core/Metrics.hpp"
#include "rivet/core/PacketRouter.hpp"
#include "rivet/core/SubscriberSession.hpp"
#include "rivet/core/SubscriberTable.hpp"
#include "rivet/core/Zones.hpp"
#include "rivet/media/ConfigGeneration.hpp"
#include "rivet/media/RetainedPacketLog.hpp"
#include "rivet/media/StreamDescriptor.hpp"
#include "rivet/memory/PacketRef.hpp"
#include "rivet/net/EventPoller.hpp"
#include "rivet/net/SocketUtils.hpp"
#include "rivet/proto/rtp/RtcpReportGenerator.hpp"
#include "rivet/proto/rtsp/RtspControlSession.hpp"
#include "rivet/proto/rtsp/RtspDigestAuth.hpp"
#include "rivet/proto/rtsp/RtspStreamFramer.hpp"
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rivet::core {

/// EgressReactor manages a disjoint partition of viewer TCP connections and UDP subscribers on a
/// dedicated CPU thread, enforcing single-socket ownership, non-blocking atomic `iovec` `$`
/// framing, `PLAY 200 OK` serialization before media, SoA/SIMD per-track dispatch, paced RAP
/// cursor replay, batched UDP `sendmmsg`/GSO, RTCP SR/RR/PLI/BYE, and fair quota eviction.
class EgressReactor {
public:
  static constexpr size_t   MAX_REACTOR_EGRESS_BYTES = 16 * 1024 * 1024;
  static constexpr uint16_t BASE_RTP_PORT            = 20000;
  static constexpr uint16_t BASE_RTCP_PORT           = 20001;

private:
  static constexpr uint64_t kTagUdpRtcp    = 0x7FFFFFFFULL;
  static constexpr size_t   kUdpBatchLimit = 64;

  struct ViewerHandoffCmd {
    int         fd{-1};
    std::string client_ip;
    std::string initial_bytes;
  };

  struct ViewerConnection {
    int                             fd{-1};
    std::string                     client_ip;
    proto::rtsp::RtspReadBuffer     read_buf{proto::rtsp::RtspReadBuffer::DEFAULT_CAPACITY};
    proto::rtsp::RtspControlSession rtsp_session;
    std::string                     control_outbox;
    size_t                          control_outbox_sent{0};
    std::vector<uint64_t>           session_ids;
    uint64_t                        last_activity_ns{0};
    bool                            close_after_control_flush{false};

    [[nodiscard]] bool has_pending_control() const noexcept {
      return control_outbox_sent < control_outbox.size();
    }

    void append_control(std::string_view bytes) {
      if (control_outbox_sent == control_outbox.size()) {
        control_outbox.clear();
        control_outbox_sent = 0;
      } else if (control_outbox_sent > 4096) {
        control_outbox.erase(0, control_outbox_sent);
        control_outbox_sent = 0;
      }
      control_outbox.append(bytes);
    }
  };

  struct UdpPendingDatagram {
    sockaddr_in             dest{};
    std::array<uint8_t, 32> rewritten_hdr{};
    uint32_t                rewritten_hdr_len{0};
    memory::PacketRef       pkt{};
    int                     out_sock{-1};
  };

  /// Context callbacks supplied by owning `RivetWorker` for viewer `DESCRIBE` / `SETUP` / `PLAY`
  struct StreamContextCallbacks {
    std::function<std::optional<media::StreamDescriptor>()>         get_descriptor;
    std::function<std::shared_ptr<const media::ConfigGeneration>()> get_config_snapshot;
    std::function<const media::RetainedPacketLog *()>               get_packet_log;
    std::function<void()>                                           request_upstream_pli;
  };

  uint32_t                  reactor_id{0};
  uint16_t                  rtp_port{0};
  uint16_t                  rtcp_port{0};
  size_t                    max_reactor_bytes_{MAX_REACTOR_EGRESS_BYTES};
  size_t                    burst_budget_bytes_{2 * 1024 * 1024};
  size_t                    max_sub_queue_bytes_{2 * 1024 * 1024};
  uint64_t                  client_timeout_ns_{60'000'000'000ULL};
  bool                      join_on_open_irap_{true};
  bool                      enable_udp_gso_{false};
  config::StartupPolicyKind startup_kind_{config::StartupPolicyKind::FastPictureFirst};
  std::string               stream_path_;

  PacketRouter::PacketQueue *spsc_channel{nullptr};
  SubscriberTable            subscribers_;

  /// Viewer TCP connections owned strictly by this shard
  std::unordered_map<int, std::unique_ptr<ViewerConnection>> viewer_conns_;

  /// Thread-safe handoff inbox from Worker Thread 0 -> Shard Thread
  std::mutex                    handoff_mutex_;
  std::vector<ViewerHandoffCmd> handoff_inbox_;
  std::vector<std::pair<std::unique_ptr<SubscriberSession>, const media::RetainedPacketLog *>>
                        external_sub_inbox_;
  std::vector<uint64_t> external_remove_inbox_;

  StreamContextCallbacks callbacks_{};

  net::EventPoller  poller_;
  std::thread       worker_thread_;
  std::atomic<bool> is_running{false};
  std::atomic<bool> thread_active_{false};
  std::atomic<bool> quiesced_{false};

  std::atomic<size_t> active_sub_count_atomic_{0};
  AtomicMetrics      *shared_metrics_{nullptr};

  size_t   aggregate_queued_bytes{0};
  uint64_t total_packets_dispatched{0};
  uint64_t total_packets_dropped{0};
  uint64_t last_housekeeping_ns_{0};
  uint64_t last_sr_ns_{0};

  int  udp_rtp_socket_{-1};
  int  udp_rtcp_socket_{-1};
  bool owns_udp_sockets_{false};
  int  udp_video_socket{-1};
  int  udp_audio_socket{-1};

  std::atomic<uint64_t>           next_local_session_id_{1};
  std::vector<uint32_t>           eligible_scratch_;
  std::vector<UdpPendingDatagram> udp_batch_;

  [[nodiscard]] static uint32_t random_u32() noexcept {
    const std::string hex = proto::rtsp::generate_random_hex_id();
    uint32_t          v   = 0x52495654U;
    for (size_t i = 0; i < 8 && i < hex.size(); ++i) {
      const char c   = hex[i];
      const auto nib = (c >= '0' && c <= '9') ? static_cast<uint32_t>(c - '0')
          : (c >= 'a' && c <= 'f')            ? static_cast<uint32_t>(10 + (c - 'a'))
          : (c >= 'A' && c <= 'F')            ? static_cast<uint32_t>(10 + (c - 'A'))
                                              : 0U;
      v              = (v << 4U) | nib;
    }
    return (v != 0U) ? v : 0x52495654U;
  }

  void sync_sub_count() noexcept {
    active_sub_count_atomic_.store(subscribers_.size(), std::memory_order_relaxed);
  }

  void reclaim_session_bytes(SubscriberSession &s) noexcept {
    const size_t unreleased = s.get_queued_bytes();
    aggregate_queued_bytes =
        (aggregate_queued_bytes >= unreleased) ? (aggregate_queued_bytes - unreleased) : 0;
  }

  void update_viewer_poll_interest(ViewerConnection &conn) noexcept {
    if (conn.fd < 0) {
      return;
    }
    bool need_write = conn.has_pending_control();
    if (!need_write) {
      for (uint64_t sid : conn.session_ids) {
        if (const auto *sub = subscribers_.find(sid)) {
          if (sub->has_pending_work()) {
            need_write = true;
            break;
          }
        }
      }
    }
    uint32_t ev = net::kPollRead | (need_write ? net::kPollWrite : 0U);
    (void)poller_.add_or_mod(conn.fd, ev, static_cast<uint64_t>(conn.fd));
  }

  void close_viewer_connection(int fd, bool count_slow_disconnect = false) noexcept {
    auto it = viewer_conns_.find(fd);
    if (it == viewer_conns_.end()) {
      return;
    }
    auto conn = std::move(it->second);
    viewer_conns_.erase(it);

    for (uint64_t sid : conn->session_ids) {
      remove_subscriber_internal(sid);
    }
    if (conn->fd >= 0) {
      poller_.remove(conn->fd);
      ::close(conn->fd);
      conn->fd = -1;
    }
    if (count_slow_disconnect && shared_metrics_) {
      shared_metrics_->slow_subscriber_disconnects.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void remove_subscriber_internal(uint64_t session_id) noexcept {
    auto removed = subscribers_.erase(session_id);
    if (removed) {
      reclaim_session_bytes(*removed);
      removed->mark_closed();
      sync_sub_count();
    }
  }

  void evict_slow_subscriber(uint64_t sid) noexcept {
    int owning_fd = -1;
    if (const auto *s = subscribers_.find(sid)) {
      if (s->get_transport() == SubscriberTransport::TcpInterleaved && s->get_socket_fd() >= 0) {
        owning_fd = s->get_socket_fd();
      }
    }
    if (owning_fd >= 0 && viewer_conns_.find(owning_fd) != viewer_conns_.end()) {
      close_viewer_connection(owning_fd, true);
    } else if (subscribers_.find(sid) != nullptr) {
      remove_subscriber_internal(sid);
      if (shared_metrics_) {
        shared_metrics_->slow_subscriber_disconnects.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  void enforce_reactor_quota_fairly(size_t incoming_bytes) noexcept {
    if (!subscribers_.empty() && aggregate_queued_bytes > 0 &&
        (aggregate_queued_bytes + incoming_bytes) > max_reactor_bytes_) {
      /// Flush non-blocked subscribers FIRST so healthy viewers drain to 0 queued bytes
      /// and are never misclassified as laggy during bursty SPSC batches!
      (void)flush_subscriber_sends();
    }
    while (!subscribers_.empty() && aggregate_queued_bytes > 0 &&
        (aggregate_queued_bytes + incoming_bytes) > max_reactor_bytes_) {
      const uint64_t laggiest_id = subscribers_.find_laggiest_session_id();
      if (laggiest_id == 0) {
        break;
      }
      evict_slow_subscriber(laggiest_id);
    }
  }

public:
  explicit EgressReactor(uint32_t id,
      PacketRouter::PacketQueue  *channel   = nullptr,
      uint16_t                    base_rtp  = BASE_RTP_PORT,
      uint16_t                    base_rtcp = BASE_RTCP_PORT)
      : reactor_id(id)
      , rtp_port(base_rtp == 0 ? 0 : static_cast<uint16_t>(base_rtp + (2 * id)))
      , rtcp_port(base_rtcp == 0 ? 0 : static_cast<uint16_t>(base_rtcp + (2 * id)))
      , spsc_channel(channel) {
    eligible_scratch_.reserve(256);
    udp_batch_.reserve(kUdpBatchLimit);
  }

  ~EgressReactor() { stop(); }

  EgressReactor(const EgressReactor &)            = delete;
  EgressReactor &operator=(const EgressReactor &) = delete;
  EgressReactor(EgressReactor &&)                 = delete;
  EgressReactor &operator=(EgressReactor &&)      = delete;

  void configure(const config::RivetConfig &cfg,
      std::string_view                      stream_path,
      AtomicMetrics                        *metrics,
      StreamContextCallbacks                cbs) {
    max_reactor_bytes_ =
        cfg.max_reactor_egress_bytes > 0 ? cfg.max_reactor_egress_bytes : MAX_REACTOR_EGRESS_BYTES;
    burst_budget_bytes_  = cfg.burst_budget_bytes;
    max_sub_queue_bytes_ = cfg.max_subscriber_queue_bytes;
    client_timeout_ns_ =
        static_cast<uint64_t>(cfg.client_timeout_ms > 0 ? cfg.client_timeout_ms : 60000) *
        1'000'000ULL;
    join_on_open_irap_ = cfg.join_on_open_irap;
    enable_udp_gso_    = cfg.enable_udp_gso;
    startup_kind_      = cfg.startup_policy_kind();
    stream_path_       = std::string(stream_path);
    shared_metrics_    = metrics;
    callbacks_         = std::move(cbs);
  }

  /// Binds this shard's dedicated `(even RTP, odd RTCP = RTP + 1)` UDP socket pair.
  bool bind_dedicated_udp_ports() noexcept {
    if (owns_udp_sockets_ && udp_rtp_socket_ >= 0) {
      return true;
    }
    int      r_fd = -1;
    int      c_fd = -1;
    uint16_t r_p  = 0;
    uint16_t c_p  = 0;
    if (!net::bind_udp_port_pair(rtp_port, r_fd, c_fd, r_p, c_p, true)) {
      return false;
    }
    udp_rtp_socket_   = r_fd;
    udp_rtcp_socket_  = c_fd;
    rtp_port          = r_p;
    rtcp_port         = c_p;
    owns_udp_sockets_ = true;
    (void)poller_.add_or_mod(udp_rtcp_socket_, net::kPollRead, kTagUdpRtcp);
    return true;
  }

  [[nodiscard]] uint32_t get_reactor_id() const noexcept { return reactor_id; }
  [[nodiscard]] uint16_t get_rtp_port() const noexcept { return rtp_port; }
  [[nodiscard]] uint16_t get_rtcp_port() const noexcept { return rtcp_port; }

  void set_spsc_channel(PacketRouter::PacketQueue *channel) noexcept { spsc_channel = channel; }
  void wake() noexcept { poller_.wake(); }

  void set_quiesced(bool q) noexcept {
    quiesced_.store(q, std::memory_order_release);
    poller_.wake();
  }

  void set_udp_sockets(int v_sock, int a_sock) noexcept {
    udp_video_socket = v_sock;
    udp_audio_socket = a_sock;
  }

  /// Hands off a newly connected viewer TCP socket (`DESCRIBE` / `SETUP`) to this shard thread.
  void handoff_viewer(int client_fd, std::string client_ip, std::string initial_bytes) {
    if (client_fd < 0) {
      return;
    }
    {
      std::lock_guard<std::mutex> lk(handoff_mutex_);
      handoff_inbox_.push_back(
          ViewerHandoffCmd{client_fd, std::move(client_ip), std::move(initial_bytes)});
    }
    if (thread_active_.load(std::memory_order_acquire)) {
      poller_.wake();
    } else {
      drain_inboxes();
    }
  }

  /// Registers a subscriber directly (thread-safe when reactor thread is running; synchronous in
  /// single-threaded unit tests).
  void add_subscriber(std::unique_ptr<SubscriberSession> session,
      const media::RetainedPacketLog                    *packet_log = nullptr) {
    if (!session) {
      return;
    }
    if (thread_active_.load(std::memory_order_acquire) &&
        std::this_thread::get_id() != worker_thread_.get_id()) {
      {
        std::lock_guard<std::mutex> lk(handoff_mutex_);
        external_sub_inbox_.emplace_back(std::move(session), packet_log);
      }
      poller_.wake();
      return;
    }
    add_subscriber_local(std::move(session), packet_log);
  }

  void remove_subscriber(uint64_t session_id) {
    if (thread_active_.load(std::memory_order_acquire) &&
        std::this_thread::get_id() != worker_thread_.get_id()) {
      {
        std::lock_guard<std::mutex> lk(handoff_mutex_);
        external_remove_inbox_.push_back(session_id);
      }
      poller_.wake();
      return;
    }
    remove_subscriber_internal(session_id);
  }

private:
  void add_subscriber_local(std::unique_ptr<SubscriberSession> session,
      const media::RetainedPacketLog                          *packet_log) {
    if (!session) {
      return;
    }

    uint32_t max_irap_bytes = 0;
    if (packet_log) {
      max_irap_bytes = packet_log->ring().max_recent_irap_bytes();
    }

    if (packet_log && session->is_playing() && session->get_track_id() == 0 &&
        session->get_startup_policy()) {
      auto burst = session->get_startup_policy()->start(*packet_log);
      for (auto &pkt : burst) {
        const size_t len = pkt.length;
        enforce_reactor_quota_fairly(len);
        if (session->enqueue_packet(std::move(pkt), true, poller_.now_ns(), max_irap_bytes)) {
          aggregate_queued_bytes += len;
        }
      }
    }

    subscribers_.insert(std::move(session), max_irap_bytes);
    sync_sub_count();
    flush_subscriber_sends();
  }

  void drain_inboxes() {
    std::vector<ViewerHandoffCmd> handoffs;
    std::vector<std::pair<std::unique_ptr<SubscriberSession>, const media::RetainedPacketLog *>>
                          ext_adds;
    std::vector<uint64_t> ext_rems;

    {
      std::lock_guard<std::mutex> lk(handoff_mutex_);
      handoffs.swap(handoff_inbox_);
      ext_adds.swap(external_sub_inbox_);
      ext_rems.swap(external_remove_inbox_);
    }

    for (uint64_t sid : ext_rems) {
      remove_subscriber_internal(sid);
    }
    for (auto &pair : ext_adds) {
      add_subscriber_local(std::move(pair.first), pair.second);
    }
    for (auto &cmd : handoffs) {
      accept_viewer_handoff(std::move(cmd));
    }
  }

  void accept_viewer_handoff(ViewerHandoffCmd cmd) {
    if (cmd.fd < 0) {
      return;
    }
    if (quiesced_.load(std::memory_order_acquire)) {
      ::close(cmd.fd);
      return;
    }
    net::configure_tcp_viewer_socket(cmd.fd);

    auto conn              = std::make_unique<ViewerConnection>();
    conn->fd               = cmd.fd;
    conn->client_ip        = std::move(cmd.client_ip);
    conn->last_activity_ns = poller_.refresh_now_ns();
    conn->rtsp_session.set_base_ports(rtp_port, rtcp_port);
    conn->rtsp_session.set_server_ports(rtp_port, rtcp_port);
    conn->rtsp_session.set_stream_path(stream_path_);

    if (!cmd.initial_bytes.empty()) {
      if (!conn->read_buf.append(cmd.initial_bytes.data(), cmd.initial_bytes.size())) {
        ::close(cmd.fd);
        return;
      }
    }

    const int fd      = conn->fd;
    viewer_conns_[fd] = std::move(conn);
    process_viewer_frames(fd);
    if (auto it = viewer_conns_.find(fd); it != viewer_conns_.end()) {
      flush_viewer_connection(*it->second);
      if (it->second->close_after_control_flush && !it->second->has_pending_control()) {
        close_viewer_connection(fd);
      } else {
        update_viewer_poll_interest(*it->second);
      }
    }
  }

  void handle_viewer_readable(int fd) {
    auto it = viewer_conns_.find(fd);
    if (it == viewer_conns_.end()) {
      return;
    }
    auto &conn = *it->second;

    while (true) {
      auto span = conn.read_buf.writable_span(4096);
      if (span.empty()) {
        close_viewer_connection(fd);
        return;
      }
      const ssize_t n = ::recv(fd, span.data(), span.size(), MSG_DONTWAIT);
      if (n > 0) {
        conn.read_buf.commit_write(static_cast<size_t>(n));
        conn.last_activity_ns = poller_.now_ns();
        for (uint64_t sid : conn.session_ids) {
          if (auto *s = subscribers_.find(sid)) {
            s->record_client_activity(conn.last_activity_ns);
          }
        }
        continue;
      }
      if (n == 0) {
        close_viewer_connection(fd);
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      close_viewer_connection(fd);
      return;
    }

    process_viewer_frames(fd);
    if (auto vit = viewer_conns_.find(fd); vit != viewer_conns_.end()) {
      flush_viewer_connection(*vit->second);
      if (vit->second->close_after_control_flush && !vit->second->has_pending_control()) {
        close_viewer_connection(fd);
      } else {
        update_viewer_poll_interest(*vit->second);
      }
    }
  }

  void process_viewer_frames(int fd) {
    while (true) {
      auto it = viewer_conns_.find(fd);
      if (it == viewer_conns_.end()) {
        return;
      }
      auto &conn = *it->second;
      auto  view = conn.read_buf.next_view();
      if (view.type == proto::rtsp::FramedMessageType::None) {
        break;
      }
      if (view.type == proto::rtsp::FramedMessageType::Error) {
        close_viewer_connection(fd);
        return;
      }

      if (view.type == proto::rtsp::FramedMessageType::Interleaved) {
        /// Inbound viewer `$` frame (RTCP RR / PLI / BYE on negotiated RTCP channel)
        const auto binding = conn.rtsp_session.lookup_interleaved_channel(view.channel);
        if (binding.bound && binding.is_rtcp) {
          handle_inbound_rtcp(view.bytes, &conn);
        }
        conn.read_buf.consume(view.wire_bytes);
        continue;
      }

      if (view.type == proto::rtsp::FramedMessageType::RtspRequest) {
        const std::string req_copy(view.as_text());
        conn.read_buf.consume(view.wire_bytes);
        handle_viewer_rtsp_request(conn, req_copy);
      }
    }
  }

  void handle_viewer_rtsp_request(ViewerConnection &conn, std::string_view req_text) {
    const auto parsed = proto::rtsp::parse_rtsp_request_text(req_text);

    /// Refresh latest StreamDescriptor + ConfigGeneration before DESCRIBE / SETUP
    std::optional<media::StreamDescriptor>         desc_opt;
    std::shared_ptr<const media::ConfigGeneration> cfg_snap;
    if (callbacks_.get_descriptor) {
      desc_opt = callbacks_.get_descriptor();
    }
    if (callbacks_.get_config_snapshot) {
      cfg_snap = callbacks_.get_config_snapshot();
    }
    if (parsed.valid &&
        (parsed.method == "DESCRIBE" ||
            (parsed.method == "SETUP" && !conn.rtsp_session.get_stream_descriptor().has_value()))) {
      conn.rtsp_session.set_stream_context(desc_opt, cfg_snap, false);
    }

    /// Before PLAY: compute cursor `StartupPlan` per SETUP track and populate `RTP-Info`
    if (parsed.valid && parsed.method == "PLAY" &&
        (conn.rtsp_session.get_state() == proto::rtsp::RtspState::Ready ||
            conn.rtsp_session.get_state() == proto::rtsp::RtspState::Playing) &&
        parsed.session == conn.rtsp_session.get_session_id()) {
      const media::RetainedPacketLog *plog =
          callbacks_.get_packet_log ? callbacks_.get_packet_log() : nullptr;
      std::vector<proto::rtsp::PlayTrackRtpInfo> rtp_infos;

      for (uint64_t sid : conn.session_ids) {
        auto *sub = subscribers_.find(sid);
        if (!sub) {
          continue;
        }
        const uint8_t trk = sub->get_track_id();

        media::StartupPlan plan{};
        if (plog && sub->is_video_track() && sub->get_startup_policy()) {
          plan = sub->get_startup_policy()->plan_cursor(plog->ring(),
              trk,
              conn.rtsp_session.get_advertised_generation(),
              burst_budget_bytes_,
              join_on_open_irap_);
        } else if (plog) {
          plan.start_pos = plog->ring().write_pos();
          plan.live_pos  = plan.start_pos;
          plan.wait_irap = false;
        }

        proto::rtsp::PlayTrackRtpInfo pinfo{};
        pinfo.track_index = trk;
        pinfo.track_url   = "trackID=" + std::to_string(trk);

        if (plog && plan.has_rap) {
          if (auto first_pkt = plog->ring().try_get_shard(plan.start_pos, reactor_id)) {
            sub->get_sender_context().anchor_initial(first_pkt->sequence, first_pkt->rtp_timestamp);
            pinfo.has_seq_and_ts = true;
            pinfo.seq            = sub->get_sender_context().first_seq_out();
            pinfo.rtptime        = sub->get_sender_context().first_ts_out();
          }
        } else if (plog && plog->ring().write_pos() > plog->ring().oldest_pos()) {
          if (auto tail_pkt =
                  plog->ring().try_get_shard(plog->ring().write_pos() - 1, reactor_id)) {
            sub->get_sender_context().anchor_initial(
                static_cast<uint16_t>(tail_pkt->sequence + 1U), tail_pkt->rtp_timestamp);
            pinfo.has_seq_and_ts = true;
            pinfo.seq            = sub->get_sender_context().first_seq_out();
            pinfo.rtptime        = sub->get_sender_context().first_ts_out();
          }
        }

        rtp_infos.push_back(std::move(pinfo));
        sub->arm_startup_plan(plan, poller_.now_ns());
        sub->set_playing(true);

        if (sub->consume_pli_request() && callbacks_.request_upstream_pli) {
          callbacks_.request_upstream_pli();
        }
      }
      conn.rtsp_session.set_play_rtp_info(std::move(rtp_infos));
    }

    const auto  prev_state = conn.rtsp_session.get_state();
    std::string resp       = conn.rtsp_session.handle_request(req_text);

    /// Always enqueue RTSP response into `control_outbox` BEFORE any media frames can flush!
    conn.append_control(resp);

    if (!parsed.valid || !resp.starts_with("RTSP/1.0 200")) {
      return;
    }

    if (parsed.method == "SETUP" && !conn.rtsp_session.is_record_mode()) {
      const uint8_t trk     = conn.rtsp_session.get_last_setup_track();
      const auto   &binding = conn.rtsp_session.track_binding(trk);
      if (binding.is_setup) {
        /// Check if this track already had a session on `conn` (re-SETUP)
        SubscriberSession *existing = nullptr;
        for (uint64_t sid : conn.session_ids) {
          if (auto *s = subscribers_.find(sid); s && s->get_track_id() == trk) {
            existing = s;
            break;
          }
        }

        bool    is_vid      = (trk == 0);
        uint8_t highest_tid = 0;
        if (desc_opt && trk < desc_opt->tracks.size()) {
          is_vid = (desc_opt->tracks[trk].kind == media::MediaKind::Video);
        }
        if (cfg_snap) {
          highest_tid = cfg_snap->highest_tid;
        }

        if (!existing) {
          const uint64_t sid = (static_cast<uint64_t>(reactor_id + 1U) << 48U) |
              next_local_session_id_.fetch_add(1, std::memory_order_relaxed);
          const auto trans = binding.is_interleaved ? SubscriberTransport::TcpInterleaved
                                                    : SubscriberTransport::UdpDedicatedPorts;
          const auto smode = (startup_kind_ == config::StartupPolicyKind::FastPictureFirst)
              ? proto::rtp::SenderMode::SessionOriginTranslation
              : proto::rtp::SenderMode::TransparentPassThrough;

          auto sub = std::make_unique<SubscriberSession>(sid,
              conn.fd,
              trans,
              conn.client_ip,
              binding.client_rtp_port,
              binding.client_rtcp_port,
              random_u32(),
              smode);
          sub->set_track_id(trk, is_vid);
          sub->set_tcp_channel(binding.rtp_channel);
          sub->set_rtcp_tcp_channel(binding.rtcp_channel);
          sub->set_stream_params(highest_tid, join_on_open_irap_, max_sub_queue_bytes_);
          sub->set_advertised_generation(conn.rtsp_session.get_advertised_generation());
          sub->set_playing(false); //< Parked in `Ready` state until `PLAY`!

          if (startup_kind_ == config::StartupPolicyKind::FreshnessFirst) {
            sub->set_startup_policy(std::make_unique<media::FreshnessFirstPolicy>());
          } else {
            sub->set_startup_policy(
                std::make_unique<media::FastPictureFirstPolicy>(burst_budget_bytes_,
                    conn.rtsp_session.get_advertised_generation(),
                    join_on_open_irap_,
                    trk));
          }

          conn.session_ids.push_back(sid);
          subscribers_.insert(std::move(sub));
          sync_sub_count();
        } else {
          existing->set_udp_ports(binding.client_rtp_port, binding.client_rtcp_port);
          existing->set_tcp_channel(binding.rtp_channel);
          existing->set_rtcp_tcp_channel(binding.rtcp_channel);
        }
      }
    } else if (parsed.method == "PLAY") {
      /// Sync SoA eligibility slots now that `sub->is_playing()` is true
      const media::RetainedPacketLog *plog =
          callbacks_.get_packet_log ? callbacks_.get_packet_log() : nullptr;
      const uint32_t max_irap = plog ? plog->ring().max_recent_irap_bytes() : 0;
      for (uint8_t t = 0; t < SubscriberTable::kMaxTracks; ++t) {
        auto &grp = subscribers_.track_group(t);
        for (size_t i = 0; i < grp.sessions.size(); ++i) {
          grp.sync_slot(i, max_irap);
        }
      }
    } else if (parsed.method == "TEARDOWN" ||
        (prev_state != proto::rtsp::RtspState::Init &&
            conn.rtsp_session.get_state() == proto::rtsp::RtspState::Init)) {
      /// Remove all subscriber tracks immediately and close TCP connection once `200 OK` flushes
      for (uint64_t sid : conn.session_ids) {
        remove_subscriber_internal(sid);
      }
      conn.session_ids.clear();
      conn.close_after_control_flush = true;
    }
  }

  void handle_inbound_rtcp(std::span<const uint8_t> data, ViewerConnection *tcp_conn) noexcept {
    const auto summary = proto::rtp::RtcpReportGenerator::parse(data);
    if (!summary.valid) {
      return;
    }
    const uint64_t now = poller_.now_ns();
    if (tcp_conn) {
      tcp_conn->last_activity_ns = now;
      for (uint64_t sid : tcp_conn->session_ids) {
        if (auto *s = subscribers_.find(sid)) {
          s->record_client_activity(now);
        }
      }
    }

    if ((summary.packet_type == proto::rtp::kRtcpPtPsfb ||
            summary.packet_type == proto::rtp::kRtcpPtRtpfb) &&
        callbacks_.request_upstream_pli) {
      callbacks_.request_upstream_pli();
    }
  }

  void drain_udp_rtcp_socket() noexcept {
    if (udp_rtcp_socket_ < 0) {
      return;
    }
    uint8_t buf[1500];
    while (true) {
      sockaddr_in   src{};
      socklen_t     slen = sizeof(src);
      const ssize_t n    = ::recvfrom(udp_rtcp_socket_,
          buf,
          sizeof(buf),
          MSG_DONTWAIT,
          reinterpret_cast<sockaddr *>(&src),
          &slen);
      if (n <= 0) {
        break;
      }
      const auto summary = proto::rtp::RtcpReportGenerator::parse(
          std::span<const uint8_t>{buf, static_cast<size_t>(n)});
      if (!summary.valid) {
        continue;
      }

      const uint64_t now      = poller_.now_ns();
      const uint16_t src_port = ntohs(src.sin_port);
      uint64_t       bye_sid  = 0;

      subscribers_.for_each([&](SubscriberSession &s) {
        if (s.get_transport() == SubscriberTransport::UdpDedicatedPorts && s.has_rtcp_dest()) {
          if (s.rtcp_dest_addr().sin_addr.s_addr == src.sin_addr.s_addr &&
              (s.get_client_rtcp_port() == src_port || s.get_client_rtp_port() == src_port)) {
            s.record_client_activity(now);
            if (summary.packet_type == proto::rtp::kRtcpPtBye) {
              bye_sid = s.get_session_id();
            }
          }
        }
      });

      if (bye_sid != 0) {
        remove_subscriber_internal(bye_sid);
      }
      if ((summary.packet_type == proto::rtp::kRtcpPtPsfb ||
              summary.packet_type == proto::rtp::kRtcpPtRtpfb) &&
          callbacks_.request_upstream_pli) {
        callbacks_.request_upstream_pli();
      }
    }
  }

  /// Advances paced historical RAP cursor bursts into subscriber send queues.
  void pump_startup_bursts() {
    const media::RetainedPacketLog *plog =
        callbacks_.get_packet_log ? callbacks_.get_packet_log() : nullptr;
    if (!plog) {
      return;
    }
    const auto    &ring     = plog->ring();
    const uint64_t now      = poller_.now_ns();
    const uint32_t max_irap = ring.max_recent_irap_bytes();

    for (uint8_t t = 0; t < SubscriberTable::kMaxTracks; ++t) {
      auto &grp = subscribers_.track_group(t);
      for (size_t i = 0; i < grp.sessions.size(); ++i) {
        auto &sub = *grp.sessions[i];
        if (!sub.active() || !sub.is_playing() || !sub.in_startup_burst()) {
          continue;
        }

        uint64_t       tokens = sub.refill_burst_tokens(now);
        uint64_t       cur    = sub.burst_cursor_pos();
        const uint64_t oldest = ring.oldest_pos();
        const uint64_t head   = ring.write_pos();

        if (cur < oldest) {
          /// Cursor fell behind ring retention -> jump to live edge & arm `wait_irap`
          sub.get_au_gate().on_gap_or_overflow();
          sub.finish_startup_burst();
          grp.sync_slot(i, max_irap);
          if (callbacks_.request_upstream_pli) {
            callbacks_.request_upstream_pli();
          }
          continue;
        }

        while (cur < sub.burst_end_pos() && cur < head) {
          if (sub.compute_fill_pct(max_irap) >= 85) {
            break; //< Wait for socket drain before queueing more burst packets
          }
          auto pkt_opt = ring.try_get_shard(cur, reactor_id);
          if (!pkt_opt) {
            sub.get_au_gate().on_gap_or_overflow();
            sub.finish_startup_burst();
            break;
          }
          if (pkt_opt->track() == sub.get_track_id()) {
            if (tokens < pkt_opt->length && sub.queue_size() > 0) {
              break;
            }
            const size_t len = pkt_opt->length;
            enforce_reactor_quota_fairly(len);
            if (sub.enqueue_packet(std::move(*pkt_opt), false, now, max_irap)) {
              aggregate_queued_bytes += len;
              ++total_packets_dispatched;
              sub.consume_burst_tokens(len);
              tokens = (tokens > len) ? (tokens - len) : 0;
            }
          }
          ++cur;
          sub.advance_burst_cursor(cur);
        }

        /// Once caught up to `burst_end_pos`, catch up any remaining delta up to current `head`
        if (cur >= sub.burst_end_pos()) {
          while (cur < head && sub.compute_fill_pct(max_irap) < 85) {
            auto pkt_opt = ring.try_get_shard(cur, reactor_id);
            if (!pkt_opt) {
              break;
            }
            if (pkt_opt->track() == sub.get_track_id()) {
              const size_t len = pkt_opt->length;
              enforce_reactor_quota_fairly(len);
              if (sub.enqueue_packet(std::move(*pkt_opt), false, now, max_irap)) {
                aggregate_queued_bytes += len;
                ++total_packets_dispatched;
              }
            }
            ++cur;
          }
          sub.finish_startup_burst();
        }
        grp.sync_slot(i, max_irap);
      }
    }
  }

  /// Flushes `control_outbox` and in-flight `$` frames on a `ViewerConnection` in strict wire order:
  /// 1. Finish any mid-frame `PartialInterleavedWrite` already started on `conn.fd`.
  /// 2. Flush `conn.control_outbox` (`SETUP 200 OK`, `PLAY 200 OK`, etc.).
  /// 3. Drain subscriber media queues for all tracks bound to `conn.fd`.
  size_t flush_viewer_connection(ViewerConnection &conn) {
    if (conn.fd < 0) {
      return 0;
    }
    size_t packets_flushed = 0;

    /// Step 1: Finish any partially transmitted `$` frame first so framing NEVER tears!
    for (uint64_t sid : conn.session_ids) {
      auto *sub = subscribers_.find(sid);
      if (!sub || sub->get_transport() != SubscriberTransport::TcpInterleaved) {
        continue;
      }
      if (sub->partial_write().active) {
        if (!flush_partial_tcp_frame(*sub)) {
          return packets_flushed; //< Blocked on EAGAIN or fatal error handled
        }
        ++packets_flushed;
      }
    }

    /// Step 2: Flush RTSP text responses (`control_outbox`) BEFORE starting any new `$` frame
    while (conn.has_pending_control()) {
      const char   *ptr = conn.control_outbox.data() + conn.control_outbox_sent;
      const size_t  rem = conn.control_outbox.size() - conn.control_outbox_sent;
      const ssize_t n   = ::send(conn.fd, ptr, rem, net::nonblocking_send_flags());
      if (n > 0) {
        conn.control_outbox_sent += static_cast<size_t>(n);
        for (uint64_t sid : conn.session_ids) {
          if (auto *s = subscribers_.find(sid)) {
            s->notify_send_progress();
          }
        }
        continue;
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return packets_flushed; //< Wait for kPollWrite before sending any media!
      }
      /// Fatal socket error -> mark closed
      conn.close_after_control_flush = true;
      conn.control_outbox.clear();
      conn.control_outbox_sent = 0;
      for (uint64_t sid : conn.session_ids) {
        if (auto *s = subscribers_.find(sid)) {
          s->mark_closed();
        }
      }
      if (shared_metrics_) {
        shared_metrics_->send_errors.fetch_add(1, std::memory_order_relaxed);
      }
      return packets_flushed;
    }

    if (conn.control_outbox_sent == conn.control_outbox.size() && !conn.control_outbox.empty()) {
      conn.control_outbox.clear();
      conn.control_outbox_sent = 0;
    }

    /// Step 3: Drain queued `$` media packets across tracks on `conn.fd`
    for (uint64_t sid : conn.session_ids) {
      auto *sub = subscribers_.find(sid);
      if (!sub || !sub->active() || sub->get_transport() != SubscriberTransport::TcpInterleaved) {
        continue;
      }
      while (true) {
        if (sub->partial_write().active) {
          if (!flush_partial_tcp_frame(*sub)) {
            return packets_flushed;
          }
          ++packets_flushed;
          continue;
        }
        auto pkt_opt = sub->pop_next_send();
        if (!pkt_opt) {
          break;
        }
        aggregate_queued_bytes = (aggregate_queued_bytes >= pkt_opt->length)
            ? (aggregate_queued_bytes - pkt_opt->length)
            : 0;
        begin_tcp_interleaved_frame(*sub, std::move(*pkt_opt));
        if (!flush_partial_tcp_frame(*sub)) {
          return packets_flushed;
        }
        ++packets_flushed;
      }
    }
    return packets_flushed;
  }

  void begin_tcp_interleaved_frame(SubscriberSession &session, memory::PacketRef pkt) noexcept {
    auto &pw       = session.partial_write();
    pw.prefix[0]   = static_cast<uint8_t>('$');
    pw.prefix[1]   = session.get_tcp_channel();
    pw.prefix[2]   = static_cast<uint8_t>((pkt.length >> 8U) & 0xFFU);
    pw.prefix[3]   = static_cast<uint8_t>(pkt.length & 0xFFU);
    pw.total_bytes = 4U + static_cast<size_t>(pkt.length);
    pw.sent_bytes  = 0;
    pw.owned_control_rtcp.clear();

    const uint32_t hlen = session.get_sender_context().build_rewritten_rtp_header(
        pkt, pw.rewritten_rtp_hdr.data(), pw.rewritten_rtp_hdr.size());
    pw.has_rewritten_hdr = (hlen == 12U && pkt.length >= 12U);
    pw.pkt               = std::move(pkt);
    pw.active            = true;
  }

  /// Sends remaining bytes of `session.partial_write()` using zero-copy `sendmsg` (`iovec[3]`).
  /// \return `true` if frame completed, `false` if blocked on `EAGAIN` or fatal error occurred.
  bool flush_partial_tcp_frame(SubscriberSession &session) noexcept {
    auto     &pw = session.partial_write();
    const int fd = session.get_socket_fd();
    if (!pw.active) {
      return true;
    }
    if (fd < 0) {
      /// Mock/unit-test subscriber with `fd == -1`: count frame as immediately drained
      if (shared_metrics_) {
        shared_metrics_->egress_packets_sent.fetch_add(1, std::memory_order_relaxed);
        shared_metrics_->egress_bytes_sent.fetch_add(
            pw.total_bytes >= 4 ? (pw.total_bytes - 4) : 0, std::memory_order_relaxed);
      }
      pw.clear();
      return true;
    }

    while (pw.sent_bytes < pw.total_bytes) {
      struct Segment {
        const uint8_t *ptr;
        size_t         len;
      };
      std::array<Segment, 3> segs{};
      size_t                 seg_count = 0;

      segs[seg_count++] = Segment{pw.prefix.data(), 4U};
      if (!pw.owned_control_rtcp.empty()) {
        segs[seg_count++] = Segment{pw.owned_control_rtcp.data(), pw.owned_control_rtcp.size()};
      } else if (pw.has_rewritten_hdr) {
        segs[seg_count++] = Segment{pw.rewritten_rtp_hdr.data(), 12U};
        if (pw.pkt.length > 12U) {
          segs[seg_count++] =
              Segment{static_cast<const uint8_t *>(pw.pkt.data()) + 12U, pw.pkt.length - 12U};
        }
      } else {
        segs[seg_count++] = Segment{static_cast<const uint8_t *>(pw.pkt.data()), pw.pkt.length};
      }

      std::array<struct iovec, 3> iov{};
      size_t                      iov_n = 0;
      size_t                      skip  = pw.sent_bytes;
      for (size_t s = 0; s < seg_count; ++s) {
        if (skip >= segs[s].len) {
          skip -= segs[s].len;
          continue;
        }
        iov[iov_n].iov_base = const_cast<uint8_t *>(segs[s].ptr + skip);
        iov[iov_n].iov_len  = segs[s].len - skip;
        ++iov_n;
        skip = 0;
      }

      struct msghdr msg{};
      msg.msg_iov    = iov.data();
      msg.msg_iovlen = static_cast<decltype(msg.msg_iovlen)>(iov_n);

      const ssize_t n = ::sendmsg(fd, &msg, net::nonblocking_send_flags());
      if (n > 0) {
        pw.sent_bytes += static_cast<size_t>(n);
        session.notify_send_progress();
        continue;
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        session.notify_send_blocked(poller_.now_ns());
        return false;
      }

      /// Fatal socket error (`EPIPE` / `ECONNRESET`): mark subscriber closed immediately
      pw.clear();
      session.mark_closed();
      if (shared_metrics_) {
        shared_metrics_->send_errors.fetch_add(1, std::memory_order_relaxed);
      }
      return false;
    }

    if (shared_metrics_) {
      if (!pw.owned_control_rtcp.empty()) {
        shared_metrics_->egress_rtcp_packets.fetch_add(1, std::memory_order_relaxed);
      } else {
        shared_metrics_->egress_packets_sent.fetch_add(1, std::memory_order_relaxed);
        shared_metrics_->egress_bytes_sent.fetch_add(pw.pkt.length, std::memory_order_relaxed);
      }
    }
    pw.clear();
    return true;
  }

  int select_udp_out_socket(uint8_t trk) noexcept {
    if (udp_rtp_socket_ >= 0) {
      return udp_rtp_socket_;
    }
    const int legacy = (trk == 1) ? udp_audio_socket : udp_video_socket;
    if (legacy >= 0) {
      return legacy;
    }
    if (udp_rtp_socket_ < 0) {
      uint16_t dummy    = 0;
      udp_rtp_socket_   = net::bind_udp_socket(0, dummy);
      owns_udp_sockets_ = (udp_rtp_socket_ >= 0);
    }
    return udp_rtp_socket_;
  }

  void stage_udp_datagram(SubscriberSession &session, memory::PacketRef pkt) {
    if (!session.has_rtp_dest()) {
      return;
    }
    const int out_sock = select_udp_out_socket(session.get_track_id());
    if (out_sock < 0) {
      return;
    }

    UdpPendingDatagram d{};
    d.dest              = session.rtp_dest_addr();
    d.out_sock          = out_sock;
    d.rewritten_hdr_len = session.get_sender_context().build_rewritten_rtp_header(
        pkt, d.rewritten_hdr.data(), d.rewritten_hdr.size());
    d.pkt = std::move(pkt);
    udp_batch_.push_back(std::move(d));
    if (udp_batch_.size() >= kUdpBatchLimit) {
      flush_udp_batch();
    }
  }

  void flush_udp_batch() noexcept {
    if (udp_batch_.empty()) {
      return;
    }

#if defined(__linux__) && defined(UDP_SEGMENT)
    /// Optional Linux UDP_SEGMENT GSO fast path for same-destination equal-sized runs
    if (enable_udp_gso_ && udp_batch_.size() >= 2) {
      size_t idx = 0;
      while (idx < udp_batch_.size()) {
        const auto    &first    = udp_batch_[idx];
        const uint16_t seg_size = static_cast<uint16_t>(first.pkt.length);
        size_t         run_end  = idx + 1;
        size_t         tot_len  = first.pkt.length;
        while (run_end < udp_batch_.size() && run_end - idx < 32 &&
            udp_batch_[run_end].out_sock == first.out_sock &&
            udp_batch_[run_end].pkt.length == seg_size &&
            udp_batch_[run_end].dest.sin_addr.s_addr == first.dest.sin_addr.s_addr &&
            udp_batch_[run_end].dest.sin_port == first.dest.sin_port &&
            tot_len + seg_size <= 64000U) {
          tot_len += seg_size;
          ++run_end;
        }
        if (run_end - idx >= 2 && seg_size > 0) {
          std::array<struct iovec, 64> iovs{};
          size_t                       iov_n = 0;
          for (size_t k = idx; k < run_end; ++k) {
            auto &item = udp_batch_[k];
            if (item.rewritten_hdr_len > 0 && item.pkt.length >= item.rewritten_hdr_len) {
              iovs[iov_n++] = iovec{item.rewritten_hdr.data(), item.rewritten_hdr_len};
              if (item.pkt.length > item.rewritten_hdr_len) {
                iovs[iov_n++] = iovec{
                    const_cast<void *>(static_cast<const void *>(
                        static_cast<const uint8_t *>(item.pkt.data()) + item.rewritten_hdr_len)),
                    item.pkt.length - item.rewritten_hdr_len};
              }
            } else {
              iovs[iov_n++] = iovec{const_cast<void *>(item.pkt.data()), item.pkt.length};
            }
          }

          alignas(struct cmsghdr) char cbuf[CMSG_SPACE(sizeof(uint16_t))]{};
          struct msghdr                msg{};
          msg.msg_name       = const_cast<sockaddr_in *>(&first.dest);
          msg.msg_namelen    = sizeof(sockaddr_in);
          msg.msg_iov        = iovs.data();
          msg.msg_iovlen     = iov_n;
          msg.msg_control    = cbuf;
          msg.msg_controllen = sizeof(cbuf);
          struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
          cm->cmsg_level     = SOL_UDP;
          cm->cmsg_type      = UDP_SEGMENT;
          cm->cmsg_len       = CMSG_LEN(sizeof(uint16_t));
          std::memcpy(CMSG_DATA(cm), &seg_size, sizeof(uint16_t));

          if (::sendmsg(first.out_sock, &msg, net::nonblocking_send_flags()) >= 0) {
            if (shared_metrics_) {
              shared_metrics_->egress_packets_sent.fetch_add(
                  run_end - idx, std::memory_order_relaxed);
              shared_metrics_->egress_bytes_sent.fetch_add(tot_len, std::memory_order_relaxed);
            }
            idx = run_end;
            continue;
          }
          /// Kernel/NIC rejected UDP_SEGMENT -> auto-disable GSO and fall through to sendmmsg
          enable_udp_gso_ = false;
          break;
        }
        break;
      }
      if (enable_udp_gso_ && idx == udp_batch_.size()) {
        udp_batch_.clear();
        return;
      }
    }
#endif

#if defined(__linux__)
    /// Batched `sendmmsg`: up to 64 datagrams in a single syscall
    size_t i = 0;
    while (i < udp_batch_.size()) {
      const int                                               sock = udp_batch_[i].out_sock;
      std::array<struct mmsghdr, kUdpBatchLimit>              mmsgs{};
      std::array<std::array<struct iovec, 2>, kUdpBatchLimit> iovs{};
      size_t                                                  batch_n = 0;

      while (i + batch_n < udp_batch_.size() && batch_n < kUdpBatchLimit &&
          udp_batch_[i + batch_n].out_sock == sock) {
        auto &d         = udp_batch_[i + batch_n];
        auto &hdr       = mmsgs[batch_n].msg_hdr;
        hdr.msg_name    = &d.dest;
        hdr.msg_namelen = sizeof(d.dest);
        if (d.rewritten_hdr_len > 0 && d.pkt.length >= d.rewritten_hdr_len) {
          iovs[batch_n][0] = iovec{d.rewritten_hdr.data(), d.rewritten_hdr_len};
          if (d.pkt.length > d.rewritten_hdr_len) {
            iovs[batch_n][1] =
                iovec{const_cast<uint8_t *>(
                          static_cast<const uint8_t *>(d.pkt.data()) + d.rewritten_hdr_len),
                    d.pkt.length - d.rewritten_hdr_len};
            hdr.msg_iovlen = 2;
          } else {
            hdr.msg_iovlen = 1;
          }
        } else {
          iovs[batch_n][0] = iovec{const_cast<void *>(d.pkt.data()), d.pkt.length};
          hdr.msg_iovlen   = 1;
        }
        hdr.msg_iov = iovs[batch_n].data();
        ++batch_n;
      }

      const int sent = ::sendmmsg(
          sock, mmsgs.data(), static_cast<unsigned int>(batch_n), net::nonblocking_send_flags());
      if (sent > 0 && shared_metrics_) {
        uint64_t bytes = 0;
        for (int k = 0; k < sent; ++k) {
          bytes += udp_batch_[i + static_cast<size_t>(k)].pkt.length;
        }
        shared_metrics_->egress_packets_sent.fetch_add(
            static_cast<uint64_t>(sent), std::memory_order_relaxed);
        shared_metrics_->egress_bytes_sent.fetch_add(bytes, std::memory_order_relaxed);
      } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && shared_metrics_) {
        shared_metrics_->send_errors.fetch_add(1, std::memory_order_relaxed);
      }
      i += batch_n;
    }
#else
    /// Portable macOS/BSD zero-copy scatter-gather `sendmsg` over precomputed `sockaddr_in`
    for (auto &d : udp_batch_) {
      std::array<struct iovec, 2> iov{};
      size_t                      iov_n = 0;
      if (d.rewritten_hdr_len > 0 && d.pkt.length >= d.rewritten_hdr_len) {
        iov[iov_n++] = iovec{d.rewritten_hdr.data(), d.rewritten_hdr_len};
        if (d.pkt.length > d.rewritten_hdr_len) {
          iov[iov_n++] = iovec{const_cast<uint8_t *>(static_cast<const uint8_t *>(d.pkt.data()) +
                                   d.rewritten_hdr_len),
              d.pkt.length - d.rewritten_hdr_len};
        }
      } else {
        iov[iov_n++] = iovec{const_cast<void *>(d.pkt.data()), d.pkt.length};
      }

      struct msghdr msg{};
      msg.msg_name    = &d.dest;
      msg.msg_namelen = sizeof(d.dest);
      msg.msg_iov     = iov.data();
      msg.msg_iovlen  = static_cast<decltype(msg.msg_iovlen)>(iov_n);

      const ssize_t rc = ::sendmsg(d.out_sock, &msg, net::nonblocking_send_flags());
      if (rc >= 0) {
        if (shared_metrics_) {
          shared_metrics_->egress_packets_sent.fetch_add(1, std::memory_order_relaxed);
          shared_metrics_->egress_bytes_sent.fetch_add(d.pkt.length, std::memory_order_relaxed);
        }
      } else if (errno != EAGAIN && errno != EWOULDBLOCK && shared_metrics_) {
        shared_metrics_->send_errors.fetch_add(1, std::memory_order_relaxed);
      }
    }
#endif
    udp_batch_.clear();
  }

  void run_housekeeping(uint64_t now_ns) {
    if (now_ns < last_housekeeping_ns_ + 200'000'000ULL) {
      return; //< Every 200 ms
    }
    last_housekeeping_ns_ = now_ns;

    /// 1. Check viewer TCP connections for L4 2-second stall, dead state, or idle timeout
    std::vector<std::pair<int, bool>> to_close;
    for (const auto &kv : viewer_conns_) {
      const auto &conn              = *kv.second;
      bool        stalled           = false;
      bool        any_alive_or_init = conn.session_ids.empty();
      for (uint64_t sid : conn.session_ids) {
        if (const auto *s = subscribers_.find(sid)) {
          if (s->active()) {
            any_alive_or_init = true;
          }
          if (s->is_tcp_stalled(now_ns)) {
            stalled = true;
          }
        }
      }
      const bool idle_expired = (conn.last_activity_ns != 0 && now_ns > conn.last_activity_ns &&
          (now_ns - conn.last_activity_ns) >= client_timeout_ns_);
      if (stalled || idle_expired || !any_alive_or_init) {
        to_close.emplace_back(kv.first, stalled);
      }
    }
    for (auto [fd, was_stalled] : to_close) {
      close_viewer_connection(fd, was_stalled);
    }

    /// 2. Check standalone UDP subscribers for client RTCP inactivity timeout
    std::vector<uint64_t> dead_subs;
    subscribers_.for_each([&](SubscriberSession &s) {
      if (!s.active() ||
          (s.get_transport() == SubscriberTransport::UdpDedicatedPorts && s.get_socket_fd() < 0 &&
              s.is_client_timed_out(now_ns, client_timeout_ns_))) {
        dead_subs.push_back(s.get_session_id());
      }
    });
    for (uint64_t sid : dead_subs) {
      remove_subscriber_internal(sid);
    }

    /// 3. Periodic RTCP Sender Reports every 5 seconds
    if (now_ns >= last_sr_ns_ + 5'000'000'000ULL) {
      last_sr_ns_ = now_ns;
      emit_periodic_rtcp_sr();
    }
  }

  void emit_periodic_rtcp_sr() noexcept {
    const auto sys_now = std::chrono::system_clock::now().time_since_epoch();
    const auto secs    = std::chrono::duration_cast<std::chrono::seconds>(sys_now).count();
    /// NTP epoch offset: 2208988800 seconds (1900 -> 1970)
    const uint32_t ntp_sec = static_cast<uint32_t>(secs + 2208988800LL);

    subscribers_.for_each([&](SubscriberSession &s) {
      if (!s.active() || !s.is_playing() || !s.get_sender_context().is_anchored()) {
        return;
      }
      proto::rtp::SenderReportInfo sr_info{};
      sr_info.ntp_sec             = ntp_sec;
      sr_info.ntp_frac            = 0;
      sr_info.rtp_timestamp       = s.get_sender_context().last_ts_out();
      sr_info.sender_packet_count = s.get_sender_context().sent_packet_count();
      sr_info.sender_octet_count  = s.get_sender_context().sent_octet_count();

      proto::rtp::RtcpReportGenerator gen(s.get_sender_context().get_ssrc_out());
      const auto                      sr_pkt = gen.build_sr(sr_info);

      if (s.get_transport() == SubscriberTransport::UdpDedicatedPorts && s.has_rtcp_dest() &&
          udp_rtcp_socket_ >= 0) {
        const ssize_t rc = ::sendto(udp_rtcp_socket_,
            sr_pkt.data(),
            sr_pkt.size(),
            net::nonblocking_send_flags(),
            reinterpret_cast<const sockaddr *>(&s.rtcp_dest_addr()),
            sizeof(sockaddr_in));
        if (rc > 0 && shared_metrics_) {
          shared_metrics_->egress_rtcp_packets.fetch_add(1, std::memory_order_relaxed);
        }
      } else if (s.get_transport() == SubscriberTransport::TcpInterleaved &&
          s.get_socket_fd() >= 0 && !s.partial_write().active) {
        auto it = viewer_conns_.find(s.get_socket_fd());
        if (it != viewer_conns_.end() && !it->second->has_pending_control()) {
          auto &pw     = s.partial_write();
          pw.prefix[0] = static_cast<uint8_t>('$');
          pw.prefix[1] = s.get_rtcp_tcp_channel();
          pw.prefix[2] = 0;
          pw.prefix[3] = static_cast<uint8_t>(sr_pkt.size());
          pw.owned_control_rtcp.assign(sr_pkt.begin(), sr_pkt.end());
          pw.total_bytes = 4U + sr_pkt.size();
          pw.sent_bytes  = 0;
          pw.active      = true;
          (void)flush_partial_tcp_frame(s);
        }
      }
    });
  }

  void thread_loop() {
    std::array<net::PollEvent, 64> events{};
    while (is_running.load(std::memory_order_acquire)) {
      const size_t n = poller_.wait(events, 10);
      drain_inboxes();

      for (size_t i = 0; i < n; ++i) {
        if ((events[i].flags & net::kPollWake) != 0) {
          continue;
        }
        if (events[i].tag == kTagUdpRtcp) {
          drain_udp_rtcp_socket();
          continue;
        }
        const int fd = static_cast<int>(events[i].tag);
        if ((events[i].flags & net::kPollRead) != 0) {
          handle_viewer_readable(fd);
        }
        if ((events[i].flags & net::kPollWrite) != 0) {
          if (auto it = viewer_conns_.find(fd); it != viewer_conns_.end()) {
            flush_viewer_connection(*it->second);
            if (it->second->close_after_control_flush && !it->second->has_pending_control()) {
              close_viewer_connection(fd);
            } else {
              update_viewer_poll_interest(*it->second);
            }
          }
        }
        if ((events[i].flags & (net::kPollError | net::kPollHup)) != 0) {
          if (viewer_conns_.find(fd) != viewer_conns_.end()) {
            handle_viewer_readable(fd); //< Drains trailing bytes or closes on EOF
          }
        }
      }

      pump_startup_bursts();
      poll_and_dispatch();
      flush_subscriber_sends();
      run_housekeeping(poller_.now_ns());

      /// If quiesced and all existing viewers have drained/disconnected, exit cleanly (Tier 2)
      if (quiesced_.load(std::memory_order_acquire) && subscribers_.empty() &&
          viewer_conns_.empty()) {
        break;
      }
    }
  }

public:
  /// Drains incoming SPSC channel and dispatches packets to subscribers matching `packet.track()`.
  size_t poll_and_dispatch() {
    RIVET_ZONE("Egress::poll_and_dispatch");
    if (!spsc_channel) {
      return 0;
    }

    size_t            dispatched = 0;
    memory::PacketRef packet;
    while (spsc_channel->try_pop(packet)) {
      ++dispatched;
      dispatch_packet_to_subscribers(packet);
    }
    return dispatched;
  }

  /// Broadcasts `packet` ONLY to subscribers of `packet.track()` via SIMD SoA pre-filtering
  /// and fair laggiest-subscriber quota enforcement.
  bool dispatch_packet_to_subscribers(const memory::PacketRef &packet) {
    const uint8_t trk = static_cast<uint8_t>(packet.track() % SubscriberTable::kMaxTracks);
    auto         &grp = subscribers_.track_group(trk);
    if (grp.sessions.empty()) {
      return false;
    }

    const media::RetainedPacketLog *plog =
        callbacks_.get_packet_log ? callbacks_.get_packet_log() : nullptr;
    const uint32_t max_irap = plog ? plog->ring().max_recent_irap_bytes() : 0;
    const uint64_t now      = poller_.now_ns();

    /// Fair 16 MiB shard quota enforcement: evict laggiest subscriber rather than dropping healthy
    enforce_reactor_quota_fairly(packet.length);

    /// Refresh SoA masks and run SIMD eligibility filter (`fill_pct < 100 && active_mask != 0`)
    const size_t count = grp.sessions.size();
    for (size_t i = 0; i < count; ++i) {
      grp.sync_slot(i, max_irap);
    }
    if (eligible_scratch_.size() < count) {
      eligible_scratch_.resize(count);
    }
    const size_t elig_n = SubscriberTable::filter_eligible_simd(
        grp.active_mask.data(), grp.fill_pct.data(), count, 100U, eligible_scratch_.data());

    bool                  any_dispatched = false;
    bool                  need_pli       = false;
    std::vector<uint64_t> to_evict;

    /// Check any slots at >= 100% fill so they receive `Disconnect` transition properly
    if (elig_n < count) {
      for (size_t i = 0; i < count; ++i) {
        auto &sub = *grp.sessions[i];
        if (!sub.active()) {
          to_evict.push_back(sub.get_session_id());
        } else if (grp.active_mask[i] != 0 && grp.fill_pct[i] >= 100U) {
          (void)sub.enqueue_packet(packet, false, now, max_irap);
          ++total_packets_dropped;
          if (shared_metrics_) {
            shared_metrics_->congestion_packet_drops.fetch_add(1, std::memory_order_relaxed);
          }
          if (!sub.active()) {
            to_evict.push_back(sub.get_session_id());
          }
        }
      }
    }

    for (size_t k = 0; k < elig_n; ++k) {
      const uint32_t idx        = eligible_scratch_[k];
      auto          &sub        = *grp.sessions[idx];
      const uint64_t prev_au_dr = sub.get_aus_dropped();

      if (sub.enqueue_packet(packet, false, now, max_irap)) {
        aggregate_queued_bytes += packet.length;
        ++total_packets_dispatched;
        any_dispatched = true;
      } else {
        ++total_packets_dropped;
        if (shared_metrics_) {
          shared_metrics_->congestion_packet_drops.fetch_add(1, std::memory_order_relaxed);
          if (sub.get_aus_dropped() > prev_au_dr) {
            shared_metrics_->congestion_au_drops.fetch_add(
                sub.get_aus_dropped() - prev_au_dr, std::memory_order_relaxed);
          }
        }
        if (!sub.active()) {
          to_evict.push_back(sub.get_session_id());
        }
      }
      if (sub.consume_pli_request()) {
        need_pli = true;
      }
      grp.sync_slot(idx, max_irap);
    }

    for (uint64_t sid : to_evict) {
      evict_slow_subscriber(sid);
    }
    if (need_pli && callbacks_.request_upstream_pli) {
      callbacks_.request_upstream_pli();
    }
    return any_dispatched;
  }

  /// Flushes queued packets across both viewer TCP connections and UDP subscribers.
  size_t flush_subscriber_sends() {
    RIVET_ZONE("Egress::flush_subscriber_sends");
    size_t sends_flushed = 0;

    /// 1. Flush viewer TCP connections (preserving RTSP `control_outbox` before `$` media order!)
    std::vector<int> dead_fds;
    for (auto &kv : viewer_conns_) {
      sends_flushed += flush_viewer_connection(*kv.second);
      if (kv.second->close_after_control_flush && !kv.second->has_pending_control()) {
        dead_fds.push_back(kv.first);
      } else {
        update_viewer_poll_interest(*kv.second);
      }
    }
    for (int fd : dead_fds) {
      close_viewer_connection(fd);
    }

    /// 2. Flush UDP subscribers (and standalone TCP subscribers not registered in `viewer_conns_`)
    std::vector<uint64_t> dead_subs;
    for (uint8_t t = 0; t < SubscriberTable::kMaxTracks; ++t) {
      auto &grp = subscribers_.track_group(t);
      for (size_t i = 0; i < grp.sessions.size(); ++i) {
        auto &sub = *grp.sessions[i];
        if (!sub.active()) {
          dead_subs.push_back(sub.get_session_id());
          continue;
        }
        if (sub.get_transport() == SubscriberTransport::TcpInterleaved &&
            sub.get_socket_fd() >= 0 &&
            viewer_conns_.find(sub.get_socket_fd()) != viewer_conns_.end()) {
          continue; //< Already flushed via `flush_viewer_connection` above
        }
        sends_flushed += drain_subscriber_queue(sub);
        if (!sub.active()) {
          dead_subs.push_back(sub.get_session_id());
        }
      }
    }
    flush_udp_batch();

    for (uint64_t sid : dead_subs) {
      remove_subscriber_internal(sid);
    }
    return sends_flushed;
  }

  size_t drain_subscriber_queue(SubscriberSession &session) {
    size_t drained = 0;
    if (session.get_transport() == SubscriberTransport::TcpInterleaved) {
      if (session.partial_write().active) {
        if (!flush_partial_tcp_frame(session)) {
          return 0;
        }
        ++drained;
      }
      while (auto pkt_opt = session.pop_next_send()) {
        aggregate_queued_bytes = (aggregate_queued_bytes >= pkt_opt->length)
            ? (aggregate_queued_bytes - pkt_opt->length)
            : 0;
        begin_tcp_interleaved_frame(session, std::move(*pkt_opt));
        if (!flush_partial_tcp_frame(session)) {
          break;
        }
        ++drained;
      }
      return drained;
    }

    /// UDP subscriber
    while (auto pkt_opt = session.pop_next_send()) {
      ++drained;
      aggregate_queued_bytes = (aggregate_queued_bytes >= pkt_opt->length)
          ? (aggregate_queued_bytes - pkt_opt->length)
          : 0;
      stage_udp_datagram(session, std::move(*pkt_opt));
    }
    return drained;
  }

  /// Synchronous single-step tick (used by single-threaded unit tests & `egress_shards == 0` mode)
  void step() {
    poller_.refresh_now_ns();
    drain_inboxes();
    drain_udp_rtcp_socket();
    pump_startup_bursts();
    poll_and_dispatch();
    flush_subscriber_sends();
  }

  /// Starts the dedicated OS thread for this `EgressReactor` shard.
  void run(bool spawn_background_thread = false) {
    if (is_running.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    if (spawn_background_thread) {
      (void)bind_dedicated_udp_ports();
      thread_active_.store(true, std::memory_order_release);
      worker_thread_ = std::thread(&EgressReactor::thread_loop, this);
    }
    RIVET_LOG_INFO(
        "EgressReactor", "Shard ", reactor_id, " active (UDP ", rtp_port, "/", rtcp_port, ").");
  }

  /// Wake-then-join-then-close clean shutdown.
  void stop() noexcept {
    const bool was_running = is_running.exchange(false, std::memory_order_acq_rel);
    if (was_running || thread_active_.load(std::memory_order_acquire)) {
      poller_.wake();
      if (worker_thread_.joinable()) {
        worker_thread_.join();
      }
      thread_active_.store(false, std::memory_order_release);
    }

    for (auto &kv : viewer_conns_) {
      if (kv.second && kv.second->fd >= 0) {
        poller_.remove(kv.second->fd);
        ::close(kv.second->fd);
        kv.second->fd = -1;
      }
    }
    viewer_conns_.clear();

    {
      std::lock_guard<std::mutex> lk(handoff_mutex_);
      for (auto &cmd : handoff_inbox_) {
        if (cmd.fd >= 0) {
          ::close(cmd.fd);
        }
      }
      handoff_inbox_.clear();
      external_sub_inbox_.clear();
      external_remove_inbox_.clear();
    }

    subscribers_.clear();
    udp_batch_.clear();
    aggregate_queued_bytes = 0;
    sync_sub_count();

    if (owns_udp_sockets_) {
      if (udp_rtcp_socket_ >= 0) {
        poller_.remove(udp_rtcp_socket_);
        ::close(udp_rtcp_socket_);
        udp_rtcp_socket_ = -1;
      }
      if (udp_rtp_socket_ >= 0) {
        ::close(udp_rtp_socket_);
        udp_rtp_socket_ = -1;
      }
      owns_udp_sockets_ = false;
    }
  }

  [[nodiscard]] bool is_active() const noexcept {
    return is_running.load(std::memory_order_acquire);
  }

  [[nodiscard]] size_t subscriber_count() const noexcept {
    if (thread_active_.load(std::memory_order_acquire)) {
      return active_sub_count_atomic_.load(std::memory_order_relaxed);
    }
    return subscribers_.size();
  }

  [[nodiscard]] size_t get_aggregate_queued_bytes() const noexcept {
    return aggregate_queued_bytes;
  }

  [[nodiscard]] uint64_t get_total_dispatched() const noexcept { return total_packets_dispatched; }

  [[nodiscard]] uint64_t get_total_dropped() const noexcept { return total_packets_dropped; }
};

} //< namespace rivet::core
