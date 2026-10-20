#pragma once

#include "rivet/media/DependencyDropPolicy.hpp"
#include "rivet/media/StartupPolicy.hpp"
#include "rivet/memory/PacketRef.hpp"
#include "rivet/proto/rtp/JitterEstimator.hpp"
#include "rivet/proto/rtp/RtpSenderContext.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace rivet::core {

/// Transport protocol chosen during RTSP SETUP negotiation
enum class SubscriberTransport : uint8_t {
  UdpDedicatedPorts, //< UDP port pair (20000+2i, 20001+2i per reactor)
  TcpInterleaved     //< RFC 2326 §10.12 embedded framing ($ + channel + 16-bit len)
};

/// Represents an individual subscriber session managed by an EgressReactor.
/// Enforces single socket execution ownership, per-subscriber 512-packet queue bounds,
/// dependency-aware dropping, and timeline normalization.
class SubscriberSession {
public:
  /// Maximum number of queued outgoing packets before Zone 4 disconnect
  static constexpr size_t MAX_PENDING_SENDS = 512;

private:
  uint64_t            session_id{0};
  int                 socket_fd{-1};
  SubscriberTransport transport{SubscriberTransport::UdpDedicatedPorts};

  std::string client_ip;
  uint16_t    client_rtp_port{0};
  uint16_t    client_rtcp_port{0};
  uint8_t     tcp_channel{0};
  uint8_t     track_id{0}; //< 0 = Video, 1 = Audio

  /// RTP sender context for Mode A (pass-through) vs Mode B (translation)
  proto::rtp::RtpSenderContext sender_context;

  /// Dependency-aware drop evaluator
  media::DependencyDropPolicy drop_policy;

  /// Jitter estimator driven by client RTCP Receiver Reports
  proto::rtp::JitterEstimator jitter_estimator;

  /// Fast-startup or Freshness-first policy
  std::unique_ptr<media::StartupPolicy> startup_policy;

  /// Bounded send queue of pre-retained PacketRef handles
  std::deque<memory::PacketRef> send_queue;

  /// Metrics & Quota Accounting
  size_t   queued_bytes{0};
  uint64_t packets_sent{0};
  uint64_t bytes_sent{0};
  uint64_t packets_dropped{0};

  bool    is_active{true};
  bool    is_quiesced{false};
  int64_t last_active_ns{0};

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
    update_activity_timestamp();
  }

  ~SubscriberSession() = default;

  /// Non-copyable, movable
  SubscriberSession(const SubscriberSession &)                = delete;
  SubscriberSession &operator=(const SubscriberSession &)     = delete;
  SubscriberSession(SubscriberSession &&) noexcept            = default;
  SubscriberSession &operator=(SubscriberSession &&) noexcept = default;

  [[nodiscard]] uint64_t            get_session_id() const noexcept { return session_id; }
  [[nodiscard]] int                 get_socket_fd() const noexcept { return socket_fd; }
  [[nodiscard]] SubscriberTransport get_transport() const noexcept { return transport; }
  [[nodiscard]] const std::string  &get_client_ip() const noexcept { return client_ip; }
  [[nodiscard]] uint16_t            get_client_rtp_port() const noexcept { return client_rtp_port; }
  [[nodiscard]] uint16_t get_client_rtcp_port() const noexcept { return client_rtcp_port; }

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
  void                  set_tcp_channel(uint8_t ch) noexcept { tcp_channel = ch; }

  [[nodiscard]] uint8_t get_track_id() const noexcept { return track_id; }
  void                  set_track_id(uint8_t id) noexcept { track_id = id; }

  [[nodiscard]] const media::StartupPolicy *get_startup_policy() const noexcept {
    return startup_policy.get();
  }
  void set_startup_policy(std::unique_ptr<media::StartupPolicy> policy) noexcept {
    startup_policy = std::move(policy);
  }

  /// Evaluates and queues an outgoing packet for this subscriber
  ///\param packet Incoming pre-retained PacketRef
  ///\param force_keep When true (e.g. during GOP keyframe catch-up burst), bypasses drop policy
  ///\return True if queued for transmission, false if dropped by policy or queue overflow
  bool enqueue_packet(memory::PacketRef packet, bool force_keep = false) {
    if (!is_active || is_quiesced) {
      return false;
    }

    /// Match track ID: only enqueue packets matching this subscriber's track
    if (packet.track_id != track_id) {
      return false;
    }

    if (!force_keep) {
      /// 1. Map packet flags to dependency priority
      media::DependencyPriority prio = media::DependencyPriority::Reference;
      if (packet.is_keyframe()) {
        prio = media::DependencyPriority::Critical;
      } else if (packet.is_rasl() || !packet.is_reference()) {
        prio = media::DependencyPriority::Disposable;
      }

      /// 2. Compute queue percentage and evaluate drop policy
      const uint8_t fill_pct = static_cast<uint8_t>(
          std::min<size_t>(100, (send_queue.size() * 100) / MAX_PENDING_SENDS));
      auto decision = drop_policy.evaluate(prio, fill_pct);

      if (decision == media::DropDecision::Drop) {
        ++packets_dropped;
        return false;
      }

      if (decision == media::DropDecision::Disconnect) {
        ++packets_dropped;
        is_active = false; //< Mark for eviction
        return false;
      }
    }

    /// 3. Normal enqueue: copy adds intrusive ref_count
    queued_bytes += packet.length;
    send_queue.push_back(std::move(packet));
    return true;
  }

  /// Pops the next packet ready for kernel dispatch (e.g. io_uring send)
  ///\return Optional PacketRef, or nullopt if send queue is empty
  [[nodiscard]] std::optional<memory::PacketRef> pop_next_send() {
    if (send_queue.empty()) {
      return std::nullopt;
    }

    memory::PacketRef pkt = std::move(send_queue.front());
    send_queue.pop_front();

    if (queued_bytes >= pkt.length) {
      queued_bytes -= pkt.length;
    } else {
      queued_bytes = 0;
    }

    ++packets_sent;
    bytes_sent += pkt.length;
    update_activity_timestamp();
    return pkt;
  }

  void update_activity_timestamp() noexcept {
    last_active_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  }

  [[nodiscard]] size_t   queue_size() const noexcept { return send_queue.size(); }
  [[nodiscard]] size_t   get_queued_bytes() const noexcept { return queued_bytes; }
  [[nodiscard]] uint64_t get_packets_sent() const noexcept { return packets_sent; }
  [[nodiscard]] uint64_t get_bytes_sent() const noexcept { return bytes_sent; }
  [[nodiscard]] uint64_t get_packets_dropped() const noexcept { return packets_dropped; }
  [[nodiscard]] bool     active() const noexcept { return is_active; }
  [[nodiscard]] int64_t  get_last_active_ns() const noexcept { return last_active_ns; }

  void mark_closed() noexcept {
    is_active    = false;
    queued_bytes = 0;
    send_queue.clear();
  }
  void set_quiesced(bool quiesce) noexcept { is_quiesced = quiesce; }
};

} //< namespace rivet::core
