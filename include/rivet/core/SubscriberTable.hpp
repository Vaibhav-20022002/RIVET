#pragma once

#include "SubscriberSession.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define RIVET_HAS_NEON 1
#elif defined(__AVX2__)
#include <immintrin.h>
#define RIVET_HAS_AVX2 1
#endif

namespace rivet::core {

/// SIMD-accelerated Structure-of-Arrays (SoA) per-track subscriber table.
/// Partitions subscribers by `track_id` (`0..7`) so dispatching a Video packet never touches Audio
/// subscribers (and never miscounts track mismatches as dropped packets), while contiguous 1-byte
/// `eligible_mask` + `fill_pct` vectors allow NEON/AVX2 16–32x parallel pre-filtering.
class SubscriberTable {
public:
  static constexpr size_t kMaxTracks = 8;

  struct TrackGroup {
    std::vector<std::unique_ptr<SubscriberSession>> sessions;
    std::vector<uint8_t> active_mask; //< 1 if active && !quiesced && playing
    std::vector<uint8_t> fill_pct;    //< 0..100 cached fill percentage

    void sync_slot(size_t idx, uint32_t max_recent_irap_bytes = 0) noexcept {
      const auto &s    = *sessions[idx];
      active_mask[idx] = (s.active() && !s.quiesced() && s.is_playing() && !s.in_startup_burst())
          ? uint8_t{1}
          : uint8_t{0};
      fill_pct[idx]    = s.compute_fill_pct(max_recent_irap_bytes);
    }
  };

private:
  struct SessionLoc {
    uint8_t track{0};
    size_t  index{0};
  };

  std::array<TrackGroup, kMaxTracks>       tracks_{};
  std::unordered_map<uint64_t, SessionLoc> id_to_loc_;
  size_t                                   total_count_{0};

public:
  SubscriberTable() = default;

  /// Reference scalar eligibility filter: outputs indices `i` where
  /// `active_mask[i] != 0 && fill_pct[i] < fill_limit`.
  static size_t filter_eligible_scalar(const uint8_t *active_mask,
      const uint8_t                                  *fill_pct,
      size_t                                          count,
      uint8_t                                         fill_limit,
      uint32_t                                       *out_indices) noexcept {
    size_t out_n = 0;
    for (size_t i = 0; i < count; ++i) {
      if (active_mask[i] != 0 && fill_pct[i] < fill_limit) {
        out_indices[out_n++] = static_cast<uint32_t>(i);
      }
    }
    return out_n;
  }

  /// SIMD-accelerated eligibility filter (`ARM_NEON` 16-wide / `AVX2` 32-wide / scalar tail).
  /// Bit-exact with `filter_eligible_scalar`.
  static size_t filter_eligible_simd(const uint8_t *active_mask,
      const uint8_t                                *fill_pct,
      size_t                                        count,
      uint8_t                                       fill_limit,
      uint32_t                                     *out_indices) noexcept {
    size_t out_n = 0;
    size_t i     = 0;

#if defined(RIVET_HAS_NEON)
    const uint8x16_t v_zero  = vdupq_n_u8(0);
    const uint8x16_t v_limit = vdupq_n_u8(fill_limit);

    for (; i + 16 <= count; i += 16) {
      const uint8x16_t v_act   = vld1q_u8(active_mask + i);
      const uint8x16_t v_fill  = vld1q_u8(fill_pct + i);
      const uint8x16_t m_act   = vcgtq_u8(v_act, v_zero);
      const uint8x16_t m_fill  = vcltq_u8(v_fill, v_limit);
      const uint8x16_t m_valid = vandq_u8(m_act, m_fill);

      /// Fast skip when all 16 lanes are ineligible
      const uint64x2_t pairs = vreinterpretq_u64_u8(m_valid);
      const uint64_t   lo    = vgetq_lane_u64(pairs, 0);
      const uint64_t   hi    = vgetq_lane_u64(pairs, 1);
      if ((lo | hi) == 0) {
        continue;
      }

      alignas(16) uint8_t lanes[16];
      vst1q_u8(lanes, m_valid);
      for (size_t k = 0; k < 16; ++k) {
        if (lanes[k] != 0) {
          out_indices[out_n++] = static_cast<uint32_t>(i + k);
        }
      }
    }
#elif defined(RIVET_HAS_AVX2)
    const __m256i v_zero      = _mm256_setzero_si256();
    const __m256i v_sign_flip = _mm256_set1_epi8(static_cast<char>(0x80));
    const __m256i v_limit_s =
        _mm256_xor_si256(_mm256_set1_epi8(static_cast<char>(fill_limit)), v_sign_flip);

    for (; i + 32 <= count; i += 32) {
      const __m256i v_act  = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(active_mask + i));
      const __m256i v_fill = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(fill_pct + i));
      const __m256i m_act  = _mm256_xor_si256(
          _mm256_cmpeq_epi8(v_act, v_zero), _mm256_set1_epi8(static_cast<char>(0xFF)));
      const __m256i v_fills = _mm256_xor_si256(v_fill, v_sign_flip);
      const __m256i m_fill  = _mm256_cmpgt_epi8(v_limit_s, v_fills);
      const __m256i m_valid = _mm256_and_si256(m_act, m_fill);

