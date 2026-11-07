#pragma once

#include "../memory/PacketRef.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace rivet::media {

/// Metadata indexing a complete, clean Random Access Point (IRAP) Access Unit in PacketRing.
struct RapEntry {
  uint64_t pos{0};          //< Monotonic slot position of first packet in IRAP AU (pkt::kAuStart)
  uint64_t byte_pos{0};     //< Cumulative bytes written prior to `pos`
  uint32_t rtp_ts{0};       //< RTP timestamp of the IRAP AU
  uint32_t generation{0};   //< Active ConfigGeneration at this IRAP AU
  uint32_t au_bytes{0};     //< Total byte size of the IRAP AU
  uint8_t  track_id{0};     //< Track index (video track)
  bool     open_rap{false}; //< true for Open-GOP IRAP (CRA / BLA_W_LP -> requires RASL gate)
  bool     valid{false};
};

/// Single-writer (`IngestReactor`), multi-reader (`EgressReactor` shards) zero-allocation ring.
/// Uses Dekker store-load fencing between per-shard `ReaderPin` and `oldest_pos_` so readers are
/// lock-free and wait-free, while the writer never mutates a slot concurrently being copied.
class PacketRing {
public:
  static constexpr size_t   kMaxReaders     = 32;
  static constexpr size_t   kSharedReaderId = kMaxReaders - 1;
  static constexpr size_t   kRapHistorySize = 16;
  static constexpr uint64_t kUnpinned       = std::numeric_limits<uint64_t>::max();

private:
  struct alignas(64) ReaderPin {
    std::atomic<uint64_t> pos{kUnpinned};
  };

  static constexpr size_t round_up_pow2(size_t n) noexcept {
    if (n < 16) {
      n = 16;
    }
    size_t p = 16;
    while (p < n && p < (size_t{1} << 30)) {
      p <<= 1;
    }
    return p;
  }

  static void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
  }

  size_t                         capacity_;
  size_t                         mask_;
  size_t                         retention_budget_bytes_;
  std::vector<memory::PacketRef> slots_;

  alignas(64) std::atomic<uint64_t> write_pos_{0};
  alignas(64) std::atomic<uint64_t> oldest_pos_{0};
  alignas(64) std::atomic<uint64_t> write_bytes_{0};
  alignas(64) std::atomic<size_t> retained_bytes_{0};

  std::array<ReaderPin, kMaxReaders> pins_{};
  mutable std::mutex                 shared_reader_mutex_;

  mutable std::mutex                    rap_mutex_;
  std::array<RapEntry, kRapHistorySize> raps_{};
  size_t                                rap_write_idx_{0};
  uint32_t                              max_recent_irap_bytes_{0};

  void evict_oldest() noexcept {
    const uint64_t evict_pos = oldest_pos_.load(std::memory_order_relaxed);
    const uint64_t head      = write_pos_.load(std::memory_order_relaxed);
    if (evict_pos >= head) {
      return;
    }

    /// Publish new floor with sequential consistency BEFORE checking reader pins (Dekker fence).
    oldest_pos_.store(evict_pos + 1, std::memory_order_seq_cst);

    for (size_t r = 0; r < kMaxReaders; ++r) {
      while (pins_[r].pos.load(std::memory_order_seq_cst) == evict_pos) {
        cpu_relax();
      }
    }

    auto        &slot = slots_[evict_pos & mask_];
    const size_t len  = slot.length;
    slot              = memory::PacketRef{};

    const size_t cur = retained_bytes_.load(std::memory_order_relaxed);
    retained_bytes_.store(cur >= len ? (cur - len) : 0, std::memory_order_relaxed);
  }

  [[nodiscard]] std::optional<memory::PacketRef> try_get_pinned(uint64_t pos,
      size_t                                                             pin_slot) const noexcept {
    const uint64_t head = write_pos_.load(std::memory_order_acquire);
    if (pos >= head) {
      return std::nullopt;
    }

    auto &pin = const_cast<ReaderPin &>(pins_[pin_slot]);
    pin.pos.store(pos, std::memory_order_seq_cst);

    const uint64_t oldest = oldest_pos_.load(std::memory_order_seq_cst);
    if (pos < oldest) {
      pin.pos.store(kUnpinned, std::memory_order_release);
      return std::nullopt;
    }

    memory::PacketRef copy = slots_[pos & mask_];
    pin.pos.store(kUnpinned, std::memory_order_release);
    return copy;
  }

