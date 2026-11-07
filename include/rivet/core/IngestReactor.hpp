#pragma once

#include "rivet/core/Log.hpp"
#include "rivet/core/Metrics.hpp"
#include "rivet/core/Zones.hpp"
#include "rivet/media/AccessUnitView.hpp"
#include "rivet/media/AuAssembler.hpp"
#include "rivet/media/ConfigGeneration.hpp"
#include "rivet/media/H264RapClassifier.hpp"
#include "rivet/media/HevcPayload.hpp"
#include "rivet/media/RetainedPacketLog.hpp"
#include "rivet/media/StreamDescriptor.hpp"
#include "rivet/memory/PacketFlags.hpp"
#include "rivet/memory/PacketRef.hpp"
#include "rivet/proto/rtp/SequenceTracker.hpp"
#include "rivet/proto/rtsp/RtspControlSession.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rivet::core {

/// Thread-0 IngestReactor: owns upstream RTP/RTSP state, per-track sequence unwrapping & SSRC
/// tracking, zero-alloc AU boundary/completeness tracking, gated H.264/HEVC parameter-set
/// extraction on `pkt.rtp_payload()`, and `PacketRing` + `RapEntry` indexing.
class IngestReactor {
  static constexpr size_t kMaxTracks = media::StreamDescriptor::MAX_TRACKS;

  struct VideoAuTracker {
    bool     open{false};
    uint32_t rtp_ts{0};
    uint64_t first_ring_pos{0};
    uint64_t first_byte_pos{0};
    uint32_t au_bytes{0};
    uint16_t last_seq{0};
    bool     has_gap{false};
    bool     fu_open{false};
    bool     is_irap{false};
    bool     is_closed_irap{false};
    bool     is_open_irap{false};
    uint8_t  track_id{0};
    uint32_t generation{0};

    void reset() noexcept { *this = VideoAuTracker{}; }
  };

  proto::rtsp::RtspControlSession        rtsp_session;
  std::optional<media::StreamDescriptor> stream_desc;
  mutable std::mutex                     desc_mutex_;

  media::AuAssembler                                  assembler;
  media::RetainedPacketLog                            packet_log;
  media::ConfigTracker                                config_tracker;
  media::hevc::FuParamReassembler                     hevc_fu_param_;
  std::array<proto::rtp::SequenceTracker, kMaxTracks> seq_trackers_{};
  std::array<memory::PacketRef, kMaxTracks>           probation_hold_{};
  std::array<uint64_t, kMaxTracks>                    last_ext_seq_{};
  std::array<bool, kMaxTracks>                        has_last_ext_seq_{};
  std::array<uint32_t, kMaxTracks>                    track_ssrc_{};
  std::array<bool, kMaxTracks>                        has_track_ssrc_{};
  VideoAuTracker                                      au_tracker_{};

  AtomicMetrics     metrics_;
  std::atomic<bool> is_running{false};
  bool              au_assembly_enabled_{false};

  /// Reused outgoing batch buffer (`egress_batch.clear()` preserves capacity -> 0 allocs/tick)
  std::vector<memory::PacketRef> egress_batch;

  [[nodiscard]] static uint32_t extract_rtp_ssrc(const memory::PacketRef &pkt) noexcept {
    auto raw = pkt.payload();
    if (raw.size() < 12) {
      return 0;
    }
    const auto *b = reinterpret_cast<const uint8_t *>(raw.data());
    if ((b[0] >> 6) != 2) {
      return 0;
    }
    return (static_cast<uint32_t>(b[8]) << 24) | (static_cast<uint32_t>(b[9]) << 16) |
        (static_cast<uint32_t>(b[10]) << 8) | static_cast<uint32_t>(b[11]);
  }

  /// Extracts RTP payload span (skipping 12B+CSRC+ext RTP header; falls back to raw payload only
  /// for synthetic <12B unit-test packets).
  [[nodiscard]] static std::span<const uint8_t> extract_codec_payload(
      const memory::PacketRef &pkt) noexcept {
    auto rtp_pl = pkt.rtp_payload();
    if (!rtp_pl.empty()) {
      return {reinterpret_cast<const uint8_t *>(rtp_pl.data()), rtp_pl.size()};
    }
    auto raw = pkt.payload();
    if (!raw.empty() && raw.size() < 12) {
      return {reinterpret_cast<const uint8_t *>(raw.data()), raw.size()};
    }
    return {};
  }

