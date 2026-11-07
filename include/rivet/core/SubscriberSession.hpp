#pragma once

#include "rivet/media/DependencyDropPolicy.hpp"
#include "rivet/media/StartupPolicy.hpp"
#include "rivet/memory/PacketRef.hpp"
#include "rivet/net/SocketUtils.hpp"
#include "rivet/proto/rtp/JitterEstimator.hpp"
#include "rivet/proto/rtp/RtpSenderContext.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <vector>

namespace rivet::core {

/// Transport protocol chosen during RTSP SETUP negotiation
enum class SubscriberTransport : uint8_t {
  UdpDedicatedPorts = 0, //< Dedicated UDP port pair per EgressReactor shard
  TcpInterleaved    = 1  //< RFC 7826 / RFC 2326 embedded framing ($ + channel + 16-bit len)
};

/// Tracks an in-flight non-blocking TCP interleaved frame (`$` + channel + len + RTP) so partial
/// `sendmsg` writes resume cleanly at the exact byte offset without ever corrupting framing.
struct PartialInterleavedWrite {
  std::array<uint8_t, 4>  prefix{};            //< '$', channel, len_hi, len_lo
  std::array<uint8_t, 12> rewritten_rtp_hdr{}; //< Rewritten 12-byte RTP header (never mutates slab)
  memory::PacketRef       pkt{};               //< Retains PooledBuffer until last byte is sent
  size_t                  total_bytes{0};      //< 4 + pkt.length (or 4 + raw_rtcp.size())
  size_t                  sent_bytes{0};       //< Bytes accepted by kernel so far
  std::vector<uint8_t>    owned_control_rtcp;  //< Used only for server-originated RTCP SR frames
  bool                    has_rewritten_hdr{false};
  bool                    active{false};

  void clear() noexcept {
    pkt               = memory::PacketRef{};
    total_bytes       = 0;
    sent_bytes        = 0;
    has_rewritten_hdr = false;
    active            = false;
    owned_control_rtcp.clear();
  }
};

/// Pre-allocated power-of-two circular buffer replacing `std::deque<PacketRef>` so steady-state
/// `push_back` / `pop_front` performs ZERO heap allocations.
class PacketSendRing {
  std::vector<memory::PacketRef> slots_;
  size_t                         head_{0};
  size_t                         count_{0};

  void grow() {
    const size_t                   old_cap = slots_.size();
    const size_t                   new_cap = (old_cap == 0U) ? 1024U : (old_cap * 2U);
    std::vector<memory::PacketRef> next(new_cap);
    for (size_t i = 0; i < count_; ++i) {
      next[i] = std::move(slots_[(head_ + i) & (old_cap - 1U)]);
    }
    slots_ = std::move(next);
    head_  = 0;
  }

public:
  explicit PacketSendRing(size_t initial_pow2_cap = 1024U)
      : slots_(initial_pow2_cap) {}

  void push_back(memory::PacketRef pkt) {
    if (count_ == slots_.size()) {
      grow();
    }
    const size_t mask               = slots_.size() - 1U;
    slots_[(head_ + count_) & mask] = std::move(pkt);
    ++count_;
  }

  [[nodiscard]] memory::PacketRef pop_front() noexcept {
    const size_t      mask = slots_.size() - 1U;
    memory::PacketRef out  = std::move(slots_[head_]);
    slots_[head_]          = memory::PacketRef{};
    head_                  = (head_ + 1U) & mask;
    --count_;
    return out;
  }

  void clear() noexcept {
    if (count_ > 0U && !slots_.empty()) {
      const size_t mask = slots_.size() - 1U;
      for (size_t i = 0; i < count_; ++i) {
        slots_[(head_ + i) & mask] = memory::PacketRef{};
      }
    }
    head_  = 0;
    count_ = 0;
  }

  [[nodiscard]] bool   empty() const noexcept { return count_ == 0U; }
  [[nodiscard]] size_t size() const noexcept { return count_; }
};

/// Represents an individual subscriber track/session managed exclusively by one EgressReactor.
class SubscriberSession {
public:
  static constexpr size_t   MAX_PENDING_SENDS       = 512;
  static constexpr size_t   DEFAULT_MAX_QUEUE_BYTES = 512 * 1024;       //< 512 KiB
  static constexpr uint64_t TCP_STALL_TIMEOUT_NS    = 2'000'000'000ULL; //< 2 seconds (L4)
  static constexpr uint64_t CLIENT_IDLE_TIMEOUT_NS  = 60'000'000'000ULL;

private:
  uint64_t            session_id{0};
  int                 socket_fd{-1};
  SubscriberTransport transport{SubscriberTransport::UdpDedicatedPorts};

