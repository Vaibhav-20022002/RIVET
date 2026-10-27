#pragma once

#include "rivet/core/Zones.hpp"
#include "rivet/media/AccessUnitView.hpp"
#include "rivet/media/AuAssembler.hpp"
#include "rivet/media/ConfigGeneration.hpp"
#include "rivet/media/H264RapClassifier.hpp"
#include "rivet/media/HevcRapClassifier.hpp"
#include "rivet/media/RetainedPacketLog.hpp"
#include "rivet/memory/PacketRef.hpp"
#include "rivet/proto/rtp/SequenceTracker.hpp"
#include "rivet/proto/rtsp/RtspControlSession.hpp"
#include <atomic>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
/// #include <string_view>
#include <vector>

namespace rivet::core {

/// The IngestReactor runs on Thread 0, owning the single connection to the upstream camera/encoder,
/// unwrapping RTP sequence numbers, assembling Access Units, caching parameter sets,
/// and appending immutable packets into the RetainedPacketLog for EgressReactors.
class IngestReactor {
  proto::rtsp::RtspControlSession rtsp_session;
  media::AuAssembler              assembler;
  media::RetainedPacketLog        packet_log;
  media::ConfigTracker            config_tracker;
  proto::rtp::SequenceTracker     video_seq_tracker;
  proto::rtp::SequenceTracker     audio_seq_tracker;

  std::atomic<bool> is_running{false};

  /// Lock-free outgoing batch storage for egress reactors
  std::vector<memory::PacketRef> egress_batch;

public:
  IngestReactor() noexcept = default;

  /// Handles incoming RTSP signaling on the TCP control plane (RFC 2326)
  ///\param request Raw RTSP request string (OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN)
  ///\return Formatted RTSP response string
  std::string handle_rtsp_request(std::string_view request) {
    return rtsp_session.handle_request(request);
  }

  /// Current RTSP session state
  [[nodiscard]] proto::rtsp::RtspState get_rtsp_state() const noexcept {
    return rtsp_session.get_state();
  }

  [[nodiscard]] const proto::rtsp::RtspControlSession &get_rtsp_session() const noexcept {
    return rtsp_session;
  }

  [[nodiscard]] proto::rtsp::RtspControlSession &get_rtsp_session() noexcept {
    return rtsp_session;
  }

  /// Processes an incoming raw RTP packet from the network / io_uring socket
  ///\param packet An owning PacketRef handle to the pooled packet memory
  ///\return An assembled AccessUnitView if this packet completes a video frame, or std::nullopt
  std::optional<media::AccessUnitView> on_upstream_packet(memory::PacketRef packet) {
    RIVET_ZONE("Ingest::on_upstream_packet");
    if (!packet) {
      return std::nullopt;
    }

    /// Audio Track (track_id == 1)
    if (packet.track_id == 1) {
      return handle_audio_packet(std::move(packet));
    }

    /// Video Track (track_id == 0)
    return handle_video_packet(std::move(packet));
  }

  /// Processes inbound AAC audio packet
  std::optional<media::AccessUnitView> handle_audio_packet(memory::PacketRef packet) {
    auto ext_seq = audio_seq_tracker.update(packet.sequence);
    if (!ext_seq.has_value()) {
      return std::nullopt;
    }
    egress_batch.push_back(std::move(packet));
    return std::nullopt;
  }

  /// Processes inbound H.264 video packet
  std::optional<media::AccessUnitView> handle_video_packet(memory::PacketRef packet) {
    auto ext_seq = video_seq_tracker.update(packet.sequence);
    if (!ext_seq.has_value()) {
      return std::nullopt;
    }

    const bool marker = packet.is_marker();
    inspect_parameter_sets(packet);

    packet_log.append(packet);
    egress_batch.push_back(packet);
    return assembler.on_packet(std::move(packet), marker);
  }

  /// Inspects video payload for Parameter Sets (SPS/PPS, including STAP-A aggregation)
  void inspect_parameter_sets(const memory::PacketRef &packet) {
    auto payload_span = packet.payload();
    if (!payload_span.empty()) {
      const auto *payload_ptr = reinterpret_cast<const uint8_t *>(payload_span.data());
      media::H264RapClassifier::for_each_parameter_set(payload_ptr,
          payload_span.size(),
          [&](media::H264NalType type, const uint8_t *ps_ptr, size_t ps_len) {
            if (type == media::H264NalType::SPS) {
              config_tracker.update_sps(ps_ptr, ps_len);
            } else if (type == media::H264NalType::PPS) {
              config_tracker.update_pps(ps_ptr, ps_len);
            }
          });
    }
  }

  /// Flushes and returns the current egress batch for distribution across Egress Reactors
  ///\return Batch of pre-retained PacketRef handles
  std::vector<memory::PacketRef> publish_batch() {
    std::vector<memory::PacketRef> batch;
    batch.swap(egress_batch);
    return batch;
  }

  /// Accessor for the RetainedPacketLog (used for new subscriber catch-up bursts)
  [[nodiscard]] const media::RetainedPacketLog &get_packet_log() const noexcept {
    return packet_log;
  }

  [[nodiscard]] media::RetainedPacketLog &get_packet_log() noexcept { return packet_log; }

  /// Accessor for the ConfigTracker
  [[nodiscard]] const media::ConfigTracker &get_config_tracker() const noexcept {
    return config_tracker;
  }

  /// Starts the Ingest Reactor event loop
  void run() {
    is_running.store(true, std::memory_order_release);
    std::cout << "[IngestReactor] Running ingest event loop...\n";
  }

  /// Signals the event loop to stop and clears retained packet buffers
  void stop() noexcept {
    is_running.store(false, std::memory_order_release);
    egress_batch.clear();
    packet_log.clear();
  }

  /// Checks if the reactor is active
  [[nodiscard]] bool is_active() const noexcept {
    return is_running.load(std::memory_order_acquire);
  }
};

} //< namespace rivet::core
