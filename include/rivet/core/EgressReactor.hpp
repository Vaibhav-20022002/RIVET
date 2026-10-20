#pragma once

#include "rivet/core/PacketRouter.hpp"
#include "rivet/core/SubscriberSession.hpp"
#include "rivet/media/RetainedPacketLog.hpp"
#include "rivet/memory/PacketRef.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>

namespace rivet::core {

/// EgressReactor manages a disjoint partition of subscriber sockets on a single CPU core.
/// Enforces single socket execution ownership, pulls pre-retained PacketRefs from its
/// dedicated SPSC queue, evaluates per-subscriber backpressure, and enforces the 16 MB reactor quota.
class EgressReactor {
public:
  /// Hard reactor-wide memory quota accounting for all queued subscriber data
  static constexpr size_t MAX_REACTOR_EGRESS_BYTES = 16 * 1024 * 1024; //< 16 MB

  /// Default dedicated UDP port pair base for this reactor shard (20000 + 2i, 20001 + 2i)
  static constexpr uint16_t BASE_RTP_PORT  = 20000;
  static constexpr uint16_t BASE_RTCP_PORT = 20001;

private:
  uint32_t reactor_id{0};
  uint16_t rtp_port{0};
  uint16_t rtcp_port{0};

  /// SPSC queue channel connected to the IngestReactor via PacketRouter
  PacketRouter::PacketQueue *spsc_channel{nullptr};

  /// Disjoint subscriber set owned strictly by this reactor thread
  std::unordered_map<uint64_t, std::unique_ptr<SubscriberSession>> subscribers;

  std::atomic<bool> is_running{false};

  /// Aggregated metrics across this reactor shard
  size_t   aggregate_queued_bytes{0};
  uint64_t total_packets_dispatched{0};
  uint64_t total_packets_dropped{0};

  int udp_socket{-1};
  int udp_video_socket{-1};
  int udp_audio_socket{-1};

public:
  explicit EgressReactor(uint32_t id,
      PacketRouter::PacketQueue  *channel   = nullptr,
      uint16_t                    base_rtp  = BASE_RTP_PORT,
      uint16_t                    base_rtcp = BASE_RTCP_PORT)
      : reactor_id(id)
      , rtp_port(static_cast<uint16_t>(base_rtp + (2 * id)))
      , rtcp_port(static_cast<uint16_t>(base_rtcp + (2 * id)))
      , spsc_channel(channel) {}

  ~EgressReactor() { stop(); }

  /// Non-copyable, non-movable to guarantee single socket ownership and thread safety
  EgressReactor(const EgressReactor &)            = delete;
  EgressReactor &operator=(const EgressReactor &) = delete;
  EgressReactor(EgressReactor &&)                 = delete;
  EgressReactor &operator=(EgressReactor &&)      = delete;

  [[nodiscard]] uint32_t get_reactor_id() const noexcept { return reactor_id; }
  [[nodiscard]] uint16_t get_rtp_port() const noexcept { return rtp_port; }
  [[nodiscard]] uint16_t get_rtcp_port() const noexcept { return rtcp_port; }

  void set_spsc_channel(PacketRouter::PacketQueue *channel) noexcept { spsc_channel = channel; }

  void set_udp_sockets(int v_sock, int a_sock) noexcept {
    udp_video_socket = v_sock;
    udp_audio_socket = a_sock;
  }

  /// Registers a new subscriber session on this reactor shard
  ///\param session Owning unique_ptr to the SubscriberSession
  ///\param packet_log Optional pointer to RetainedPacketLog for fast catch-up GOP replay
  void add_subscriber(std::unique_ptr<SubscriberSession> session,
      const media::RetainedPacketLog                    *packet_log = nullptr) {
    if (!session) {
      return;
    }

    /// Startup Policy execution: Burst historical keyframe packets if FastPictureFirst policy is
    /// active, or start cleanly at the live edge if FreshnessFirst policy is active.
    if (packet_log && session->get_track_id() == 0 && session->get_startup_policy()) {
      auto burst = session->get_startup_policy()->start(*packet_log);
      for (auto &pkt : burst) {
        session->enqueue_packet(std::move(pkt), true);
      }
    }

    uint64_t id     = session->get_session_id();
    subscribers[id] = std::move(session);
    flush_subscriber_sends();
  }

  /// Removes a subscriber session (e.g. on client TEARDOWN or socket close)
  void remove_subscriber(uint64_t session_id) {
    auto it = subscribers.find(session_id);
    if (it != subscribers.end()) {
      it->second->mark_closed();
      subscribers.erase(it);
    }
  }