      uint32_t mask = static_cast<uint32_t>(_mm256_movemask_epi8(m_valid));
      while (mask != 0) {
        const uint32_t bit   = static_cast<uint32_t>(__builtin_ctz(mask));
        out_indices[out_n++] = static_cast<uint32_t>(i + bit);
        mask &= (mask - 1u);
      }
    }
#endif

    /// Scalar tail
    for (; i < count; ++i) {
      if (active_mask[i] != 0 && fill_pct[i] < fill_limit) {
        out_indices[out_n++] = static_cast<uint32_t>(i);
      }
    }
    return out_n;
  }

  bool insert(std::unique_ptr<SubscriberSession> session, uint32_t max_recent_irap_bytes = 0) {
    if (!session) {
      return false;
    }
    const uint64_t sid = session->get_session_id();
    if (id_to_loc_.find(sid) != id_to_loc_.end()) {
      return false;
    }

    const uint8_t trk = static_cast<uint8_t>(session->get_track_id() % kMaxTracks);
    auto         &grp = tracks_[trk];
    const size_t  idx = grp.sessions.size();

    grp.sessions.push_back(std::move(session));
    grp.active_mask.push_back(0);
    grp.fill_pct.push_back(0);
    grp.sync_slot(idx, max_recent_irap_bytes);

    id_to_loc_[sid] = SessionLoc{trk, idx};
    ++total_count_;
    return true;
  }

  /// Removes subscriber in O(1) via swap-and-pop, returning the removed session for cleanup.
  std::unique_ptr<SubscriberSession> erase(uint64_t session_id) {
    auto it = id_to_loc_.find(session_id);
    if (it == id_to_loc_.end()) {
      return nullptr;
    }

    const uint8_t trk      = it->second.track;
    const size_t  idx      = it->second.index;
    auto         &grp      = tracks_[trk];
    const size_t  last_idx = grp.sessions.size() - 1;

    std::unique_ptr<SubscriberSession> removed = std::move(grp.sessions[idx]);
    if (idx != last_idx) {
      const uint64_t moved_id = grp.sessions[last_idx]->get_session_id();
      grp.sessions[idx]       = std::move(grp.sessions[last_idx]);
      grp.active_mask[idx]    = grp.active_mask[last_idx];
      grp.fill_pct[idx]       = grp.fill_pct[last_idx];
      id_to_loc_[moved_id]    = SessionLoc{trk, idx};
    }

    grp.sessions.pop_back();
    grp.active_mask.pop_back();
    grp.fill_pct.pop_back();
    id_to_loc_.erase(it);
    --total_count_;
    return removed;
  }

  [[nodiscard]] SubscriberSession *find(uint64_t session_id) noexcept {
    auto it = id_to_loc_.find(session_id);
    if (it == id_to_loc_.end()) {
      return nullptr;
    }
    return tracks_[it->second.track].sessions[it->second.index].get();
  }

  [[nodiscard]] const SubscriberSession *find(uint64_t session_id) const noexcept {
    auto it = id_to_loc_.find(session_id);
    if (it == id_to_loc_.end()) {
      return nullptr;
    }
    return tracks_[it->second.track].sessions[it->second.index].get();
  }

  [[nodiscard]] TrackGroup &track_group(uint8_t track_id) noexcept {
    return tracks_[track_id % kMaxTracks];
  }

  [[nodiscard]] const TrackGroup &track_group(uint8_t track_id) const noexcept {
    return tracks_[track_id % kMaxTracks];
  }

  void sync_slot(uint8_t track_id, size_t idx, uint32_t max_recent_irap_bytes = 0) noexcept {
    auto &grp = tracks_[track_id % kMaxTracks];
    if (idx < grp.sessions.size()) {
      grp.sync_slot(idx, max_recent_irap_bytes);
    }
  }

  void clear() noexcept {
    for (auto &grp : tracks_) {
      for (auto &s : grp.sessions) {
        if (s) {
          s->mark_closed();
        }
      }
      grp.sessions.clear();
      grp.active_mask.clear();
      grp.fill_pct.clear();
    }
    id_to_loc_.clear();
    total_count_ = 0;
  }

  [[nodiscard]] size_t size() const noexcept { return total_count_; }
  [[nodiscard]] bool   empty() const noexcept { return total_count_ == 0; }

  template<typename Fn> void for_each(Fn &&fn) {
    for (auto &grp : tracks_) {
      for (auto &s : grp.sessions) {
        if (s) {
          fn(*s);
        }
      }
    }
  }

  /// Finds the laggiest subscriber (largest `queued_bytes`) across all tracks for fair quota
  /// eviction.
  [[nodiscard]] uint64_t find_laggiest_session_id() const noexcept {
    uint64_t worst_id    = 0;
    size_t   worst_bytes = 0;
    bool     found       = false;
    for (const auto &grp : tracks_) {
      for (const auto &s : grp.sessions) {
        if (!s) {
          continue;
        }
        const size_t qb = s->get_queued_bytes();
        if (!found || qb > worst_bytes) {
          found       = true;
          worst_bytes = qb;
          worst_id    = s->get_session_id();
        }
      }
    }
    return found ? worst_id : 0;
  }
};

} //< namespace rivet::core
