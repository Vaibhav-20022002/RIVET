#pragma once

#include <cstdint>
#include <optional>

namespace rivet::proto::rtp {

/// Strict RTP sequence tracker (RFC 3550 Appendix A.1):
///   - Enforces `MIN_SEQUENTIAL = 2` source probation via `validate_source()` separate from
///     `init_source()`.
///   - Uses `BAD_SEQ_SENTINEL = RTP_SEQ_MOD + 1` (`65537`) so a stale `seq == 1` packet never
///     falsely matches an uninitialized `bad_seq`.
///   - Preserves monotonic `cycles` / `max_ext_seq` across stream restarts (never resets `cycles`
///     to 0).
///   - Uses a 128-bit sliding deduplication window (`seen_lo_`, `seen_hi_`) covering the full
///     `MAX_MISORDER = 100` packet reordering window.
class SequenceTracker {
public:
  static constexpr uint32_t RTP_SEQ_MOD      = (1U << 16U);
  static constexpr uint32_t BAD_SEQ_SENTINEL = RTP_SEQ_MOD + 1U; //< 65537
  static constexpr uint16_t MAX_DROPOUT      = 3000;
  static constexpr uint16_t MAX_MISORDER     = 100;
  static constexpr uint32_t MIN_SEQUENTIAL   = 2;

private:
  uint16_t max_seq_{0};
  uint64_t cycles_{0};
  uint64_t max_ext_seq_{0};
  uint32_t base_seq_{0};
  uint32_t bad_seq_{BAD_SEQ_SENTINEL};
  uint32_t probation_{MIN_SEQUENTIAL};

  uint32_t received_{0};
  uint32_t expected_prior_{0};
  uint32_t received_prior_{0};

  /// 128-bit sliding deduplication window: bit `d` (0..127) == 1 iff `(max_ext_seq_ - d)` was seen
  uint64_t seen_lo_{0};
  uint64_t seen_hi_{0};
  bool     initialized_{false};
  bool     resynced_last_{false};

  void shift_seen_window(uint64_t diff) noexcept {
    if (diff >= 128ULL) {
      seen_lo_ = 1ULL;
      seen_hi_ = 0ULL;
    } else if (diff >= 64ULL) {
      seen_hi_ = (diff == 64ULL) ? seen_lo_ : (seen_lo_ << (diff - 64ULL));
      seen_lo_ = 1ULL;
    } else if (diff > 0ULL) {
      seen_hi_ = (seen_hi_ << diff) | (seen_lo_ >> (64ULL - diff));
      seen_lo_ = (seen_lo_ << diff) | 1ULL;
    } else {
      seen_lo_ |= 1ULL;
    }
  }

  [[nodiscard]] bool is_seen(uint64_t dist) const noexcept {
    if (dist >= 128ULL) return true; //< Older than 128-packet window -> reject as stale
    if (dist >= 64ULL) {
      return (seen_hi_ & (1ULL << (dist - 64ULL))) != 0ULL;
    }
    return (seen_lo_ & (1ULL << dist)) != 0ULL;
  }

  void mark_seen(uint64_t dist) noexcept {
    if (dist >= 128ULL) return;
    if (dist >= 64ULL) {
      seen_hi_ |= (1ULL << (dist - 64ULL));
    } else {
      seen_lo_ |= (1ULL << dist);
    }
  }

  void validate_source(uint16_t seq) noexcept {
    max_seq_     = seq;
    probation_   = MIN_SEQUENTIAL - 1U;
    bad_seq_     = BAD_SEQ_SENTINEL;
    initialized_ = true;
  }