public:
  explicit PacketRing(size_t slot_capacity          = 16384,
      size_t                 retention_budget_bytes = 16ULL * 1024ULL * 1024ULL)
      : capacity_(round_up_pow2(slot_capacity))
      , mask_(capacity_ - 1)
      , retention_budget_bytes_(
            retention_budget_bytes > 0 ? retention_budget_bytes : (16ULL * 1024ULL * 1024ULL))
      , slots_(capacity_) {}

  PacketRing(const PacketRing &)            = delete;
  PacketRing &operator=(const PacketRing &) = delete;

  /// Evicts one oldest retained packet on the writer thread (used to reclaim arena slabs under
  /// pressure).
  bool evict_one_oldest() noexcept {
    const uint64_t oldest = oldest_pos_.load(std::memory_order_relaxed);
    const uint64_t wpos   = write_pos_.load(std::memory_order_relaxed);
    if (oldest >= wpos) {
      return false;
    }
    evict_oldest();
    return true;
  }

  /// Appends a packet on the writer thread, evicting oldest entries if slot or byte budget requires.
  /// \return The monotonic 64-bit ring position assigned to this packet.
  uint64_t append(memory::PacketRef pkt) noexcept {
    const size_t   pkt_len = pkt.length;
    const uint64_t wpos    = write_pos_.load(std::memory_order_relaxed);

    while (true) {
      const uint64_t oldest = oldest_pos_.load(std::memory_order_relaxed);
      if (wpos == oldest) {
        break;
      }
      const bool slots_full = (wpos - oldest) >= capacity_;
      const bool bytes_full =
          (retained_bytes_.load(std::memory_order_relaxed) + pkt_len) > retention_budget_bytes_;
      if (!slots_full && !bytes_full) {
        break;
      }
      evict_oldest();
    }

    slots_[wpos & mask_] = std::move(pkt);
    retained_bytes_.store(
        retained_bytes_.load(std::memory_order_relaxed) + pkt_len, std::memory_order_relaxed);
    write_bytes_.store(
        write_bytes_.load(std::memory_order_relaxed) + pkt_len, std::memory_order_release);
    write_pos_.store(wpos + 1, std::memory_order_release);
    return wpos;
  }

  /// Registers a completed, gap-free IRAP Access Unit in the 16-entry RAP index (cold ~1 Hz path).
  void record_rap(const RapEntry &entry) noexcept {
    const uint64_t oldest = oldest_pos_.load(std::memory_order_acquire);
    if (entry.pos < oldest) {
      return; //< IRAP AU already exceeded retention window while assembling
    }
    std::lock_guard<std::mutex> lock(rap_mutex_);
    RapEntry                    stored      = entry;
    stored.valid                            = true;
    raps_[rap_write_idx_ % kRapHistorySize] = stored;
    ++rap_write_idx_;
    if (entry.au_bytes > max_recent_irap_bytes_) {
      max_recent_irap_bytes_ = entry.au_bytes;
    }
  }

  /// Lock-free wait-free read for a dedicated EgressReactor shard (`shard_id < kSharedReaderId`).
  [[nodiscard]] std::optional<memory::PacketRef> try_get_shard(uint64_t pos,
      size_t                                                            shard_id) const noexcept {
    if (shard_id >= kSharedReaderId) {
      std::lock_guard<std::mutex> lock(shared_reader_mutex_);
      return try_get_pinned(pos, kSharedReaderId);
    }
    return try_get_pinned(pos, shard_id);
  }

  /// Thread-safe read using the shared control/test reader slot.
  [[nodiscard]] std::optional<memory::PacketRef> try_get(uint64_t pos) const noexcept {
    std::lock_guard<std::mutex> lock(shared_reader_mutex_);
    return try_get_pinned(pos, kSharedReaderId);
  }

  /// Searches the 16-entry RAP ring from newest to oldest for a retained RAP matching constraints.
  [[nodiscard]] std::optional<RapEntry> find_best_rap(uint8_t track_id,
      uint32_t                                                required_generation,
      size_t                                                  burst_budget_bytes,
      bool                                                    allow_open_irap) const noexcept {
    const uint64_t oldest     = oldest_pos_.load(std::memory_order_acquire);
    const uint64_t live_bytes = write_bytes_.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> lock(rap_mutex_);
    const size_t                count = std::min(rap_write_idx_, kRapHistorySize);
    for (size_t i = 0; i < count; ++i) {
      const size_t    idx = (rap_write_idx_ - 1 - i) % kRapHistorySize;
      const RapEntry &rap = raps_[idx];
      if (!rap.valid || rap.track_id != track_id) {
        continue;
      }
      if (rap.pos < oldest) {
        continue;
      }
      if (required_generation != 0 && rap.generation != required_generation) {
        continue;
      }
      if (!allow_open_irap && rap.open_rap) {
        continue;
      }
      const uint64_t backlog = (live_bytes >= rap.byte_pos) ? (live_bytes - rap.byte_pos) : 0;
      if (burst_budget_bytes != 0 && backlog > burst_budget_bytes) {
        continue;
      }
      return rap;
    }
    return std::nullopt;
  }

  /// Returns the latest retained RAP across any track (used by legacy RetainedPacketLog API).
  [[nodiscard]] std::optional<RapEntry> find_latest_rap() const noexcept {
    const uint64_t              oldest = oldest_pos_.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(rap_mutex_);
    const size_t                count = std::min(rap_write_idx_, kRapHistorySize);
    for (size_t i = 0; i < count; ++i) {
      const size_t    idx = (rap_write_idx_ - 1 - i) % kRapHistorySize;
      const RapEntry &rap = raps_[idx];
      if (rap.valid && rap.pos >= oldest) {
        return rap;
      }
    }
    return std::nullopt;
  }

  /// Clears all retained packets and RAP indices (writer thread only, e.g. on SSRC reset/shutdown).
  void clear() noexcept {
    while (
        oldest_pos_.load(std::memory_order_relaxed) < write_pos_.load(std::memory_order_relaxed)) {
      evict_oldest();
    }
    std::lock_guard<std::mutex> lock(rap_mutex_);
    for (auto &r : raps_) {
      r = RapEntry{};
    }
    rap_write_idx_         = 0;
    max_recent_irap_bytes_ = 0;
  }

  [[nodiscard]] uint64_t write_pos() const noexcept {
    return write_pos_.load(std::memory_order_acquire);
  }

  [[nodiscard]] uint64_t oldest_pos() const noexcept {
    return oldest_pos_.load(std::memory_order_acquire);
  }

  [[nodiscard]] uint64_t write_bytes() const noexcept {
    return write_bytes_.load(std::memory_order_acquire);
  }

  [[nodiscard]] size_t retained_bytes() const noexcept {
    return retained_bytes_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] size_t size() const noexcept {
    const uint64_t h = write_pos_.load(std::memory_order_acquire);
    const uint64_t o = oldest_pos_.load(std::memory_order_acquire);
    return (h >= o) ? static_cast<size_t>(h - o) : 0;
  }

  [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

  [[nodiscard]] size_t retention_budget_bytes() const noexcept { return retention_budget_bytes_; }

  [[nodiscard]] uint32_t max_recent_irap_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(rap_mutex_);
    return max_recent_irap_bytes_;
  }
};

} //< namespace rivet::media
