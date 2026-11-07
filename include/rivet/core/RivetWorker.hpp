#pragma once

#include "rivet/config/RivetConfig.hpp"
#include "rivet/core/EgressReactor.hpp"
#include "rivet/core/IngestReactor.hpp"
#include "rivet/core/Metrics.hpp"
#include "rivet/core/PacketRouter.hpp"
#include "rivet/core/Probes.hpp"
#include "rivet/core/SubscriberSession.hpp"
#include "rivet/core/WorkerIpc.hpp"
#include "rivet/core/Zones.hpp"
#include "rivet/memory/NumaArena.hpp"
#include "rivet/memory/PacketRef.hpp"
#include "rivet/net/EventPoller.hpp"
#include "rivet/net/SocketUtils.hpp"
#include "rivet/proto/rtp/RtcpReportGenerator.hpp"
#include "rivet/proto/rtp/RtpPacketClassifier.hpp"
#include "rivet/proto/rtsp/RtspStreamFramer.hpp"
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace rivet::core {

/// Entrypoint for `--worker` child OS processes (implemented in `src/worker_main.cpp`).
int run_worker_process(int argc, char **argv);

/// RivetWorker is the isolated per-stream media engine container.
/// Coordinates Thread 0 (`IngestReactor` + UDS IPC + publisher TCP/UDP I/O) with Threads `1..S`
/// (`EgressReactor` shards) over lock-free SPSC queues and `PacketRing`.
class RivetWorker {
  /// CRITICAL DESTRUCTION ORDER INVARIANT:
  /// `arena_` MUST be the very first member so all reactors, queues, and `PacketRing` slots
  /// release every `PacketRef` BEFORE `~MultiClassArena()` verifies `active_allocations == 0`!
  memory::MultiClassArena arena_;

  std::string         stream_id;
  config::RivetConfig config_;
  IngestReactor       ingest;
  PacketRouter        router;

  std::vector<std::unique_ptr<EgressReactor>> egress_reactors;

  net::EventPoller ingest_poller_;
  std::thread      ingest_thread_;
  int              uds_fd_{-1};
  bool             owns_uds_fd_{false};

  /// Active publisher TCP state (Thread 0)
  int                         pub_fd_{-1};
  std::string                 pub_ip_;
  in_addr_t                   latched_pub_addr_{0};
  bool                        has_latched_pub_addr_{false};
  std::array<uint16_t, 8>     latched_pub_rtp_ports_{};
  proto::rtsp::RtspReadBuffer pub_read_buf_{proto::rtsp::RtspReadBuffer::DEFAULT_CAPACITY};
  std::string                 pub_outbox_;
  size_t                      pub_outbox_sent_{0};

  /// Direct-to-slab TCP `$` interleaved payload reader state
  memory::PooledBuffer *pub_direct_slab_{nullptr};
  uint32_t              pub_direct_total_{0};
  uint32_t              pub_direct_offset_{0};
  uint8_t               pub_direct_channel_{0};

  /// Dedicated Worker UDP ingest sockets (Video track 0 + Audio track 1)
  int      ingest_video_rtp_fd_{-1};
  int      ingest_video_rtcp_fd_{-1};
  uint16_t bound_video_rtp_port_{0};
  uint16_t bound_video_rtcp_port_{0};
  int      ingest_audio_rtp_fd_{-1};
  int      ingest_audio_rtcp_fd_{-1};
  uint16_t bound_audio_rtp_port_{0};
  uint16_t bound_audio_rtcp_port_{0};

  /// Rate-limited upstream RTCP PLI trigger (`>= 250 ms` coalescing)
  std::atomic<bool> pli_requested_{false};
  uint64_t          last_pli_sent_ns_{0};

  std::atomic<bool> is_running{false};
  std::atomic<bool> loop_active_{false};
  std::atomic<bool> quiescing_{false};
  uint32_t          egress_shard_count{1};
  bool              inline_egress_mode_{false};

  static constexpr uint64_t kTagUds       = 1;
  static constexpr uint64_t kTagPubTcp    = 2;
  static constexpr uint64_t kTagVideoRtp  = 3;
  static constexpr uint64_t kTagVideoRtcp = 4;
  static constexpr uint64_t kTagAudioRtp  = 5;
  static constexpr uint64_t kTagAudioRtcp = 6;

  void init_shards(uint16_t base_rtp, uint16_t base_rtcp) {
    inline_egress_mode_  = (config_.egress_shards == 0);
    const uint32_t count = (egress_shard_count > 0) ? egress_shard_count : 1U;
    egress_reactors.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
      /// Reserve ports `base_rtp + 0..3` for worker UDP ingest when `base_rtp != 0`,
      /// or use `base_rtp + 4 + 2*i` for egress shards so ports never collide on localhost.
      const uint16_t shard_base_rtp =
          (base_rtp == 0) ? 0 : static_cast<uint16_t>(base_rtp + 4U + (2U * i));
      const uint16_t shard_base_rtcp =
          (base_rtcp == 0) ? 0 : static_cast<uint16_t>(shard_base_rtp + 1U);

      const size_t chan_id  = router.register_egress_channel();
      auto        *chan_ptr = router.get_channel(chan_id);
      auto reactor = std::make_unique<EgressReactor>(i, chan_ptr, shard_base_rtp, shard_base_rtcp);

      reactor->configure(config_,
          stream_id,
          &ingest.metrics(),
          {
              [this]() { return ingest.get_stream_descriptor_snapshot(); },
              [this]() { return ingest.get_config_tracker().current_snapshot(); },
              [this]() { return &ingest.get_packet_log(); },
              [this]() { request_upstream_pli(); },
          });
      egress_reactors.push_back(std::move(reactor));
    }
  }

  void release_direct_slab() noexcept {
    if (pub_direct_slab_) {
      pub_direct_slab_->release();
      pub_direct_slab_ = nullptr;
    }
    pub_direct_total_   = 0;
    pub_direct_offset_  = 0;
    pub_direct_channel_ = 0;
  }

  /// Allocates an ingest slab, reclaiming oldest historical GOP packets from `PacketRing` on
  /// Thread 0 if historical retention is holding the last free slabs in that size class.
  [[nodiscard]] memory::PooledBuffer *allocate_ingest_slab(size_t needed_bytes) noexcept {
    if (auto *slab = arena_.allocate_for_size(needed_bytes)) {
      return slab;
    }
    auto &ring = ingest.get_packet_log().ring();
    while (ring.evict_one_oldest()) {
      if (auto *slab = arena_.allocate_for_size(needed_bytes)) {
        return slab;
      }
    }
    return nullptr;
  }

  void append_pub_outbox(std::string_view data) {
    if (pub_outbox_sent_ == pub_outbox_.size()) {
      pub_outbox_.clear();
      pub_outbox_sent_ = 0;
    }
    pub_outbox_.append(data);
  }

  void flush_pub_outbox() noexcept {
    if (pub_fd_ < 0) {
      pub_outbox_.clear();
      pub_outbox_sent_ = 0;
      return;
    }
    while (pub_outbox_sent_ < pub_outbox_.size()) {
      const ssize_t n = ::send(pub_fd_,
          pub_outbox_.data() + pub_outbox_sent_,
          pub_outbox_.size() - pub_outbox_sent_,
          net::nonblocking_send_flags());
      if (n > 0) {
        pub_outbox_sent_ += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        (void)ingest_poller_.add_or_mod(pub_fd_, net::kPollRead | net::kPollWrite, kTagPubTcp);
        return;
      }
      close_publisher();
      return;
    }
    pub_outbox_.clear();
    pub_outbox_sent_ = 0;
    if (pub_fd_ >= 0) {
      (void)ingest_poller_.add_or_mod(pub_fd_, net::kPollRead, kTagPubTcp);
    }
  }

  void notify_controller_publisher_state() noexcept {
    if (uds_fd_ < 0) {
      return;
    }
    ipc::PublisherStatePayload st{};
    const auto                 desc = ingest.get_stream_descriptor_snapshot();
    st.has_publisher                = (pub_fd_ >= 0 && desc.has_value()) ? 1U : 0U;
    st.is_recording      = (ingest.get_rtsp_state() == proto::rtsp::RtspState::Recording) ? 1U : 0U;
    st.track_count       = desc.has_value() ? static_cast<uint8_t>(desc->tracks.size()) : 0U;
    st.config_generation = ingest.get_config_tracker().current_generation();
    (void)ipc::send_ipc_message(uds_fd_, ipc::IpcMsgType::PublisherStateChanged, &st, sizeof(st));
  }

  void close_publisher() noexcept {
    release_direct_slab();
    if (pub_fd_ >= 0) {
      ingest_poller_.remove(pub_fd_);
      ::close(pub_fd_);
      pub_fd_ = -1;
    }
    pub_read_buf_.clear();
    pub_outbox_.clear();
    pub_outbox_sent_      = 0;
    has_latched_pub_addr_ = false;
    latched_pub_rtp_ports_.fill(0);
    notify_controller_publisher_state();
  }

  bool bind_ingest_udp_sockets() noexcept {
    if (ingest_video_rtp_fd_ < 0) {
      const uint16_t desired_v = config_.base_rtp_port;
      if (!net::bind_udp_port_pair(desired_v,
              ingest_video_rtp_fd_,
              ingest_video_rtcp_fd_,
              bound_video_rtp_port_,
              bound_video_rtcp_port_,
              true)) {
        return false;
      }
      (void)ingest_poller_.add_or_mod(ingest_video_rtp_fd_, net::kPollRead, kTagVideoRtp);
      (void)ingest_poller_.add_or_mod(ingest_video_rtcp_fd_, net::kPollRead, kTagVideoRtcp);
    }
    if (ingest_audio_rtp_fd_ < 0) {
      const uint16_t desired_a =
          (config_.base_rtp_port == 0) ? 0 : static_cast<uint16_t>(bound_video_rtp_port_ + 2U);
      if (!net::bind_udp_port_pair(desired_a,
              ingest_audio_rtp_fd_,
              ingest_audio_rtcp_fd_,
              bound_audio_rtp_port_,
              bound_audio_rtcp_port_,
              true)) {
        return false;
      }
      (void)ingest_poller_.add_or_mod(ingest_audio_rtp_fd_, net::kPollRead, kTagAudioRtp);
      (void)ingest_poller_.add_or_mod(ingest_audio_rtcp_fd_, net::kPollRead, kTagAudioRtcp);
    }
    ingest.get_rtsp_session().set_base_ports(bound_video_rtp_port_, bound_video_rtcp_port_);
    ingest.get_rtsp_session().set_server_ports(bound_video_rtp_port_, bound_video_rtcp_port_);
    return true;
  }

  void close_ingest_udp_sockets() noexcept {
    auto close_one = [&](int &fd) {
      if (fd >= 0) {
        ingest_poller_.remove(fd);
        ::close(fd);
        fd = -1;
      }
    };
    close_one(ingest_video_rtp_fd_);
    close_one(ingest_video_rtcp_fd_);
    close_one(ingest_audio_rtp_fd_);
    close_one(ingest_audio_rtcp_fd_);
  }

  void record_classifier_diagnostics(const proto::rtp::RtpPacketInfo &info) noexcept {
    auto &m = ingest.metrics();
    if (info.hevc_nonbase_layer) {
      m.hevc_nonbase_layer_pkts.fetch_add(1, std::memory_order_relaxed);
    }
    if (info.hevc_malformed) {
      switch (info.hevc_reason) {
        case media::hevc::MalformedReason::BadNalHeader:
          m.hevc_malformed_nal_header.fetch_add(1, std::memory_order_relaxed);
          break;
        case media::hevc::MalformedReason::BadAp:
          m.hevc_malformed_ap.fetch_add(1, std::memory_order_relaxed);
          break;
        case media::hevc::MalformedReason::BadFu:
          m.hevc_malformed_fu.fetch_add(1, std::memory_order_relaxed);
          break;
        case media::hevc::MalformedReason::BadPaci:
          m.hevc_malformed_paci.fetch_add(1, std::memory_order_relaxed);
          break;
        case media::hevc::MalformedReason::None:
          break;
      }
    }
  }

  void classify_and_ingest_slab(memory::PooledBuffer *slab, uint32_t byte_len, uint8_t track_idx) {
    if (!slab) {
      return;
    }
    const auto    &desc_opt = ingest.stream_descriptor_thread0();
    const uint8_t *ptr      = slab->u8_data();

    proto::rtp::RtpPacketInfo info{};
    if (desc_opt.has_value() && track_idx < desc_opt->tracks.size()) {
      info = proto::rtp::RtpPacketClassifier::classify(ptr, byte_len, desc_opt->tracks[track_idx]);
    } else {
      info = proto::rtp::RtpPacketClassifier::classify(ptr, byte_len, track_idx);
    }

    if (info.is_rtcp) {
      ingest.metrics().ingest_rtcp_packets.fetch_add(1, std::memory_order_relaxed);
      slab->release();
      return;
    }
    if (!info.valid) {
      ingest.metrics().malformed_rtp_packets.fetch_add(1, std::memory_order_relaxed);
      slab->release();
      return;
    }

    record_classifier_diagnostics(info);

    const uint64_t now_ns = ingest_poller_.now_ns();
    RIVET_PROBE(IngestPacketReceived, now_ns, info.seq, info.track_id);

    auto pkt = memory::PacketRef::adopt(
        slab, 0, byte_len, now_ns, info.timestamp, info.seq, info.packed_track_meta, info.flags);
    (void)ingest.on_upstream_packet(std::move(pkt));
  }

  void flush_ingested_batch_to_shards() {
    const auto &batch = ingest.peek_batch();
    if (batch.empty()) {
      return;
    }
    router.broadcast(batch);
    ingest.clear_batch();

    if (inline_egress_mode_) {
      for (auto &r : egress_reactors) {
        r->step();
      }
    } else {
      for (auto &r : egress_reactors) {
        r->wake();
      }
    }
  }

  /// Drains UDP RTP datagrams directly into `PooledBuffer` slabs.
  void drain_udp_ingest_rtp(int fd, uint8_t track_idx) {
    if (fd < 0) {
      return;
    }
    const bool recording =
        (pub_fd_ >= 0 && ingest.get_rtsp_state() == proto::rtsp::RtspState::Recording);

    for (size_t iter = 0; iter < 64; ++iter) {
      auto *slab = allocate_ingest_slab(config_.numa_slab_size);
      if (!slab) {
        /// Arena exhausted: discard 1 datagram from socket so poller doesn't spin, count drop
        uint8_t       discard[16];
        const ssize_t r = ::recv(fd, discard, sizeof(discard), MSG_DONTWAIT);
        if (r > 0) {
          ingest.metrics().arena_exhaustion_drops.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        break;
      }

      sockaddr_in  src{};
      struct iovec iov{};
      iov.iov_base = slab->payload_data;
      iov.iov_len  = slab->capacity;

      struct msghdr msg{};
      msg.msg_name    = &src;
      msg.msg_namelen = sizeof(src);
      msg.msg_iov     = &iov;
      msg.msg_iovlen  = 1;

#if defined(__linux__)
      const int recv_flags = MSG_DONTWAIT | MSG_TRUNC;
#else
      const int recv_flags = MSG_DONTWAIT;
#endif
      const ssize_t bytes = ::recvmsg(fd, &msg, recv_flags);
      if (bytes <= 0) {
        slab->release();
        break;
      }

      /// Reject truncated / oversized UDP datagrams
      if ((msg.msg_flags & MSG_TRUNC) != 0 ||
          static_cast<size_t>(bytes) > static_cast<size_t>(slab->capacity)) {
        ingest.metrics().oversize_packet_drops.fetch_add(1, std::memory_order_relaxed);
        slab->release();
        continue;
      }

      /// Enforce `RtspState::Recording` and latched publisher UDP source
      if (!recording) {
        ingest.metrics().unauthorized_packet_drops.fetch_add(1, std::memory_order_relaxed);
        slab->release();
        continue;
      }
      if (has_latched_pub_addr_ && src.sin_addr.s_addr != latched_pub_addr_) {
        ingest.metrics().unauthorized_packet_drops.fetch_add(1, std::memory_order_relaxed);
        slab->release();
        continue;
      }
      const uint16_t src_port = ntohs(src.sin_port);
      if (track_idx < latched_pub_rtp_ports_.size()) {
        if (latched_pub_rtp_ports_[track_idx] == 0) {
          latched_pub_rtp_ports_[track_idx] = src_port;
        } else if (latched_pub_rtp_ports_[track_idx] != src_port) {
          ingest.metrics().unauthorized_packet_drops.fetch_add(1, std::memory_order_relaxed);
          slab->release();
          continue;
        }
      }

      classify_and_ingest_slab(slab, static_cast<uint32_t>(bytes), track_idx);
    }

    flush_ingested_batch_to_shards();
  }

  void drain_udp_ingest_rtcp(int fd) noexcept {
    if (fd < 0) {
      return;
    }
    uint8_t buf[1500];
    while (true) {
      sockaddr_in   src{};
      socklen_t     slen = sizeof(src);
      const ssize_t n =
          ::recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&src), &slen);
      if (n <= 0) {
        break;
      }
      if (has_latched_pub_addr_ && src.sin_addr.s_addr != latched_pub_addr_) {
        ingest.metrics().unauthorized_packet_drops.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      ingest.metrics().ingest_rtcp_packets.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void
  dispatch_publisher_interleaved_slab(uint8_t channel, memory::PooledBuffer *slab, uint32_t plen) {
    if (!slab) {
      return;
    }
    if (ingest.get_rtsp_state() != proto::rtsp::RtspState::Recording) {
      ingest.metrics().unauthorized_packet_drops.fetch_add(1, std::memory_order_relaxed);
      slab->release();
      return;
    }

    const auto binding = ingest.get_rtsp_session().lookup_interleaved_channel(channel);
    if (!binding.bound) {
      ingest.metrics().unauthorized_packet_drops.fetch_add(1, std::memory_order_relaxed);
      slab->release();
      return;
    }
    if (binding.is_rtcp) {
      ingest.metrics().ingest_rtcp_packets.fetch_add(1, std::memory_order_relaxed);
      slab->release();
      return;
    }

    classify_and_ingest_slab(slab, plen, binding.track_index);
  }

  void handle_publisher_rtsp_text(std::string_view req_text) {
    const auto parsed = proto::rtsp::parse_rtsp_request_text(req_text);
    if (parsed.valid && parsed.method == "SETUP") {
      /// Select dedicated video or audio UDP port pair based on track index
      uint8_t trk = 0;
      if (const auto &d = ingest.get_rtsp_session().get_stream_descriptor(); d.has_value()) {
        if (auto resolved = media::resolve_setup_track(*d, stream_id, parsed.uri)) {
          trk = *resolved;
        }
      } else if (parsed.uri.find("trackID=1") != std::string_view::npos ||
          parsed.uri.find("streamid=1") != std::string_view::npos) {
        trk = 1;
      }
      if (trk == 1 && bound_audio_rtp_port_ != 0) {
        ingest.get_rtsp_session().set_server_ports(bound_audio_rtp_port_, bound_audio_rtcp_port_);
      } else {
        ingest.get_rtsp_session().set_server_ports(bound_video_rtp_port_, bound_video_rtcp_port_);
      }
    }

    const auto  prev_state = ingest.get_rtsp_state();
    std::string resp       = ingest.handle_rtsp_request(req_text);
    append_pub_outbox(resp);

    if (parsed.valid && parsed.method == "SETUP" && resp.starts_with("RTSP/1.0 200")) {
      const uint8_t trk     = ingest.get_rtsp_session().get_last_setup_track();
      const auto   &binding = ingest.get_rtsp_session().track_binding(trk);
      if (binding.is_setup && !binding.is_interleaved && trk < latched_pub_rtp_ports_.size()) {
        latched_pub_rtp_ports_[trk] = binding.client_rtp_port;
      }
    }

    const auto new_state = ingest.get_rtsp_state();
    if (new_state != prev_state || (parsed.valid && parsed.method == "ANNOUNCE")) {
      notify_controller_publisher_state();
    }

    if (parsed.valid && parsed.method == "TEARDOWN" && resp.starts_with("RTSP/1.0 200")) {
      flush_pub_outbox();
      close_publisher();
    }
  }

  /// Reads from `pub_fd_`, using direct-to-slab `recv` for `$` payloads.
  void handle_publisher_readable() {
    if (pub_fd_ < 0) {
      return;
    }

    while (pub_fd_ >= 0) {
      /// Fast path: finish reading in-flight direct-to-slab `$` payload directly into `PooledBuffer`
      if (pub_direct_slab_ != nullptr) {
        const size_t  rem = pub_direct_total_ - pub_direct_offset_;
        const ssize_t n =
            ::recv(pub_fd_, pub_direct_slab_->payload_data + pub_direct_offset_, rem, MSG_DONTWAIT);
        if (n > 0) {
          pub_direct_offset_ += static_cast<uint32_t>(n);
          if (pub_direct_offset_ == pub_direct_total_) {
            auto          *done_slab = pub_direct_slab_;
            const uint32_t done_len  = pub_direct_total_;
            const uint8_t  done_ch   = pub_direct_channel_;
            pub_direct_slab_         = nullptr;
            pub_direct_total_        = 0;
            pub_direct_offset_       = 0;
            dispatch_publisher_interleaved_slab(done_ch, done_slab, done_len);
          }
          continue;
        }
        if (n == 0) {
          close_publisher();
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        close_publisher();
        break;
      }

      /// Otherwise read into `pub_read_buf_` and parse RTSP / `$` headers
      auto span = pub_read_buf_.writable_span(8192);
      if (span.empty()) {
        close_publisher();
        break;
      }
      const ssize_t n = ::recv(pub_fd_, span.data(), span.size(), MSG_DONTWAIT);
      if (n > 0) {
        pub_read_buf_.commit_write(static_cast<size_t>(n));
        if (!process_publisher_buffered_frames()) {
          close_publisher();
          break;
        }
        continue;
      }
      if (n == 0) {
        close_publisher();
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      close_publisher();
      break;
    }

    flush_ingested_batch_to_shards();
    flush_pub_outbox();
  }

  [[nodiscard]] bool process_publisher_buffered_frames() {
    while (pub_fd_ >= 0 && !pub_read_buf_.empty() && pub_direct_slab_ == nullptr) {
      const auto readable = pub_read_buf_.readable_span();

      /// Direct-to-slab switchover when a `$` header arrives whose body isn't fully buffered yet
      if (readable[0] == static_cast<uint8_t>('$')) {
        if (readable.size() < 4U) {
          return true; //< Need remaining bytes of 4-byte `$` header
        }
        const uint8_t  ch = readable[1];
        const uint16_t plen =
            static_cast<uint16_t>((static_cast<uint16_t>(readable[2]) << 8U) | readable[3]);
        if (plen == 0U) {
          pub_read_buf_.consume(4U);
          continue;
        }

        auto *slab = allocate_ingest_slab(plen);
        if (!slab) {
          ingest.metrics().arena_exhaustion_drops.fetch_add(1, std::memory_order_relaxed);
          if (readable.size() >= 4U + plen) {
            pub_read_buf_.consume(4U + plen);
            continue;
          }
          /// Drain partial frame via `RtspStreamFramer` wait
          break;
        }

        const size_t avail_payload = readable.size() - 4U;
        if (avail_payload >= plen) {
          std::memcpy(slab->payload_data, readable.data() + 4U, plen);
          pub_read_buf_.consume(4U + plen);
          dispatch_publisher_interleaved_slab(ch, slab, plen);
          continue;
        }

        /// Partial `$` payload in `pub_read_buf_`: move prefix into `slab` and recv rest directly!
        if (avail_payload > 0) {
          std::memcpy(slab->payload_data, readable.data() + 4U, avail_payload);
        }
        pub_read_buf_.clear();
        pub_direct_slab_    = slab;
        pub_direct_total_   = plen;
        pub_direct_offset_  = static_cast<uint32_t>(avail_payload);
        pub_direct_channel_ = ch;
        return true;
      }

      auto view = pub_read_buf_.next_view();
      if (view.type == proto::rtsp::FramedMessageType::None) {
        return true;
      }
      if (view.type == proto::rtsp::FramedMessageType::Error) {
        return false;
      }
      if (view.type == proto::rtsp::FramedMessageType::RtspRequest) {
        const std::string req_copy(view.as_text());
        pub_read_buf_.consume(view.wire_bytes);
        handle_publisher_rtsp_text(req_copy);
      }
    }
    return true;
  }

  void accept_publisher_handoff(int client_fd,
      std::string                   ip,
      std::string                   initial_bytes,
      std::string                   auth_nonce = {}) {
    if (client_fd < 0) {
      return;
    }

    /// Single-active-publisher guard: reject second publisher if one is active!
    const bool conflict = (pub_fd_ >= 0 && ingest.get_rtsp_session().has_announced());
    if (conflict) {
      net::configure_tcp_publisher_socket(client_fd);
      proto::rtsp::RtspControlSession reject_sess(bound_video_rtp_port_, bound_video_rtcp_port_);
      reject_sess.set_publish_credentials(config_.publish_user, config_.publish_password);
      reject_sess.set_auth_nonce(auth_nonce);
      reject_sess.set_stream_context(ingest.get_stream_descriptor_snapshot(),
          ingest.get_config_tracker().current_snapshot(),
          true /* conflict! */);
      std::string buf = std::move(initial_bytes);
      auto        msg = proto::rtsp::RtspStreamFramer::next_frame(buf);
      if (msg.type == proto::rtsp::FramedMessageType::RtspRequest) {
        std::string resp = reject_sess.handle_request(msg.data);
        (void)::send(client_fd, resp.data(), resp.size(), net::nonblocking_send_flags());
      }
      ::close(client_fd);
      return;
    }

    /// Replace stale half-open publisher socket if not yet announced
    if (pub_fd_ >= 0) {
      close_publisher();
    }

    net::configure_tcp_publisher_socket(client_fd);
    pub_fd_ = client_fd;
    pub_ip_ = std::move(ip);

    sockaddr_in paddr{};
    if (net::make_ipv4_addr(pub_ip_, 0, paddr)) {
      latched_pub_addr_     = paddr.sin_addr.s_addr;
      has_latched_pub_addr_ = true;
    }

    ingest.get_rtsp_session() =
        proto::rtsp::RtspControlSession(bound_video_rtp_port_, bound_video_rtcp_port_);
    ingest.get_rtsp_session().set_stream_path(stream_id);
    ingest.get_rtsp_session().set_publish_credentials(
        config_.publish_user, config_.publish_password);
    ingest.get_rtsp_session().set_auth_nonce(std::move(auth_nonce));

    if (!initial_bytes.empty()) {
      if (!pub_read_buf_.append(initial_bytes.data(), initial_bytes.size())) {
        close_publisher();
        return;
      }
    }

    (void)ingest_poller_.add_or_mod(pub_fd_, net::kPollRead, kTagPubTcp);
    if (!process_publisher_buffered_frames()) {
      close_publisher();
      return;
    }
    flush_ingested_batch_to_shards();
    flush_pub_outbox();
  }

  void maybe_emit_upstream_pli(uint64_t now_ns) {
    if (!pli_requested_.load(std::memory_order_relaxed)) {
      return;
    }
    if (now_ns < last_pli_sent_ns_ + 250'000'000ULL) {
      return; //< Coalesce downstream PLI storms to at most 4 Hz
    }
    pli_requested_.store(false, std::memory_order_relaxed);
    last_pli_sent_ns_ = now_ns;

    if (ingest.get_rtsp_state() != proto::rtsp::RtspState::Recording) {
      return;
    }
    const uint32_t                  media_ssrc = ingest.get_track_ssrc(0);
    proto::rtp::RtcpReportGenerator gen(0x52495654U);
    const auto                      pli = gen.build_pli(media_ssrc);

    const auto &binding = ingest.get_rtsp_session().track_binding(0);
    if (binding.is_setup && binding.is_interleaved && pub_fd_ >= 0) {
      std::array<uint8_t, 16> frame{};
      frame[0] = static_cast<uint8_t>('$');
      frame[1] = binding.rtcp_channel;
      frame[2] = 0;
      frame[3] = static_cast<uint8_t>(pli.size());
      std::memcpy(frame.data() + 4, pli.data(), pli.size());
      append_pub_outbox(
          std::string_view(reinterpret_cast<const char *>(frame.data()), frame.size()));
      flush_pub_outbox();
    } else if (binding.is_setup && !binding.is_interleaved && ingest_video_rtcp_fd_ >= 0 &&
        binding.client_rtcp_port > 0) {
      sockaddr_in dest{};
      if (net::make_ipv4_addr(pub_ip_, binding.client_rtcp_port, dest)) {
        (void)::sendto(ingest_video_rtcp_fd_,
            pli.data(),
            pli.size(),
            net::nonblocking_send_flags(),
            reinterpret_cast<const sockaddr *>(&dest),
            sizeof(dest));
      }
    }
  }

  void handle_uds_messages() {
    while (uds_fd_ >= 0) {
      ipc::IpcMsgHeader    hdr{};
      std::vector<uint8_t> payload;
      int                  passed_fd = -1;
      const auto           st        = ipc::recv_ipc_message(uds_fd_, hdr, payload, passed_fd);
      if (st == ipc::IpcRecvStatus::WouldBlock) {
        break;
      }
      if (st == ipc::IpcRecvStatus::EofOrError) {
        /// Master supervisor closed UDS or exited -> shut down worker cleanly
        is_running.store(false, std::memory_order_release);
        break;
      }

      const auto msg_type = static_cast<ipc::IpcMsgType>(hdr.type);
      switch (msg_type) {
        case ipc::IpcMsgType::HandoffPublisher: {
          std::string ip;
          std::string init_bytes;
          std::string nonce;
          if (passed_fd >= 0 &&
              ipc::decode_handoff_payload(payload.data(), payload.size(), ip, init_bytes, &nonce)) {
            accept_publisher_handoff(
                passed_fd, std::move(ip), std::move(init_bytes), std::move(nonce));
          } else if (passed_fd >= 0) {
            ::close(passed_fd);
          }
          break;
        }
        case ipc::IpcMsgType::HandoffViewer: {
          std::string ip;
          std::string init_bytes;
          if (passed_fd >= 0 &&
              ipc::decode_handoff_payload(payload.data(), payload.size(), ip, init_bytes)) {
            handoff_viewer_to_least_loaded_shard(passed_fd, std::move(ip), std::move(init_bytes));
          } else if (passed_fd >= 0) {
            ::close(passed_fd);
          }
          break;
        }
        case ipc::IpcMsgType::RequestMetrics: {
          if (passed_fd >= 0) {
            ::close(passed_fd);
          }
          const MetricsSnapshot snap = snapshot_metrics();
          (void)ipc::send_ipc_message(uds_fd_, ipc::IpcMsgType::MetricsReply, &snap, sizeof(snap));
          break;
        }
        case ipc::IpcMsgType::QuiesceAndDrain: {
          if (passed_fd >= 0) {
            ::close(passed_fd);
          }
          quiescing_.store(true, std::memory_order_release);
          close_publisher();
          for (auto &r : egress_reactors) {
            r->set_quiesced(true);
          }
          break;
        }
        case ipc::IpcMsgType::Shutdown: {
          if (passed_fd >= 0) {
            ::close(passed_fd);
          }
          is_running.store(false, std::memory_order_release);
          break;
        }
        default:
          if (passed_fd >= 0) {
            ::close(passed_fd);
          }
          break;
      }
    }
  }

public:
  explicit RivetWorker(std::string id,
      uint32_t                     num_egress_shards = 2,
      uint16_t                     base_rtp          = 20000,
      uint16_t                     base_rtcp         = 20001)
      : arena_(4096, 64, 512, false)
      , stream_id(std::move(id))
      , ingest(16384, 16ULL * 1024ULL * 1024ULL)
      , egress_shard_count(num_egress_shards > 0 ? num_egress_shards : 1) {
    config_.stream_name    = stream_id;
    config_.egress_shards  = num_egress_shards;
    config_.base_rtp_port  = base_rtp;
    config_.base_rtcp_port = base_rtcp;
    init_shards(base_rtp, base_rtcp);
  }

  RivetWorker(std::string id, const config::RivetConfig &cfg)
      : arena_(cfg.numa_capacity > 0 ? cfg.numa_capacity : 4096, 64, 512, cfg.enable_hugepages)
      , stream_id(std::move(id))
      , config_(cfg)
      , ingest(16384, cfg.retention_budget_bytes)
      , egress_shard_count(cfg.egress_shards > 0 ? cfg.egress_shards : 1) {
    init_shards(cfg.base_rtp_port, cfg.base_rtcp_port);
  }

  ~RivetWorker() { stop(); }

  RivetWorker(const RivetWorker &)            = delete;
  RivetWorker &operator=(const RivetWorker &) = delete;
  RivetWorker(RivetWorker &&)                 = delete;
  RivetWorker &operator=(RivetWorker &&)      = delete;

  [[nodiscard]] std::string_view         get_stream_id() const noexcept { return stream_id; }
  [[nodiscard]] const IngestReactor     &get_ingest() const noexcept { return ingest; }
  [[nodiscard]] IngestReactor           &get_ingest() noexcept { return ingest; }
  [[nodiscard]] memory::MultiClassArena &get_arena() noexcept { return arena_; }

  [[nodiscard]] const std::string &get_sdp() const noexcept {
    return ingest.get_rtsp_session().get_announced_sdp();
  }

  void set_sdp(std::string sdp) {
    ingest.get_rtsp_session().set_announced_sdp(std::move(sdp));
    ingest.sync_descriptor_from_session();
  }

  [[nodiscard]] const PacketRouter &get_router() const noexcept { return router; }
  [[nodiscard]] PacketRouter       &get_router() noexcept { return router; }

  [[nodiscard]] size_t get_egress_shard_count() const noexcept { return egress_reactors.size(); }

  [[nodiscard]] EgressReactor *get_egress_reactor(size_t index) noexcept {
    return (index < egress_reactors.size()) ? egress_reactors[index].get() : nullptr;
  }

  void set_udp_sockets(int v_sock, int a_sock) noexcept {
    for (auto &reactor : egress_reactors) {
      reactor->set_udp_sockets(v_sock, a_sock);
    }
  }

  void request_upstream_pli() noexcept {
    pli_requested_.store(true, std::memory_order_relaxed);
    ingest_poller_.wake();
  }

  void handoff_viewer_to_least_loaded_shard(int client_fd,
      std::string                               client_ip,
      std::string                               initial_bytes) {
    if (egress_reactors.empty()) {
      if (client_fd >= 0) {
        ::close(client_fd);
      }
      return;
    }
    size_t best_shard = 0;
    size_t min_subs   = egress_reactors[0]->subscriber_count();
    for (size_t i = 1; i < egress_reactors.size(); ++i) {
      const size_t c = egress_reactors[i]->subscriber_count();
      if (c < min_subs) {
        min_subs   = c;
        best_shard = i;
      }
    }
    egress_reactors[best_shard]->handoff_viewer(
        client_fd, std::move(client_ip), std::move(initial_bytes));
    if (inline_egress_mode_) {
      egress_reactors[best_shard]->step();
    }
  }

  uint32_t assign_subscriber(std::unique_ptr<SubscriberSession> session) {
    if (egress_reactors.empty() || !session) {
      return 0;
    }
    uint32_t best_shard      = 0;
    size_t   min_subscribers = egress_reactors[0]->subscriber_count();
    for (size_t i = 1; i < egress_reactors.size(); ++i) {
      if (egress_reactors[i]->subscriber_count() < min_subscribers) {
        min_subscribers = egress_reactors[i]->subscriber_count();
        best_shard      = static_cast<uint32_t>(i);
      }
    }
    egress_reactors[best_shard]->add_subscriber(std::move(session), &ingest.get_packet_log());
    return best_shard;
  }

  std::optional<media::AccessUnitView> process_incoming_packet(memory::PacketRef packet) {
    RIVET_ZONE("Worker::process_incoming_packet");
    auto au = ingest.on_upstream_packet(std::move(packet));
    broadcast_to_reactors();
    step_egress_reactors();
    return au;
  }

  void broadcast_to_reactors() {
    const auto &batch = ingest.peek_batch();
    if (!batch.empty()) {
      router.broadcast(batch);
      ingest.clear_batch();
    }
  }

  void step_egress_reactors() {
    for (auto &reactor : egress_reactors) {
      reactor->step();
    }
  }

  /// Lightweight activation (preserves fast unit-test contract without spawning OS threads).
  void run() {
    is_running.store(true, std::memory_order_release);
    ingest.run();
    for (auto &reactor : egress_reactors) {
      reactor->run(false);
    }
  }

  /// Runs the full event-driven Thread-0 `IngestReactor` loop + Threads `1..S` `EgressReactor`
  /// shards connected to `uds_fd` (used identically by `--worker` child processes and `InProcess`
  /// background workers!).
  void run_process_loop(int uds_fd, bool take_uds_ownership = true) {
    uds_fd_      = uds_fd;
    owns_uds_fd_ = take_uds_ownership;
    arena_.bind_owner_thread();

    is_running.store(true, std::memory_order_release);
    loop_active_.store(true, std::memory_order_release);

    (void)bind_ingest_udp_sockets();
    if (uds_fd_ >= 0) {
      (void)ingest_poller_.add_or_mod(uds_fd_, net::kPollRead, kTagUds);
    }

    ingest.run();
    for (auto &reactor : egress_reactors) {
      if (inline_egress_mode_) {
        (void)reactor->bind_dedicated_udp_ports();
        reactor->run(false);
      } else {
        reactor->run(true /* spawn dedicated shard OS thread! */);
      }
    }

    /// Notify supervisor (`RivetController`) that worker sockets and shard threads are ready
    if (uds_fd_ >= 0) {
      ipc::WorkerReadyPayload ready{};
      ready.pid             = static_cast<uint32_t>(::getpid());
      ready.video_rtp_port  = bound_video_rtp_port_;
      ready.video_rtcp_port = bound_video_rtcp_port_;
      ready.audio_rtp_port  = bound_audio_rtp_port_;
      ready.audio_rtcp_port = bound_audio_rtcp_port_;
      (void)ipc::send_ipc_message(uds_fd_, ipc::IpcMsgType::WorkerReady, &ready, sizeof(ready));
    }

    std::array<net::PollEvent, 64> events{};
    while (is_running.load(std::memory_order_acquire)) {
      const size_t n = ingest_poller_.wait(events, 10);

      for (size_t i = 0; i < n; ++i) {
        const uint64_t tag = events[i].tag;
        const uint32_t fl  = events[i].flags;

        if (tag == kTagUds) {
          handle_uds_messages();
        } else if (tag == kTagPubTcp) {
          if ((fl & net::kPollRead) != 0) {
            handle_publisher_readable();
          }
          if ((fl & net::kPollWrite) != 0) {
            flush_pub_outbox();
          }
          if ((fl & (net::kPollHup | net::kPollError)) != 0 && pub_fd_ >= 0) {
            handle_publisher_readable();
          }
        } else if (tag == kTagVideoRtp) {
          drain_udp_ingest_rtp(ingest_video_rtp_fd_, 0);
        } else if (tag == kTagVideoRtcp) {
          drain_udp_ingest_rtcp(ingest_video_rtcp_fd_);
        } else if (tag == kTagAudioRtp) {
          drain_udp_ingest_rtp(ingest_audio_rtp_fd_, 1);
        } else if (tag == kTagAudioRtcp) {
          drain_udp_ingest_rtcp(ingest_audio_rtcp_fd_);
        }
      }

      maybe_emit_upstream_pli(ingest_poller_.now_ns());
      if (inline_egress_mode_) {
        step_egress_reactors();
      }

      if (quiescing_.load(std::memory_order_acquire) && total_subscribers() == 0 && pub_fd_ < 0) {
        break;
      }
    }

    /// Clean teardown of Thread-0 sockets and Threads 1..S before exiting loop
    close_publisher();
    close_ingest_udp_sockets();
    for (auto &reactor : egress_reactors) {
      reactor->stop();
    }
    ingest.stop();

    if (uds_fd_ >= 0) {
      ingest_poller_.remove(uds_fd_);
      if (owns_uds_fd_) {
        ::close(uds_fd_);
      }
      uds_fd_ = -1;
    }
    loop_active_.store(false, std::memory_order_release);
    is_running.store(false, std::memory_order_release);
  }

  /// Launches `run_process_loop(uds_fd)` on a background Thread-0 (used in
  /// `ExecutionMode::InProcess`).
  void start_background(int uds_fd) {
    if (loop_active_.load(std::memory_order_acquire)) {
      return;
    }
    is_running.store(true, std::memory_order_release);
    ingest_thread_ = std::thread([this, uds_fd]() { run_process_loop(uds_fd, true); });
  }

  void stop() noexcept {
    is_running.store(false, std::memory_order_release);
    ingest_poller_.wake();
    if (ingest_thread_.joinable()) {
      ingest_thread_.join();
    }
    close_publisher();
    close_ingest_udp_sockets();
    for (auto &reactor : egress_reactors) {
      reactor->stop();
    }
    ingest.stop();
    if (uds_fd_ >= 0 && owns_uds_fd_) {
      ::close(uds_fd_);
      uds_fd_ = -1;
    }
  }

  [[nodiscard]] bool is_active() const noexcept {
    return is_running.load(std::memory_order_acquire);
  }

  [[nodiscard]] size_t total_subscribers() const noexcept {
    size_t count = 0;
    for (const auto &reactor : egress_reactors) {
      count += reactor->subscriber_count();
    }
    return count;
  }

  [[nodiscard]] MetricsSnapshot snapshot_metrics() const noexcept {
    return ingest.metrics().snapshot(
        total_subscribers(), ingest.get_config_tracker().current_generation(), 0);
  }
};

} //< namespace rivet::core
