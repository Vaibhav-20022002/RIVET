#pragma once

#include "H264RapClassifier.hpp"
#include "HevcPayload.hpp"
#include "StreamDescriptor.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rivet::media {

/// Immutable snapshot of active Video Parameter Sets (VPS, SPS, PPS) and derived codec metadata.
/// Tagged with a monotonic `generation_id` so cached Random Access Points in `PacketRing` stay
/// bound to the exact parameter sets active when they were encoded.
struct ConfigGeneration {
  uint32_t generation_id{0};

  /// Primary active parameter sets (kept for backwards compatibility with callers/tests)
  std::vector<uint8_t> vps;
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;

  /// Full ordered list of active keyed parameter sets (up to 16 VPS, 16 SPS, 64 PPS)
  std::vector<std::vector<uint8_t>> all_vps;
  std::vector<std::vector<uint8_t>> all_sps;
  std::vector<std::vector<uint8_t>> all_pps;

  /// Derived bitstream metadata for SDP rendering & TID-aware drop gating
  uint8_t                  highest_tid{0};
  std::optional<hevc::Ptl> hevc_ptl{};
  std::string              h264_profile_level_id{"42e01f"};
  bool                     has_h264_profile{false};
};

using ConfigSnapshot = ConfigGeneration;

/// Bounded, keyed, AU-atomic Configuration Generation Tracker:
///   - Keyed slots: 16 VPS, 16 SPS, 64 PPS.
///   - Per-AU staging: `stage_vps`/`stage_sps`/`stage_pps` + `commit_au()` produces at most ONE
///     generation per Access Unit, with zero mutexes and zero allocations on identical repeats!
///   - Bounded ring of `MAX_HISTORY = 4` immutable `ConfigSnapshot`s for concurrent DESCRIBE &
///     late-join readers.
class ConfigTracker {
public:
  static constexpr size_t MAX_VPS_SLOTS = 16;
  static constexpr size_t MAX_SPS_SLOTS = 16;
  static constexpr size_t MAX_PPS_SLOTS = 64;
  static constexpr size_t MAX_HISTORY   = 4;

private:
  /// Single-writer ingest staging state (owned by IngestReactor Thread 0)
  std::array<std::vector<uint8_t>, MAX_VPS_SLOTS> vps_slots_{};
  std::array<std::vector<uint8_t>, MAX_SPS_SLOTS> sps_slots_{};
  std::array<std::vector<uint8_t>, MAX_PPS_SLOTS> pps_slots_{};
  uint8_t                                         last_vps_id_{0};
  uint8_t                                         last_sps_id_{0};
  uint8_t                                         last_pps_id_{0};
  bool                                            dirty_{false};
  VideoCodec                                      codec_{VideoCodec::H264};

  /// Bounded ring of published immutable snapshots (protected only on rare publish/read)
  mutable std::mutex                                               publish_mutex_;
  std::array<std::shared_ptr<const ConfigGeneration>, MAX_HISTORY> ring_{};
  size_t                                                           ring_count_{0};
  size_t                                                           ring_head_{0};
  std::atomic<uint32_t>                                            active_gen_id_{0};

  [[nodiscard]] static bool
  slot_equals(const std::vector<uint8_t> &slot, const uint8_t *data, size_t len) noexcept {
    return slot.size() == len && std::equal(data, data + len, slot.begin());
  }

