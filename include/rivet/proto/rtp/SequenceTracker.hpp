#pragma once

#include <cstdint>
#include <optional>

namespace rivet::proto::rtp {

/// Strict RTP sequence tracker (RFC 3550 Appendix A.1)
/// Extended to a 64-bit de-duplication window for high-bitrate video and rollovers.
class SequenceTracker {
  static constexpr uint16_t MAX_DROPOUT    = 3000;
  static constexpr uint16_t MAX_MISORDER   = 100;
  static constexpr uint32_t MIN_SEQUENTIAL = 2;

  uint16_t max_seq{0}; //< Max 16-bit sequence number we have seen so far
  uint64_t cycles{0};  //< Wraps (from 65536 to 0) add 65536 to this cycles variable
  uint32_t base_seq{0};
  uint32_t bad_seq{0}; //< Used to detect any anomaly (Camera restarts or crashes)
  uint32_t probation{
      MIN_SEQUENTIAL}; //< Shield against garbage. If a random packet arrives, stay in probation
                       /// until MIN_SEQUENTIAL perfectly ordered packets in a row.

  uint32_t received{0};
  uint32_t expected_prior{0};
  uint32_t received_prior{0};

  uint64_t seen_mask{0}; //< Bit 0 -> Newest pkt, Bit 1 -> pkt before that, etc.
  bool     initialized{false};

  void init_source(uint16_t seq) noexcept {
    base_seq       = seq;
    max_seq        = seq;
    bad_seq        = (seq + 1) & 0xFFFF; //< Offset so it doesn't match seq on next call
    cycles         = 0;
    received       = 0;
    expected_prior = 0;
    received_prior = 0;
    probation      = 0; //< Verified source: probation is complete!
    seen_mask      = 1; //< Mark index 0 (dist 0) as seen
    initialized    = true;
  }

public:
  SequenceTracker() noexcept = default;

  /// Returns the extended 64-bit sequence number if the packet is valid.
  /// Returns std::nullopt if the packet is a duplicate or an invalid jump.
  [[nodiscard]] std::optional<uint64_t> update(uint16_t seq) noexcept {
    if (!initialized) {
      init_source(seq);
      ++received;
      return cycles + seq;
    }

    if (probation > 0) {
      if (seq == ((max_seq + 1) & 0xFFFF)) {
        max_seq = seq;
        if (--probation == 0) {
          init_source(seq);
          ++received;
          return cycles + seq;
        }
      } else {
        probation = MIN_SEQUENTIAL - 1;
        max_seq   = seq;
      }
      return std::nullopt;
    }

    /// NOTE: Taking `udelta` as 16-bit so it can handle wrap-around calculation.
    /// If `max_seq` is 65535 and the new seq is 2, so 2-65535 underflows and becomes exactly 3.
    /// This make sures that packet is 3 step ahead.
    uint16_t udelta = seq - max_seq;
    if (udelta == 0) {
      return std::nullopt; //< Immediate duplicate!
    }
    if (udelta < MAX_DROPOUT) {
      /// In order, with allowable gap
      if (seq < max_seq) {
        cycles += (1ULL << 16);
      }

      max_seq = seq;

      /// Shift duplicate tracking mask by the gap size
      if (udelta >= 64) {
        seen_mask = 0;
      } else {
        seen_mask <<= udelta;
      }

      seen_mask |= 1; //< Mark this new packet as seen (bit 0)
    } else if (udelta <= (0xFFFF - MAX_MISORDER)) {
      /// Sequence number made a very large jump
      if (seq == bad_seq) {
        /// Two consecutive packets arrived with this large jump, assume source restarted
        init_source(seq);
      } else {
        bad_seq = (seq + 1) & 0xFFFF;
        return std::nullopt;
      }
    } else {
      /// Out of order (late packet)
      uint64_t ext_seq     = cycles + seq;
      uint64_t max_ext_seq = cycles + max_seq;

      /// Handle wraparound edge case where late packet belongs to previous cycle
      if (seq > max_seq) {
        ext_seq -= (1ULL << 16);
      }

      if (ext_seq > max_ext_seq) {
        return std::nullopt; //< Guard against monotonic failure
      }

      uint64_t dist = max_ext_seq - ext_seq;
      if (dist >= 64) {
        return std::nullopt; //< Too old for our 64-packet duplicate window
      }

      if (seen_mask & (1ULL << dist)) {
        return std::nullopt; //< Duplicate!
      }

      seen_mask |= (1ULL << dist);
    }

    ++received;
    return cycles + seq;
  }
};

} //< namespace rivet::proto::rtp