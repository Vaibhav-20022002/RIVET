#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace rivet::media {

/// Tracks the active Video Parameter Sets (VPS, PPS, SPS) needed to initialize a decoder.
/// NOTE: Cameras can change resolutions mid-stream. If we burst an old 1080p IDR from the
/// RetainedPacketLog, but give the client the new 4K Parameter Sets, their decoder will crash!

/// We tag each configuration with a monotonic `generation_id`. Old packets in the log are tied to
/// older generations, ensuring clients always get the mathematically correct metadata for the
/// specific video frame they are starting on.
struct ConfigGeneration {
  uint32_t generation_id{0};

  /// In Zero-Copy architectures, we normally don't copy bytes.
  /// However, Parameter Sets are tiny (usually < 100 bytes) and must be kept alive indefinitely.
  /// Therefore, we explicitly copy them into standard vectors so they survive even after the
  /// original kernel memory buffers are evicted from the MemoryPool.
  std::vector<uint8_t> vps;
  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
};

/// Thread-safe manager for historical Configuration Generations
class ConfigTracker {
  std::vector<ConfigGeneration> history;
  mutable std::mutex            tracker_mutex;

public:
  ConfigTracker() noexcept {
    /// Start with generation 0
    history.emplace_back();
  };

  /// Updates the VPS parameter set. If it is different from the current generation, a new
  /// generation is created and the generation_id increments.
  ///\return The active generation_id after the update
  uint32_t update_vps(const uint8_t *data, size_t length) {
    std::lock_guard<std::mutex> lock(tracker_mutex);
    if (history.back().vps.size() == length &&
        std::equal(data, data + length, history.back().vps.begin())) {
      return history.back().generation_id; //< Unchanged
    }

    /// Create new generation
    ConfigGeneration next_gen = history.back();
    ++next_gen.generation_id;
    next_gen.vps.assign(data, data + length);
    history.push_back(std::move(next_gen));
    return history.back().generation_id;
  }

  uint32_t update_sps(const uint8_t *data, size_t length) {
    std::lock_guard<std::mutex> lock(tracker_mutex);
    if (history.back().sps.size() == length &&
        std::equal(data, data + length, history.back().sps.begin())) {
      return history.back().generation_id;
    }

    ConfigGeneration next_gen = history.back();
    ++next_gen.generation_id;
    next_gen.sps.assign(data, data + length);
    history.push_back(std::move(next_gen));
    return history.back().generation_id;
  }

  uint32_t update_pps(const uint8_t *data, size_t length) {
    std::lock_guard<std::mutex> lock(tracker_mutex);
    if (history.back().pps.size() == length &&
        std::equal(data, data + length, history.back().pps.begin())) {
      return history.back().generation_id;
    }

    ConfigGeneration next_gen = history.back();
    ++next_gen.generation_id;
    next_gen.pps.assign(data, data + length);
    history.push_back(std::move(next_gen));
    return history.back().generation_id;
  }

  /// Retrieves the configuration that was active during a specific generation
  [[nodiscard]] std::optional<ConfigGeneration> get_generation(uint32_t generation_id) const {
    std::lock_guard<std::mutex> lock(tracker_mutex);

    /// History is ordered by generation_id, so we could binary search, but since resolutions rarely
    /// change, a linear search is O(1) in practice.
    for (const auto &gen : history) {
      if (gen.generation_id == generation_id) {
        return gen;
      }
    }

    return std::nullopt;
  }

  /// Returns the current active generation ID
  [[nodiscard]] uint32_t current_generation() const noexcept {
    std::lock_guard<std::mutex> lock(tracker_mutex);
    return history.back().generation_id;
  }
};

} //< namespace rivet::media