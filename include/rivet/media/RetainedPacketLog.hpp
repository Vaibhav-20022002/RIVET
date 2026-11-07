#pragma once

#include "../memory/PacketRef.hpp"
#include "PacketRing.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace rivet::media {

/// RetainedPacketLog wraps PacketRing with byte-budgeted zero-allocation retention and
/// race-free RAP-to-live-edge snapshot helpers.
class RetainedPacketLog {
  static constexpr size_t kDefaultSlots       = 16384;
  static constexpr size_t kDefaultBudgetBytes = 16ULL * 1024ULL * 1024ULL;
  static constexpr size_t kDefaultBurstBytes  = 4ULL * 1024ULL * 1024ULL;

  PacketRing ring_;
  bool       has_last_direct_rap_ts_{false};
  uint32_t   last_direct_rap_ts_{0};

public:
  explicit RetainedPacketLog(size_t slot_capacity          = kDefaultSlots,
      size_t                        retention_budget_bytes = kDefaultBudgetBytes)
      : ring_(slot_capacity, retention_budget_bytes) {}

  [[nodiscard]] PacketRing       &ring() noexcept { return ring_; }
  [[nodiscard]] const PacketRing &ring() const noexcept { return ring_; }

  /// Appends a packet directly (used by unit tests & standalone callers).
  /// Note: IngestReactor uses `ring().append()` + `ring().record_rap()` for strict whole-AU RAPs.
  uint64_t append(memory::PacketRef packet) noexcept {
    const bool     is_rap      = packet.is_rap();
    const bool     is_open     = packet.is_open_rap() && !packet.is_idr();
    const uint32_t ts          = packet.rtp_timestamp;
    const uint32_t len         = packet.length;
    const uint8_t  trk         = packet.track();
    const uint32_t gen         = packet.generation_id;
    const uint64_t byte_before = ring_.write_bytes();

    const uint64_t pos = ring_.append(std::move(packet));
    if (is_rap && (!has_last_direct_rap_ts_ || last_direct_rap_ts_ != ts)) {
      has_last_direct_rap_ts_ = true;
      last_direct_rap_ts_     = ts;
      RapEntry entry{};
      entry.pos        = pos;
      entry.byte_pos   = byte_before;
      entry.rtp_ts     = ts;
      entry.generation = gen;
      entry.au_bytes   = len;
      entry.track_id   = trk;
      entry.open_rap   = is_open;
      entry.valid      = true;
      ring_.record_rap(entry);
    }
    return pos;
  }

  /// Finds the RTP sequence number of the latest retained RAP, if still in retention window.
  [[nodiscard]] std::optional<uint16_t> find_latest_rap_sequence() const noexcept {
    auto rap = ring_.find_latest_rap();
    if (!rap) {
      return std::nullopt;
    }
    auto pkt = ring_.try_get(rap->pos);
    if (!pkt) {
      return std::nullopt;
    }
    return pkt->sequence;
  }

  /// Extracts all retained packets from `start_pos` up to `live_pos`, bounded only by byte budget
  /// (never artificially truncated at 128 packets).
  [[nodiscard]] std::vector<memory::PacketRef> snapshot_range(uint64_t start_pos,
      size_t max_burst_bytes = kDefaultBurstBytes) const {
    const uint64_t head   = ring_.write_pos();
    const uint64_t oldest = ring_.oldest_pos();
    if (start_pos < oldest || start_pos >= head) {
      return {};
    }

    std::vector<memory::PacketRef> burst;
    burst.reserve(static_cast<size_t>(head - start_pos));

    size_t accumulated_bytes = 0;
    for (uint64_t pos = start_pos; pos < head; ++pos) {
      auto pkt = ring_.try_get(pos);
      if (!pkt) {
        /// Evicted mid-scan: abort partial GOP rather than returning a broken prefix
        return {};
      }
      if (max_burst_bytes != 0 && !burst.empty() &&
          (accumulated_bytes + pkt->length) > max_burst_bytes) {
        /// Byte budget exceeded before reaching live edge
        return {};
      }
      accumulated_bytes += pkt->length;
      burst.push_back(std::move(*pkt));
    }
    return burst;
  }

  /// Single-call race-free RAP selection + snapshot.
  [[nodiscard]] std::vector<memory::PacketRef> snapshot_from_latest_rap(
      size_t   burst_budget_bytes  = kDefaultBurstBytes,
      uint32_t required_generation = 0,
      bool     allow_open_irap     = true,
      uint8_t  track_id            = 0) const {
    auto rap =
        ring_.find_best_rap(track_id, required_generation, burst_budget_bytes, allow_open_irap);
    if (!rap) {
      return {};
    }
    return snapshot_range(rap->pos, burst_budget_bytes);
  }

  /// Extracts packets starting at `start_sequence` up to live edge (no 128-packet truncation).
  [[nodiscard]] std::vector<memory::PacketRef> snapshot_from(uint16_t start_sequence,
      size_t max_burst_bytes = kDefaultBurstBytes) const {
    /// Fast path: check if latest RAP matches `start_sequence`
    if (auto rap = ring_.find_latest_rap()) {
      if (auto first = ring_.try_get(rap->pos); first && first->sequence == start_sequence) {
        return snapshot_range(rap->pos, max_burst_bytes);
      }
    }

    const uint64_t oldest = ring_.oldest_pos();
    const uint64_t head   = ring_.write_pos();
    for (uint64_t pos = head; pos > oldest; --pos) {
      const uint64_t candidate = pos - 1;
      auto           pkt       = ring_.try_get(candidate);
      if (!pkt) {
        break;
      }
      if (pkt->sequence == start_sequence) {
        return snapshot_range(candidate, max_burst_bytes);
      }
    }
    return {};
  }

  void clear() noexcept {
    has_last_direct_rap_ts_ = false;
    last_direct_rap_ts_     = 0;
    ring_.clear();
  }

  [[nodiscard]] size_t size() const noexcept { return ring_.size(); }
};

} //< namespace rivet::media