  std::string client_ip;
  uint16_t    client_rtp_port{0};
  uint16_t    client_rtcp_port{0};
  sockaddr_in rtp_dest_addr_{};
  sockaddr_in rtcp_dest_addr_{};
  bool        has_rtp_dest_{false};
  bool        has_rtcp_dest_{false};

  uint8_t tcp_channel{0};
  uint8_t rtcp_tcp_channel{1};
  uint8_t track_id{0};
  bool    is_video_track_{true};
  uint8_t highest_tid_{0};
  bool    allow_open_irap_{true};

  proto::rtp::RtpSenderContext          sender_context;
  media::DependencyDropPolicy           drop_policy;
  media::AuGate                         au_gate;
  proto::rtp::JitterEstimator           jitter_estimator;
  std::unique_ptr<media::StartupPolicy> startup_policy;

  PacketSendRing          send_queue;
  PartialInterleavedWrite partial_write_;

  size_t   max_queue_bytes_{DEFAULT_MAX_QUEUE_BYTES};
  size_t   queued_bytes{0};
  uint64_t packets_sent{0};
  uint64_t bytes_sent{0};
  uint64_t packets_dropped{0};
  uint64_t aus_dropped_{0};
  uint64_t rasl_dropped_{0};
  uint64_t sublayer_dropped_{0};

  /// Paced historical RAP catch-up cursor state
  uint64_t burst_cursor_pos_{0};
  uint64_t burst_end_pos_{0};
  uint64_t burst_tokens_bytes_{0};
  uint64_t last_burst_refill_ns_{0};
  uint64_t burst_rate_bytes_per_sec_{16ULL * 1024ULL * 1024ULL}; //< 16 MiB/s default 4x pace
  bool     in_burst_{false};
  uint32_t advertised_generation_{0};

  bool     is_active{true};
  bool     is_quiesced{false};
  bool     is_playing_{true};
  bool     needs_pli_{false};
  uint64_t last_client_activity_ns_{0};
  uint64_t stall_started_ns_{0};

  [[nodiscard]] static uint64_t steady_now_ns() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }

  void rebuild_dest_addrs() noexcept {
    has_rtp_dest_ =
        (client_rtp_port > 0) && net::make_ipv4_addr(client_ip, client_rtp_port, rtp_dest_addr_);
    has_rtcp_dest_ =
        (client_rtcp_port > 0) && net::make_ipv4_addr(client_ip, client_rtcp_port, rtcp_dest_addr_);
  }

public:
  SubscriberSession(uint64_t id,
      int                    fd,
      SubscriberTransport    trans,
      std::string            ip,
      uint16_t               rtp_port,
      uint16_t               rtcp_port,
      uint32_t               ssrc,
      proto::rtp::SenderMode mode = proto::rtp::SenderMode::TransparentPassThrough)
      : session_id(id)
      , socket_fd(fd)
      , transport(trans)
      , client_ip(std::move(ip))
      , client_rtp_port(rtp_port)
      , client_rtcp_port(rtcp_port)
      , sender_context(mode, ssrc)
      , startup_policy(std::make_unique<media::FastPictureFirstPolicy>()) {
    rebuild_dest_addrs();
    record_client_activity(steady_now_ns());
  }

  ~SubscriberSession() = default;

  SubscriberSession(const SubscriberSession &)                = delete;
  SubscriberSession &operator=(const SubscriberSession &)     = delete;
  SubscriberSession(SubscriberSession &&) noexcept            = default;
  SubscriberSession &operator=(SubscriberSession &&) noexcept = default;