  uint32_t publish_staged_locked(uint32_t next_id) {
    auto snap           = std::make_shared<ConfigGeneration>();
    snap->generation_id = next_id;

    if (!vps_slots_[last_vps_id_].empty()) snap->vps = vps_slots_[last_vps_id_];
    if (!sps_slots_[last_sps_id_].empty()) snap->sps = sps_slots_[last_sps_id_];
    if (!pps_slots_[last_pps_id_].empty()) snap->pps = pps_slots_[last_pps_id_];

    std::optional<hevc::Ptl> vps_fallback_ptl{};
    for (const auto &v : vps_slots_) {
      if (!v.empty()) {
        if (snap->vps.empty()) snap->vps = v;
        if (!vps_fallback_ptl) {
          if (auto vi = hevc::parse_vps_info(v)) {
            vps_fallback_ptl = vi->ptl;
          }
        }
        snap->all_vps.push_back(v);
      }
    }

    for (const auto &s : sps_slots_) {
      if (!s.empty()) {
        if (snap->sps.empty()) snap->sps = s;
        if (auto ptl = hevc::parse_sps_ptl(s)) {
          snap->hevc_ptl = *ptl;
          snap->highest_tid =
              (ptl->max_sub_layers > 0U) ? static_cast<uint8_t>(ptl->max_sub_layers - 1U) : 0U;
        } else if (auto h264_info = parse_h264_sps_info(s)) {
          snap->h264_profile_level_id = h264_info->profile_level_id_hex;
          snap->has_h264_profile      = true;
        }
        snap->all_sps.push_back(s);
      }
    }

    if (!snap->hevc_ptl && vps_fallback_ptl) {
      snap->hevc_ptl    = *vps_fallback_ptl;
      snap->highest_tid = (vps_fallback_ptl->max_sub_layers > 0U)
          ? static_cast<uint8_t>(vps_fallback_ptl->max_sub_layers - 1U)
          : 0U;
    }

    for (const auto &p : pps_slots_) {
      if (!p.empty()) {
        if (snap->pps.empty()) snap->pps = p;
        snap->all_pps.push_back(p);
      }
    }

    ring_[ring_head_] = std::move(snap);
    ring_head_        = (ring_head_ + 1U) % MAX_HISTORY;
    if (ring_count_ < MAX_HISTORY) {
      ++ring_count_;
    }
    dirty_ = false;
    active_gen_id_.store(next_id, std::memory_order_release);
    return next_id;
  }

public:
  ConfigTracker() {
    auto init   = std::make_shared<ConfigGeneration>();
    ring_[0]    = std::move(init);
    ring_head_  = 1U;
    ring_count_ = 1U;
    active_gen_id_.store(0U, std::memory_order_relaxed);
  }

  void set_codec(VideoCodec codec) noexcept { codec_ = codec; }

  /// Stages a VPS NAL unit on the ingest thread without creating a partial generation.
  void stage_vps(const uint8_t *data, size_t length) {
    if (!data || length == 0U) return;
    const uint8_t id =
        hevc::parse_vps_id(std::span<const uint8_t>{data, length}).value_or(uint8_t{0}) %
        MAX_VPS_SLOTS;
    last_vps_id_ = id;
    if (slot_equals(vps_slots_[id], data, length)) {
      return;
    }
    vps_slots_[id].assign(data, data + length);
    dirty_ = true;
  }

  /// Stages an SPS NAL unit (keyed by HEVC or H.264 `sps_id`).
  void stage_sps(const uint8_t *data, size_t length) {
    if (!data || length == 0U) return;
    std::span<const uint8_t> sp{data, length};
    uint8_t                  id = 0U;
    if (codec_ == VideoCodec::H265 ||
        (length >= 2U && ((data[0] >> 1U) & 0x3FU) == 33U && (data[0] & 0x1FU) != 7U)) {
      if (auto ptl = hevc::parse_sps_ptl(sp)) {
        id = ptl->sps_id;
      }
    } else if (auto h264_sps = parse_h264_sps_info(sp)) {
      id = static_cast<uint8_t>(h264_sps->sps_id % MAX_SPS_SLOTS);
    }
    id %= MAX_SPS_SLOTS;
    last_sps_id_ = id;
    if (slot_equals(sps_slots_[id], data, length)) {
      return;
    }
    sps_slots_[id].assign(data, data + length);
    dirty_ = true;
  }