  void init_source(uint16_t seq) noexcept {
    /// Guarantee monotonic extended sequence numbers across mid-stream restarts
    if (received_ > 0U && (cycles_ + seq) <= max_ext_seq_) {
      const uint64_t needed_cycles =
          ((max_ext_seq_ - seq) / RTP_SEQ_MOD + 1ULL) * static_cast<uint64_t>(RTP_SEQ_MOD);
      if (needed_cycles > cycles_) {
        cycles_ = needed_cycles;
      }
    }
    base_seq_       = seq;
    max_seq_        = seq;
    bad_seq_        = BAD_SEQ_SENTINEL;
    probation_      = 0U;
    max_ext_seq_    = cycles_ + seq;
    seen_lo_        = 0b11ULL; //< Current seq and preceding probation packet are both seen
    seen_hi_        = 0ULL;
    expected_prior_ = 0U;
    received_prior_ = 0U;
  }

public:
  SequenceTracker() noexcept = default;

  void reset() noexcept { *this = SequenceTracker{}; }

  [[nodiscard]] bool is_in_probation() const noexcept { return !initialized_ || probation_ > 0U; }
  [[nodiscard]] bool did_resync_on_last() const noexcept { return resynced_last_; }
  [[nodiscard]] uint32_t received_count() const noexcept { return received_; }
  [[nodiscard]] uint64_t highest_extended_seq() const noexcept { return max_ext_seq_; }

  /// Returns the extended 64-bit sequence number if the packet is valid and post-probation.
  /// Returns std::nullopt during probation, on duplicate packets, or on unconfirmed jumps.
  [[nodiscard]] std::optional<uint64_t> update(uint16_t seq) noexcept {
    resynced_last_ = false;

    if (!initialized_) {
      validate_source(seq);
      if (probation_ == 0U) {
        init_source(seq);
        ++received_;
        return max_ext_seq_;
      }
      return std::nullopt;
    }

    if (probation_ > 0U) {
      if (seq == static_cast<uint16_t>(max_seq_ + 1U)) {
        if (seq < max_seq_) {
          cycles_ += RTP_SEQ_MOD;
        }
        max_seq_ = seq;
        if (--probation_ == 0U) {
          init_source(seq);
          received_ += MIN_SEQUENTIAL;
          return max_ext_seq_;
        }
      } else {
        probation_ = MIN_SEQUENTIAL - 1U;
        max_seq_   = seq;
      }
      return std::nullopt;
    }

    const uint16_t udelta = static_cast<uint16_t>(seq - max_seq_);
    if (udelta == 0U) {
      return std::nullopt; //< Immediate duplicate
    }

    uint64_t ext_seq = 0;

    if (udelta < MAX_DROPOUT) {
      /// In order, or forward jump within permissible dropout
      if (seq < max_seq_) {
        cycles_ += RTP_SEQ_MOD;
      }
      max_seq_            = seq;
      ext_seq             = cycles_ + seq;
      const uint64_t diff = ext_seq - max_ext_seq_;
      shift_seen_window(diff);
      max_ext_seq_ = ext_seq;
    } else if (udelta <= (RTP_SEQ_MOD - MAX_MISORDER)) {
      /// Large sequence jump: require two consecutive packets before resynchronizing
      if (bad_seq_ != BAD_SEQ_SENTINEL && seq == static_cast<uint16_t>(bad_seq_)) {
        init_source(seq);
        resynced_last_ = true;
        ++received_;
        return max_ext_seq_;
      }
      bad_seq_ = (static_cast<uint32_t>(seq) + 1U) & (RTP_SEQ_MOD - 1U);
      return std::nullopt;
    } else {
      /// Delayed / out-of-order packet within `MAX_MISORDER` (100 packets)
      if (seq > max_seq_) {
        if (cycles_ < RTP_SEQ_MOD) {
          return std::nullopt; //< Precedes initial cycle 0
        }
        ext_seq = (cycles_ - RTP_SEQ_MOD) + seq;
      } else {
        ext_seq = cycles_ + seq;
      }

      if (ext_seq > max_ext_seq_) {
        return std::nullopt;
      }

      const uint64_t dist = max_ext_seq_ - ext_seq;
      if (dist > MAX_MISORDER || is_seen(dist)) {
        return std::nullopt;
      }
      mark_seen(dist);
    }

    ++received_;
    return ext_seq;
  }
};

} //< namespace rivet::proto::rtp