  [[nodiscard]] uint64_t            get_session_id() const noexcept { return session_id; }
  [[nodiscard]] int                 get_socket_fd() const noexcept { return socket_fd; }
  void                              set_socket_fd(int fd) noexcept { socket_fd = fd; }
  [[nodiscard]] SubscriberTransport get_transport() const noexcept { return transport; }
  [[nodiscard]] const std::string  &get_client_ip() const noexcept { return client_ip; }
  [[nodiscard]] uint16_t            get_client_rtp_port() const noexcept { return client_rtp_port; }
  [[nodiscard]] uint16_t get_client_rtcp_port() const noexcept { return client_rtcp_port; }

  void set_udp_ports(uint16_t rtp_p, uint16_t rtcp_p) noexcept {
    client_rtp_port  = rtp_p;
    client_rtcp_port = rtcp_p;
    rebuild_dest_addrs();
  }

  [[nodiscard]] bool               has_rtp_dest() const noexcept { return has_rtp_dest_; }
  [[nodiscard]] bool               has_rtcp_dest() const noexcept { return has_rtcp_dest_; }
  [[nodiscard]] const sockaddr_in &rtp_dest_addr() const noexcept { return rtp_dest_addr_; }
  [[nodiscard]] const sockaddr_in &rtcp_dest_addr() const noexcept { return rtcp_dest_addr_; }

  [[nodiscard]] proto::rtp::RtpSenderContext &get_sender_context() noexcept {
    return sender_context;
  }
  [[nodiscard]] const proto::rtp::RtpSenderContext &get_sender_context() const noexcept {
    return sender_context;
  }
  [[nodiscard]] proto::rtp::JitterEstimator &get_jitter_estimator() noexcept {
    return jitter_estimator;
  }

  [[nodiscard]] uint8_t get_tcp_channel() const noexcept { return tcp_channel; }
  void                  set_tcp_channel(uint8_t ch) noexcept {
    tcp_channel      = ch;
    rtcp_tcp_channel = static_cast<uint8_t>(ch + 1);
  }
  [[nodiscard]] uint8_t get_rtcp_tcp_channel() const noexcept { return rtcp_tcp_channel; }
  void                  set_rtcp_tcp_channel(uint8_t ch) noexcept { rtcp_tcp_channel = ch; }

  [[nodiscard]] uint8_t get_track_id() const noexcept { return track_id; }
  void                  set_track_id(uint8_t id, bool is_video = true) noexcept {
    track_id        = id;
    is_video_track_ = is_video;
  }
  [[nodiscard]] bool is_video_track() const noexcept { return is_video_track_; }

  void set_stream_params(uint8_t highest_tid,
      bool                       allow_open_irap,
      size_t                     max_queue_bytes = DEFAULT_MAX_QUEUE_BYTES) noexcept {
    highest_tid_     = highest_tid;
    allow_open_irap_ = allow_open_irap;
    if (max_queue_bytes > 0) {
      max_queue_bytes_ = max_queue_bytes;
    }
    au_gate.tid_ceiling = std::min<uint8_t>(highest_tid, 7);
  }

  void set_advertised_generation(uint32_t gen) noexcept { advertised_generation_ = gen; }
  [[nodiscard]] uint32_t get_advertised_generation() const noexcept {
    return advertised_generation_;
  }

  [[nodiscard]] bool is_playing() const noexcept { return is_playing_; }
  void               set_playing(bool playing) noexcept { is_playing_ = playing; }

  [[nodiscard]] media::AuGate       &get_au_gate() noexcept { return au_gate; }
  [[nodiscard]] const media::AuGate &get_au_gate() const noexcept { return au_gate; }

  [[nodiscard]] const media::StartupPolicy *get_startup_policy() const noexcept {
    return startup_policy.get();
  }
  void set_startup_policy(std::unique_ptr<media::StartupPolicy> policy) noexcept {
    startup_policy = std::move(policy);
  }

  /// Arms paced RAP cursor replay from a computed `StartupPlan`.
  void arm_startup_plan(const media::StartupPlan &plan,
      uint64_t                                    now_ns,
      uint64_t pace_bytes_per_sec = 16ULL * 1024ULL * 1024ULL) noexcept {
    au_gate.reset(std::min<uint8_t>(highest_tid_, 7), plan.wait_irap, plan.rasl_gate);
    needs_pli_ = plan.request_pli;
    if (plan.has_rap && plan.start_pos < plan.live_pos) {
      burst_cursor_pos_ = plan.start_pos;
      burst_end_pos_    = plan.live_pos;
      burst_rate_bytes_per_sec_ =
          pace_bytes_per_sec > 0 ? pace_bytes_per_sec : (16ULL * 1024ULL * 1024ULL);
      /// Grant initial 128 KiB burst quantum so first keyframe slices start immediately
      burst_tokens_bytes_   = 128 * 1024;
      last_burst_refill_ns_ = now_ns;
      in_burst_             = true;
    } else {
      burst_cursor_pos_ = plan.live_pos;
      burst_end_pos_    = plan.live_pos;
      in_burst_         = false;
    }
  }