  /// Stages a PPS NAL unit (keyed by HEVC or H.264 `pps_id`).
  void stage_pps(const uint8_t *data, size_t length) {
    if (!data || length == 0U) return;
    std::span<const uint8_t> sp{data, length};
    uint8_t                  id = 0U;
    if (codec_ == VideoCodec::H265 ||
        (length >= 2U && ((data[0] >> 1U) & 0x3FU) == 34U && (data[0] & 0x1FU) != 8U)) {
      id = hevc::parse_pps_id(sp).value_or(uint8_t{0});
    } else {
      id = parse_h264_pps_id(sp).value_or(uint8_t{0});
    }
    id %= MAX_PPS_SLOTS;
    last_pps_id_ = id;
    if (slot_equals(pps_slots_[id], data, length)) {
      return;
    }
    pps_slots_[id].assign(data, data + length);
    dirty_ = true;
  }

  /// Commits any parameter sets staged during the current Access Unit as a single generation.
  /// Fast path when nothing changed: zero locks, zero allocations!
  uint32_t commit_au() {
    if (!dirty_) {
      return active_gen_id_.load(std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(publish_mutex_);
    const uint32_t              next_id = active_gen_id_.load(std::memory_order_relaxed) + 1U;
    return publish_staged_locked(next_id);
  }

  /// Seeds Generation 1 from SDP `sprop-*` parameter sets at ANNOUNCE.
  uint32_t seed_from_descriptor(const StreamDescriptor &desc) {
    const TrackDescriptor *vid = desc.find_first_track(MediaKind::Video);
    if (!vid) return current_generation();

    codec_ = vid->codec;
    for (const auto &v : vid->hevc.sprop_vps) stage_vps(v.data(), v.size());
    for (const auto &s : vid->hevc.sprop_sps) stage_sps(s.data(), s.size());
    for (const auto &p : vid->hevc.sprop_pps) stage_pps(p.data(), p.size());
    for (const auto &s : vid->h264.sprop_sps) stage_sps(s.data(), s.size());
    for (const auto &p : vid->h264.sprop_pps) stage_pps(p.data(), p.size());
    return commit_au();
  }

  /// Resets parameter-set state on unannounced SSRC change
  void reset_for_new_source() {
    for (auto &v : vps_slots_) v.clear();
    for (auto &s : sps_slots_) s.clear();
    for (auto &p : pps_slots_) p.clear();
    dirty_ = true;
    (void)commit_au();
  }

  /// Immediate single-NAL update helpers (stage + commit, preserving unit-test compatibility)
  uint32_t update_vps(const uint8_t *data, size_t length) {
    stage_vps(data, length);
    return commit_au();
  }

  uint32_t update_sps(const uint8_t *data, size_t length) {
    stage_sps(data, length);
    return commit_au();
  }

  uint32_t update_pps(const uint8_t *data, size_t length) {
    stage_pps(data, length);
    return commit_au();
  }

  /// Returns a shared immutable pointer to the latest published ConfigGeneration snapshot.
  [[nodiscard]] std::shared_ptr<const ConfigGeneration> current_snapshot() const {
    std::lock_guard<std::mutex> lock(publish_mutex_);
    const size_t                latest = (ring_head_ + MAX_HISTORY - 1U) % MAX_HISTORY;
    return ring_[latest];
  }

  /// Retrieves a specific historical generation if still retained in the 4-snapshot ring.
  [[nodiscard]] std::optional<ConfigGeneration> get_generation(uint32_t generation_id) const {
    std::lock_guard<std::mutex> lock(publish_mutex_);
    for (size_t i = 0; i < ring_count_; ++i) {
      const size_t idx = (ring_head_ + MAX_HISTORY - 1U - i) % MAX_HISTORY;
      if (ring_[idx] && ring_[idx]->generation_id == generation_id) {
        return *ring_[idx];
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] bool has_generation(uint32_t generation_id) const noexcept {
    std::lock_guard<std::mutex> lock(publish_mutex_);
    for (size_t i = 0; i < ring_count_; ++i) {
      const size_t idx = (ring_head_ + MAX_HISTORY - 1U - i) % MAX_HISTORY;
      if (ring_[idx] && ring_[idx]->generation_id == generation_id) {
        return true;
      }
    }
    return false;
  }

  /// Lock-free read of the current active generation ID (1 cycle on ingest hot path)
  [[nodiscard]] uint32_t current_generation() const noexcept {
    return active_gen_id_.load(std::memory_order_acquire);
  }
};

} //< namespace rivet::media