  void inspect_hevc_fu_state(std::span<const uint8_t> codec_payload, bool has_donl, uint16_t seq) {
    if (codec_payload.size() < 3) {
      return;
    }
    const auto hdr = media::hevc::parse_nal_header(codec_payload[0], codec_payload[1]);
    if (!hdr.ok || hdr.layer_id > 0) {
      return;
    }

    uint8_t        outer_type = hdr.type;
    const uint8_t *body       = codec_payload.data() + 2;
    size_t         body_len   = codec_payload.size() - 2;
    if (outer_type == 50U) {
      if (codec_payload.size() < 5) {
        return;
      }
      const uint8_t ctype = static_cast<uint8_t>((codec_payload[2] >> 1U) & 0x3FU);
      const uint8_t phs_size =
          static_cast<uint8_t>(((codec_payload[2] & 0x01U) << 4U) | (codec_payload[3] >> 4U));
      const size_t inner = 4U + static_cast<size_t>(phs_size);
      if (inner >= codec_payload.size()) {
        return;
      }
      outer_type = ctype;
      body       = codec_payload.data() + inner;
      body_len   = codec_payload.size() - inner;
    }

    if (outer_type != 49U || body_len < 1U) {
      return;
    }

    const uint8_t fu_hdr   = body[0];
    const bool    fu_start = (fu_hdr & 0x80u) != 0;
    const bool    fu_end   = (fu_hdr & 0x40u) != 0;
    if (fu_start && !fu_end) {
      au_tracker_.fu_open = true;
    } else if (fu_end) {
      au_tracker_.fu_open = false;
    }

    /// Reassemble fragmented VPS(32) / SPS(33) / PPS(34) over FU
    const uint8_t fu_type = static_cast<uint8_t>(fu_hdr & 0x3Fu);
    if (fu_type >= 32U && fu_type <= 34U) {
      const bool ok = hevc_fu_param_.on_fu_packet(codec_payload.data(),
          codec_payload.size(),
          seq,
          has_donl,
          [&](uint8_t nal_type, const uint8_t *ps_ptr, size_t ps_len) {
            if (nal_type == 32U) {
              config_tracker.stage_vps(ps_ptr, ps_len);
            } else if (nal_type == 33U) {
              config_tracker.stage_sps(ps_ptr, ps_len);
            } else if (nal_type == 34U) {
              config_tracker.stage_pps(ps_ptr, ps_len);
            }
          });
      if (!ok) {
        metrics_.param_set_fu_overflow_or_gap.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  void finalize_current_au(bool tail_seq_verified) {
    if (!au_tracker_.open) {
      return;
    }
    const uint32_t new_gen = config_tracker.commit_au();

    if (tail_seq_verified && !au_tracker_.has_gap && !au_tracker_.fu_open && au_tracker_.is_irap) {
      media::RapEntry rap{};
      rap.pos        = au_tracker_.first_ring_pos;
      rap.byte_pos   = au_tracker_.first_byte_pos;
      rap.rtp_ts     = au_tracker_.rtp_ts;
      rap.generation = (new_gen != 0) ? new_gen : au_tracker_.generation;
      rap.au_bytes   = au_tracker_.au_bytes;
      rap.track_id   = au_tracker_.track_id;
      rap.open_rap   = au_tracker_.is_open_irap && !au_tracker_.is_closed_irap;
      rap.valid      = true;
      packet_log.ring().record_rap(rap);
    }
    au_tracker_.reset();
  }

public:
  explicit IngestReactor(size_t ring_slots             = 16384,
      size_t                    retention_budget_bytes = 16ULL * 1024ULL * 1024ULL)
      : packet_log(ring_slots, retention_budget_bytes) {
    egress_batch.reserve(128);
  }

  void set_au_assembly_enabled(bool enabled) noexcept { au_assembly_enabled_ = enabled; }
  [[nodiscard]] bool is_au_assembly_enabled() const noexcept { return au_assembly_enabled_; }

  void sync_descriptor_from_session() {
    const auto &desc_opt = rtsp_session.get_stream_descriptor();
    if (desc_opt.has_value()) {
      {
        std::lock_guard<std::mutex> lk(desc_mutex_);
        stream_desc = *desc_opt;
      }
      (void)config_tracker.seed_from_descriptor(*desc_opt);
      rtsp_session.set_stream_context(desc_opt, config_tracker.current_snapshot(), false);
    }
  }

  void set_stream_descriptor(const media::StreamDescriptor &desc) {
    {
      std::lock_guard<std::mutex> lk(desc_mutex_);
      stream_desc = desc;
    }
    (void)config_tracker.seed_from_descriptor(desc);
    rtsp_session.set_stream_context(desc, config_tracker.current_snapshot(), false);
  }

  [[nodiscard]] std::optional<media::StreamDescriptor> get_stream_descriptor_snapshot() const {
    std::lock_guard<std::mutex> lk(desc_mutex_);
    return stream_desc;
  }

  /// Zero-copy, lock-free accessor strictly for the Thread-0 owner (`RivetWorker` ingest path).
  [[nodiscard]] const std::optional<media::StreamDescriptor> &
  stream_descriptor_thread0() const noexcept {
    return stream_desc;
  }

  [[nodiscard]] uint32_t get_track_ssrc(uint8_t trk) const noexcept {
    if (trk < kMaxTracks && has_track_ssrc_[trk]) {
      return track_ssrc_[trk];
    }
    return 0;
  }

  /// Handles incoming RTSP signaling on the publisher TCP control plane
  std::string handle_rtsp_request(std::string_view request) {
    std::string resp = rtsp_session.handle_request(request);
    if (request.starts_with("ANNOUNCE")) {
      sync_descriptor_from_session();
    }
    return resp;
  }

  [[nodiscard]] proto::rtsp::RtspState get_rtsp_state() const noexcept {
    return rtsp_session.get_state();
  }
  [[nodiscard]] const proto::rtsp::RtspControlSession &get_rtsp_session() const noexcept {
    return rtsp_session;
  }
  [[nodiscard]] proto::rtsp::RtspControlSession &get_rtsp_session() noexcept {
    return rtsp_session;
  }

  [[nodiscard]] AtomicMetrics       &metrics() noexcept { return metrics_; }
  [[nodiscard]] const AtomicMetrics &metrics() const noexcept { return metrics_; }

  /// Processes an incoming RTP packet on Thread 0.
  std::optional<media::AccessUnitView> on_upstream_packet(memory::PacketRef packet) {
    RIVET_ZONE("Ingest::on_upstream_packet");
    if (!packet) {
      return std::nullopt;
    }

    const uint8_t trk = static_cast<uint8_t>(packet.track() % kMaxTracks);

    /// Detect upstream SSRC switch (Tier 1 publisher reconnect) and reset per-track state cleanly
    const uint32_t ssrc = extract_rtp_ssrc(packet);
    if (ssrc != 0) {
      if (has_track_ssrc_[trk] && track_ssrc_[trk] != ssrc) {
        seq_trackers_[trk].reset();
        probation_hold_[trk]   = memory::PacketRef{};
        has_last_ext_seq_[trk] = false;
        if (trk == 0) {
          hevc_fu_param_.reset();
          au_tracker_.reset();
          assembler.reset();
        }
      }
      has_track_ssrc_[trk] = true;
      track_ssrc_[trk]     = ssrc;
    }

    media::MediaKind  kind     = (trk == 0) ? media::MediaKind::Video : media::MediaKind::Audio;
    media::VideoCodec codec    = media::VideoCodec::H264;
    bool              has_donl = false;
    {
      std::lock_guard<std::mutex> lk(desc_mutex_);
      if (stream_desc.has_value() && trk < stream_desc->tracks.size()) {
        kind     = stream_desc->tracks[trk].kind;
        codec    = stream_desc->tracks[trk].codec;
        has_donl = (stream_desc->tracks[trk].hevc.sprop_max_don_diff > 0);
      }
    }

    if (kind != media::MediaKind::Video) {
      return handle_audio_packet(std::move(packet), trk);
    }
    return handle_video_packet(std::move(packet), trk, codec, has_donl);
  }

  void accept_validated_audio(memory::PacketRef packet, uint8_t t, uint64_t ext_seq) {
    if (has_last_ext_seq_[t] && ext_seq > last_ext_seq_[t] + 1ULL) {
      metrics_.sequence_gaps.fetch_add(
          ext_seq - (last_ext_seq_[t] + 1ULL), std::memory_order_relaxed);
    }
    has_last_ext_seq_[t] = true;
    last_ext_seq_[t]     = ext_seq;

    metrics_.ingest_packets.fetch_add(1, std::memory_order_relaxed);
    metrics_.ingest_bytes.fetch_add(packet.length, std::memory_order_relaxed);

    packet_log.ring().append(packet);
    egress_batch.push_back(std::move(packet));
  }

  std::optional<media::AccessUnitView> handle_audio_packet(memory::PacketRef packet,
      uint8_t                                                                trk = 1) {
    const uint8_t t             = static_cast<uint8_t>(trk % kMaxTracks);
    const bool    was_probation = seq_trackers_[t].is_in_probation();
    auto          ext_seq       = seq_trackers_[t].update(packet.sequence);
    if (!ext_seq.has_value()) {
      if (was_probation) {
        probation_hold_[t] = std::move(packet);
      }
      metrics_.sequence_duplicates_or_probation.fetch_add(1, std::memory_order_relaxed);
      return std::nullopt;
    }

    if (was_probation && probation_hold_[t] &&
        probation_hold_[t].sequence == static_cast<uint16_t>(packet.sequence - 1U)) {
      auto           first_pkt = std::move(probation_hold_[t]);
      const uint64_t first_ext = (*ext_seq > 0ULL) ? (*ext_seq - 1ULL) : 0ULL;
      accept_validated_audio(std::move(first_pkt), t, first_ext);
    } else {
      probation_hold_[t] = memory::PacketRef{};
    }

    accept_validated_audio(std::move(packet), t, *ext_seq);
    return std::nullopt;
  }

  std::optional<media::AccessUnitView> accept_validated_video(memory::PacketRef packet,
      uint8_t                                                                   t,
      uint64_t                                                                  ext_seq,
      media::VideoCodec                                                         codec,
      bool                                                                      has_donl) {
    if (has_last_ext_seq_[t] && ext_seq > last_ext_seq_[t] + 1ULL) {
      metrics_.sequence_gaps.fetch_add(
          ext_seq - (last_ext_seq_[t] + 1ULL), std::memory_order_relaxed);
    }
    has_last_ext_seq_[t] = true;
    last_ext_seq_[t]     = ext_seq;

    metrics_.ingest_packets.fetch_add(1, std::memory_order_relaxed);
    metrics_.ingest_bytes.fetch_add(packet.length, std::memory_order_relaxed);

    /// 1. Check AU boundary on timestamp change
    if (au_tracker_.open && packet.rtp_timestamp != au_tracker_.rtp_ts) {
      const bool clean_tail = (packet.sequence == static_cast<uint16_t>(au_tracker_.last_seq + 1u));
      finalize_current_au(clean_tail);
    }

    /// 2. Start new AU or check intra-AU sequence continuity
    const uint64_t byte_pos_before = packet_log.ring().write_bytes();
    const uint64_t next_ring_pos   = packet_log.ring().write_pos();

    if (!au_tracker_.open) {
      au_tracker_.open           = true;
      au_tracker_.rtp_ts         = packet.rtp_timestamp;
      au_tracker_.first_ring_pos = next_ring_pos;
      au_tracker_.first_byte_pos = byte_pos_before;
      au_tracker_.au_bytes       = 0;
      au_tracker_.last_seq       = packet.sequence;
      au_tracker_.has_gap        = false;
      au_tracker_.fu_open        = false;
      au_tracker_.is_irap        = false;
      au_tracker_.is_closed_irap = false;
      au_tracker_.is_open_irap   = false;
      au_tracker_.track_id       = t;
      au_tracker_.generation     = config_tracker.current_generation();
      packet.flags               = static_cast<uint8_t>(packet.flags | memory::pkt::kAuStart);
    } else {
      if (packet.sequence != static_cast<uint16_t>(au_tracker_.last_seq + 1u)) {
        au_tracker_.has_gap = true;
      }
      au_tracker_.last_seq = packet.sequence;
    }

    /// 3. Gated parameter-set inspection + HEVC FU completeness tracking
    const auto codec_pl = extract_codec_payload(packet);
    if (!codec_pl.empty()) {
      if (codec == media::VideoCodec::H265) {
        inspect_hevc_fu_state(codec_pl, has_donl, packet.sequence);
      }
      if (packet.is_param_set() || !packet.is_vcl()) {
        inspect_parameter_sets(packet, codec, has_donl);
      }
    }

    /// 4. Accumulate AU IRAP metadata (only LayerId-0 IRAPs have kClosedIrap/kOpenIrap set)
    au_tracker_.au_bytes += packet.length;
    if (packet.is_idr()) {
      au_tracker_.is_irap        = true;
      au_tracker_.is_closed_irap = true;
    }
    if (packet.is_open_rap()) {
      au_tracker_.is_irap      = true;
      au_tracker_.is_open_irap = true;
    }

    packet.generation_id = config_tracker.current_generation();
    const bool marker    = packet.is_marker();

    /// 5. Append to PacketRing and stage for EgressReactors
    packet_log.ring().append(packet);
    egress_batch.push_back(packet);

    if (marker) {
      finalize_current_au(true);
      egress_batch.back().generation_id = config_tracker.current_generation();
    }

    if (au_assembly_enabled_) {
      return assembler.on_packet(std::move(packet), marker);
    }
    return std::nullopt;
  }

  std::optional<media::AccessUnitView> handle_video_packet(memory::PacketRef packet,
      uint8_t                                                                trk = 0,
      media::VideoCodec codec    = media::VideoCodec::H264,
      bool              has_donl = false) {
    const uint8_t t             = static_cast<uint8_t>(trk % kMaxTracks);
    const bool    was_probation = seq_trackers_[t].is_in_probation();
    auto          ext_seq       = seq_trackers_[t].update(packet.sequence);
    if (!ext_seq.has_value()) {
      if (was_probation) {
        probation_hold_[t] = std::move(packet);
      }
      metrics_.sequence_duplicates_or_probation.fetch_add(1, std::memory_order_relaxed);
      return std::nullopt;
    }

    if (was_probation && probation_hold_[t] &&
        probation_hold_[t].sequence == static_cast<uint16_t>(packet.sequence - 1U)) {
      auto           first_pkt = std::move(probation_hold_[t]);
      const uint64_t first_ext = (*ext_seq > 0ULL) ? (*ext_seq - 1ULL) : 0ULL;
      (void)accept_validated_video(std::move(first_pkt), t, first_ext, codec, has_donl);
    } else {
      probation_hold_[t] = memory::PacketRef{};
    }

    return accept_validated_video(std::move(packet), t, *ext_seq, codec, has_donl);
  }

  /// Inspects RTP payload (never the 12-byte RTP header!) for VPS/SPS/PPS
  void inspect_parameter_sets(const memory::PacketRef &packet,
      media::VideoCodec                                codec    = media::VideoCodec::H264,
      bool                                             has_donl = false) {
    const auto span = extract_codec_payload(packet);
    if (span.empty()) {
      return;
    }

    if (codec == media::VideoCodec::H265) {
      media::hevc::for_each_parameter_set(span.data(),
          span.size(),
          has_donl,
          [&](uint8_t nal_type, const uint8_t *ps_ptr, size_t ps_len) {
            if (nal_type == 32U) {
              config_tracker.stage_vps(ps_ptr, ps_len);
            } else if (nal_type == 33U) {
              config_tracker.stage_sps(ps_ptr, ps_len);
            } else if (nal_type == 34U) {
              config_tracker.stage_pps(ps_ptr, ps_len);
            }
          });
    } else {
      media::H264RapClassifier::for_each_parameter_set(span.data(),
          span.size(),
          [&](media::H264NalType type, const uint8_t *ps_ptr, size_t ps_len) {
            if (type == media::H264NalType::SPS) {
              config_tracker.stage_sps(ps_ptr, ps_len);
            } else if (type == media::H264NalType::PPS) {
              config_tracker.stage_pps(ps_ptr, ps_len);
            }
          });
    }
  }

  /// Zero-allocation batch view + clear: keeps `egress_batch.capacity()`.
  [[nodiscard]] const std::vector<memory::PacketRef> &peek_batch() const noexcept {
    return egress_batch;
  }
  void clear_batch() noexcept { egress_batch.clear(); }

  /// Legacy batch publisher
  std::vector<memory::PacketRef> publish_batch() {
    std::vector<memory::PacketRef> out;
    out.reserve(egress_batch.size());
    for (auto &p : egress_batch) {
      out.push_back(std::move(p));
    }
    egress_batch.clear();
    return out;
  }

  [[nodiscard]] const media::RetainedPacketLog &get_packet_log() const noexcept {
    return packet_log;
  }
  [[nodiscard]] media::RetainedPacketLog &get_packet_log() noexcept { return packet_log; }

  [[nodiscard]] const media::ConfigTracker &get_config_tracker() const noexcept {
    return config_tracker;
  }
  [[nodiscard]] media::ConfigTracker &get_config_tracker() noexcept { return config_tracker; }

  void run() noexcept {
    is_running.store(true, std::memory_order_release);
    RIVET_LOG_INFO("IngestReactor", "IngestReactor active.");
  }

  void stop() noexcept {
    is_running.store(false, std::memory_order_release);
    for (auto &held : probation_hold_) {
      held = memory::PacketRef{};
    }
    egress_batch.clear();
    packet_log.clear();
    au_tracker_.reset();
    hevc_fu_param_.reset();
    assembler.reset();
  }

  [[nodiscard]] bool is_active() const noexcept {
    return is_running.load(std::memory_order_acquire);
  }
};

} //< namespace rivet::core