  [[nodiscard]] bool     in_startup_burst() const noexcept { return in_burst_; }
  [[nodiscard]] uint64_t burst_cursor_pos() const noexcept { return burst_cursor_pos_; }
  [[nodiscard]] uint64_t burst_end_pos() const noexcept { return burst_end_pos_; }
  void                   advance_burst_cursor(uint64_t next_pos) noexcept {
    burst_cursor_pos_ = next_pos;
    if (burst_cursor_pos_ >= burst_end_pos_) {
      in_burst_ = false;
    }
  }
  void finish_startup_burst() noexcept { in_burst_ = false; }

  uint64_t refill_burst_tokens(uint64_t now_ns) noexcept {
    if (now_ns > last_burst_refill_ns_) {
      const uint64_t elapsed_ns = now_ns - last_burst_refill_ns_;
      const uint64_t add        = (elapsed_ns * burst_rate_bytes_per_sec_) / 1'000'000'000ULL;
      if (add > 0) {
        burst_tokens_bytes_   = std::min<uint64_t>(burst_tokens_bytes_ + add, 512 * 1024);
        last_burst_refill_ns_ = now_ns;
      }
    }
    return burst_tokens_bytes_;
  }

  void consume_burst_tokens(size_t bytes) noexcept {
    burst_tokens_bytes_ = (burst_tokens_bytes_ > bytes) ? (burst_tokens_bytes_ - bytes) : 0;
  }

  [[nodiscard]] bool consume_pli_request() noexcept {
    const bool r = needs_pli_;
    needs_pli_   = false;
    return r;
  }

  /// Effective byte queue ceiling: `max(max_queue_bytes_, 2 * max_recent_irap_bytes)`.
  [[nodiscard]] size_t effective_max_queue_bytes(
      uint32_t max_recent_irap_bytes = 0) const noexcept {
    const size_t irap_floor = static_cast<size_t>(max_recent_irap_bytes) * 2u;
    return std::max(max_queue_bytes_, irap_floor);
  }

  [[nodiscard]] uint8_t compute_fill_pct(uint32_t max_recent_irap_bytes = 0) const noexcept {
    const size_t byte_cap = effective_max_queue_bytes(max_recent_irap_bytes);
    const size_t byte_pct = (byte_cap > 0) ? ((queued_bytes * 100u) / byte_cap) : 0;
    const size_t slot_pct = (send_queue.size() * 100u) / MAX_PENDING_SENDS;
    const size_t pct      = std::max(byte_pct, slot_pct);
    return static_cast<uint8_t>(std::min<size_t>(100u, pct));
  }

  /// Evaluates whole-AU admission (`AuGate`) and queues packet for transmission.
  bool enqueue_packet(memory::PacketRef packet,
      bool                              force_keep            = false,
      uint64_t                          now_ns                = 0,
      uint32_t                          max_recent_irap_bytes = 0) {
    if (!is_active || is_quiesced || !is_playing_) {
      return false;
    }

    /// Match logical track index (`packet.track()` unpacks low 3 bits of `track_id` byte!)
    if (packet.track() != track_id) {
      return false;
    }

    if (!force_keep) {
      const uint8_t fill_pct = compute_fill_pct(max_recent_irap_bytes);
      const auto    admit    = drop_policy.admit(au_gate,
          packet,
          fill_pct,
          highest_tid_,
          now_ns != 0 ? now_ns : steady_now_ns(),
          allow_open_irap_,
          is_video_track_);

      if (admit.started_au_drop) {
        ++aus_dropped_;
      }
      if (admit.dropped_rasl) {
        ++rasl_dropped_;
      }
      if (admit.dropped_sublayer) {
        ++sublayer_dropped_;
      }
      if (admit.entered_wait_irap) {
        needs_pli_ = true;
      }

      if (admit.decision == media::DropDecision::Drop) {
        ++packets_dropped;
        return false;
      }
      if (admit.decision == media::DropDecision::Disconnect) {
        ++packets_dropped;
        is_active = false;
        return false;
      }
    }

    queued_bytes += packet.length;
    send_queue.push_back(std::move(packet));
    return true;
  }