  /// Consumes all available pre-retained PacketRefs from the SPSC channel and dispatches them
  /// across active subscriber queues without lock contention
  size_t poll_and_dispatch() {
    if (!spsc_channel) {
      return 0;
    }

    size_t            dispatched = 0;
    memory::PacketRef packet;

    /// Drain incoming SPSC batch
    while (spsc_channel->try_pop(packet)) {
      ++dispatched;

      /// Broadcast to all owned subscribers
      for (auto it = subscribers.begin(); it != subscribers.end();) {
        auto &session = it->second;

        if (!session->active()) {
          /// Cleanly evict dead or timed-out subscriber
          it = subscribers.erase(it);
          continue;
        }

        /// Check 16 MB reactor quota before enqueuing
        if (aggregate_queued_bytes >= MAX_REACTOR_EGRESS_BYTES) {
          ++total_packets_dropped;
          ++it;
          continue;
        }

        /// Enqueuing calls PacketRef copy constructor -> increments intrusive buffer ref_count
        if (session->enqueue_packet(packet)) {
          aggregate_queued_bytes += packet.length;
          ++total_packets_dispatched;
        } else {
          ++total_packets_dropped;
        }

        ++it;
      }
    }

    return dispatched;
  }

  /// Flushes packets from subscriber send queues towards network sockets
  ///\return Total number of packets transferred to kernel/mock socket buffers
  size_t flush_subscriber_sends() {
    size_t sends_flushed = 0;

    for (auto &pair : subscribers) {
      auto &session = pair.second;
      while (auto pkt_opt = session->pop_next_send()) {
        ++sends_flushed;
        if (aggregate_queued_bytes >= pkt_opt->length) {
          aggregate_queued_bytes -= pkt_opt->length;
        } else {
          aggregate_queued_bytes = 0;
        }

        if (session->get_transport() == SubscriberTransport::UdpDedicatedPorts) {
          int out_sock = (session->get_track_id() == 1) ? udp_audio_socket : udp_video_socket;
          if (out_sock < 0) {
            if (udp_socket < 0) {
              udp_socket = ::socket(AF_INET, SOCK_DGRAM, 0);
            }
            out_sock = udp_socket;
          }
          if (out_sock >= 0 && session->get_client_rtp_port() > 0) {
            sockaddr_in dest{};
#if defined(__APPLE__)
            dest.sin_len = sizeof(dest);
#endif
            dest.sin_family = AF_INET;
            dest.sin_port   = htons(session->get_client_rtp_port());
            const char *ip_str =
                session->get_client_ip().empty() ? "127.0.0.1" : session->get_client_ip().c_str();
            ::inet_pton(AF_INET, ip_str, &dest.sin_addr);

            ::sendto(out_sock,
                pkt_opt->data(),
                pkt_opt->length,
                0,
                reinterpret_cast<const struct sockaddr *>(&dest),
                sizeof(dest));
          }
        } else if (session->get_transport() == SubscriberTransport::TcpInterleaved) {
          int fd = session->get_socket_fd();
          if (fd >= 0 && pkt_opt->length > 0 && pkt_opt->length <= 2048) {
            uint8_t send_buf[2048 + 4];
            send_buf[0] = '$';
            send_buf[1] = session->get_tcp_channel();
            send_buf[2] = static_cast<uint8_t>((pkt_opt->length >> 8) & 0xFF);
            send_buf[3] = static_cast<uint8_t>(pkt_opt->length & 0xFF);
            std::memcpy(send_buf + 4, pkt_opt->data(), pkt_opt->length);

            const size_t total_len  = 4 + pkt_opt->length;
            size_t       total_sent = 0;
            while (total_sent < total_len) {
              auto res = ::send(fd, send_buf + total_sent, total_len - total_sent, 0);
              if (res <= 0) {
                break;
              }
              total_sent += static_cast<size_t>(res);
            }
          }
        }
      }
    }

    return sends_flushed;
  }

  /// Runs one tick of the reactor event loop
  void step() {
    poll_and_dispatch();
    flush_subscriber_sends();
  }

  /// Starts the Egress Reactor event loop
  void run() {
    is_running.store(true, std::memory_order_release);
    std::cout << "[EgressReactor " << reactor_id << "] Running event loop on dedicated port pair ("
              << rtp_port << "/" << rtcp_port << ")...\n";
  }

  /// Signals the event loop to stop
  void stop() noexcept {
    if (is_running.exchange(false, std::memory_order_release)) {
      subscribers.clear();
      if (udp_socket >= 0) {
        ::close(udp_socket);
        udp_socket = -1;
      }
    }
  }

  [[nodiscard]] bool is_active() const noexcept {
    return is_running.load(std::memory_order_acquire);
  }

  [[nodiscard]] size_t subscriber_count() const noexcept { return subscribers.size(); }

  [[nodiscard]] size_t get_aggregate_queued_bytes() const noexcept {
    return aggregate_queued_bytes;
  }

  [[nodiscard]] uint64_t get_total_dispatched() const noexcept { return total_packets_dispatched; }

  [[nodiscard]] uint64_t get_total_dropped() const noexcept { return total_packets_dropped; }
};

} //< namespace rivet::core
