#pragma once

#include "rivet/media/AccessUnitView.hpp"
#include "rivet/media/AuAssembler.hpp"
#include "rivet/media/ConfigGeneration.hpp"
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
    if (!packet) {
      return std::nullopt;
    }

    /// Audio Track (track_id == 1)
    if (packet.track_id == 1) {
      auto ext_seq = audio_seq_tracker.update(packet.sequence);
      if (!ext_seq.has_value()) {
        return std::nullopt;
      }
      egress_batch.push_back(std::move(packet));
      return std::nullopt;
    }

    /// Video Track (track_id == 0)
    auto ext_seq = video_seq_tracker.update(packet.sequence);
    if (!ext_seq.has_value()) {
      return std::nullopt;
    }

    const bool marker = packet.is_marker();

    /// Inspect video payload for Parameter Sets (VPS/SPS/PPS)
    auto payload_span = packet.payload();
    if (payload_span.size() >= 2) {
      const auto *payload_ptr = reinterpret_cast<const uint8_t *>(payload_span.data());
      auto        nal_type = media::HevcRapClassifier::classify(payload_ptr, payload_span.size());
      if (nal_type == media::HevcNalType::ParameterSet) {
        uint8_t raw_nal_type = (payload_ptr[0] >> 1) & 0x3F;
        if (raw_nal_type == 32) { //< VPS_NUT
          config_tracker.update_vps(payload_ptr, payload_span.size());
        } else if (raw_nal_type == 33) { //< SPS_NUT
          config_tracker.update_sps(payload_ptr, payload_span.size());
        } else if (raw_nal_type == 34) { //< PPS_NUT
          config_tracker.update_pps(payload_ptr, payload_span.size());
        }
      }
    }

    packet_log.append(packet);
    egress_batch.push_back(packet);
    return assembler.on_packet(std::move(packet), marker);
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