  /// Pops the next queued packet for dispatch (does NOT fake client liveness!).
  [[nodiscard]] std::optional<memory::PacketRef> pop_next_send() {
    if (send_queue.empty()) {
      return std::nullopt;
    }

    memory::PacketRef pkt = send_queue.pop_front();

    queued_bytes = (queued_bytes >= pkt.length) ? (queued_bytes - pkt.length) : 0;
    ++packets_sent;
    bytes_sent += pkt.length;
    return pkt;
  }

  void record_direct_send(size_t byte_len) noexcept {
    ++packets_sent;
    bytes_sent += byte_len;
  }

  void record_drop() noexcept { ++packets_dropped; }

  /// Updates client activity strictly upon inbound client RTSP/RTCP traffic.
  void record_client_activity(uint64_t now_ns) noexcept { last_client_activity_ns_ = now_ns; }

  void update_activity_timestamp() noexcept { record_client_activity(steady_now_ns()); }

  /// Tracks sustained TCP `EAGAIN` stall duration (disconnect after 2 s zero progress).
  void notify_send_blocked(uint64_t now_ns) noexcept {
    if (stall_started_ns_ == 0) {
      stall_started_ns_ = now_ns;
    }
  }

  void notify_send_progress() noexcept { stall_started_ns_ = 0; }

  [[nodiscard]] bool is_tcp_stalled(uint64_t now_ns) const noexcept {
    return transport == SubscriberTransport::TcpInterleaved && stall_started_ns_ != 0 &&
        now_ns > stall_started_ns_ && (now_ns - stall_started_ns_) >= TCP_STALL_TIMEOUT_NS;
  }

  [[nodiscard]] bool is_client_timed_out(uint64_t now_ns,
      uint64_t timeout_ns = CLIENT_IDLE_TIMEOUT_NS) const noexcept {
    return last_client_activity_ns_ != 0 && now_ns > last_client_activity_ns_ &&
        (now_ns - last_client_activity_ns_) >= timeout_ns;
  }

  [[nodiscard]] PartialInterleavedWrite       &partial_write() noexcept { return partial_write_; }
  [[nodiscard]] const PartialInterleavedWrite &partial_write() const noexcept {
    return partial_write_;
  }
  [[nodiscard]] bool has_pending_work() const noexcept {
    return partial_write_.active || !send_queue.empty() || in_burst_;
  }

  [[nodiscard]] size_t   queue_size() const noexcept { return send_queue.size(); }
  [[nodiscard]] size_t   get_queued_bytes() const noexcept { return queued_bytes; }
  [[nodiscard]] uint64_t get_packets_sent() const noexcept { return packets_sent; }
  [[nodiscard]] uint64_t get_bytes_sent() const noexcept { return bytes_sent; }
  [[nodiscard]] uint64_t get_packets_dropped() const noexcept { return packets_dropped; }
  [[nodiscard]] uint64_t get_aus_dropped() const noexcept { return aus_dropped_; }
  [[nodiscard]] uint64_t get_rasl_dropped() const noexcept { return rasl_dropped_; }
  [[nodiscard]] uint64_t get_sublayer_dropped() const noexcept { return sublayer_dropped_; }
  [[nodiscard]] bool     active() const noexcept { return is_active; }
  [[nodiscard]] bool     quiesced() const noexcept { return is_quiesced; }
  [[nodiscard]] int64_t  get_last_active_ns() const noexcept {
    return static_cast<int64_t>(last_client_activity_ns_);
  }

  void mark_closed() noexcept {
    is_active    = false;
    in_burst_    = false;
    queued_bytes = 0;
    partial_write_.clear();
    send_queue.clear();
  }

  void set_quiesced(bool quiesce) noexcept { is_quiesced = quiesce; }
};

} //< namespace rivet::core
