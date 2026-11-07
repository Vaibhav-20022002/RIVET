#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rivet::core {

class RivetWorker;

/// Generic thread-safe RTSP mount-path router.
/// Performs exact normalized stream-path matching (plus track control sub-paths such as
/// `/trackID=0` or `/streamid=0`) and NEVER falls back to an arbitrary unrelated stream.
template<typename HandleT> class BasicStreamRegistry {
  std::unordered_map<std::string, HandleT> routes_;
  mutable std::shared_mutex                registry_mutex_;

  [[nodiscard]] static bool is_track_suffix(std::string_view seg) noexcept {
    return seg.starts_with("trackID=") || seg.starts_with("trackid=") ||
        seg.starts_with("streamid=") || seg.starts_with("track");
  }

public:
  BasicStreamRegistry()  = default;
  ~BasicStreamRegistry() = default;

  BasicStreamRegistry(const BasicStreamRegistry &)            = delete;
  BasicStreamRegistry &operator=(const BasicStreamRegistry &) = delete;
  BasicStreamRegistry(BasicStreamRegistry &&)                 = delete;
  BasicStreamRegistry &operator=(BasicStreamRegistry &&)      = delete;

  /// Normalizes RTSP URLs/paths by stripping `rtsp://host:port`, query strings, leading/trailing `/`
  [[nodiscard]] static std::string normalize_path(std::string_view raw) {
    while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t' || raw.front() == '<')) {
      raw.remove_prefix(1);
    }
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '>')) {
      raw.remove_suffix(1);
    }

    bool had_scheme = false;
    if (raw.starts_with("rtsp://")) {
      raw.remove_prefix(7);
      had_scheme = true;
    } else if (raw.starts_with("rtsps://")) {
      raw.remove_prefix(8);
      had_scheme = true;
    }

    if (had_scheme) {
      const auto slash_pos = raw.find('/');
      if (slash_pos == std::string_view::npos) {
        return {};
      }
      raw = raw.substr(slash_pos);
    }

    const auto qpos = raw.find('?');
    if (qpos != std::string_view::npos) {
      raw = raw.substr(0, qpos);
    }

    while (!raw.empty() && raw.front() == '/') {
      raw.remove_prefix(1);
    }
    while (!raw.empty() && raw.back() == '/') {
      raw.remove_suffix(1);
    }

    return std::string(raw);
  }

  /// Extracts normalized stream base path (stripping trailing `/trackID=N` or `/streamid=N` if any)
  [[nodiscard]] static std::string extract_stream_path(std::string_view raw_url) {
    std::string norm = normalize_path(raw_url);
    const auto  pos  = norm.rfind('/');
    if (pos != std::string::npos) {
      std::string_view tail(norm.data() + pos + 1, norm.size() - pos - 1);
      if (is_track_suffix(tail)) {
        norm.resize(pos);
      }
    }
    return norm;
  }

  bool register_stream(std::string_view stream_id, HandleT handle) {
    const std::string normalized = normalize_path(stream_id);
    if (!handle || normalized.empty()) {
      return false;
    }

    std::unique_lock<std::shared_mutex> lock(registry_mutex_);
    if (routes_.find(normalized) != routes_.end()) {
      return false;
    }
    routes_.emplace(normalized, std::move(handle));
    return true;
  }

  void insert_or_replace(std::string_view stream_id, HandleT handle) {
    const std::string normalized = normalize_path(stream_id);
    if (!handle || normalized.empty()) {
      return;
    }
    std::unique_lock<std::shared_mutex> lock(registry_mutex_);
    routes_[normalized] = std::move(handle);
  }

  /// Exact path + sub-track lookup. Returns `nullptr` (`404 Not Found`) on unknown stream paths.
  [[nodiscard]] HandleT route(std::string_view path) const {
    const std::string normalized = normalize_path(path);
    if (normalized.empty()) {
      return nullptr;
    }

    std::shared_lock<std::shared_mutex> lock(registry_mutex_);
    auto                                it = routes_.find(normalized);
    if (it != routes_.end()) {
      return it->second;
    }

    const auto slash_pos = normalized.rfind('/');
    if (slash_pos != std::string::npos) {
      std::string_view tail(normalized.data() + slash_pos + 1, normalized.size() - slash_pos - 1);
      if (is_track_suffix(tail)) {
        std::string parent = normalized.substr(0, slash_pos);
        auto        pit    = routes_.find(parent);
        if (pit != routes_.end()) {
          return pit->second;
        }
      }
    }
    return nullptr;
  }

  template<typename StopFn>
  bool unregister_stream_with(std::string_view stream_id, StopFn &&on_remove) {
    const std::string normalized = normalize_path(stream_id);
    HandleT           removed{};
    {
      std::unique_lock<std::shared_mutex> lock(registry_mutex_);
      auto                                it = routes_.find(normalized);
      if (it == routes_.end()) {
        return false;
      }
      removed = std::move(it->second);
      routes_.erase(it);
    }
    if (removed) {
      on_remove(removed);
    }
    return true;
  }

  bool unregister_stream(std::string_view stream_id) {
    return unregister_stream_with(stream_id, [](HandleT &h) { h->stop(); });
  }

  template<typename StopFn> void clear_with(StopFn &&on_remove) {
    std::unordered_map<std::string, HandleT> snapshot;
    {
      std::unique_lock<std::shared_mutex> lock(registry_mutex_);
      snapshot.swap(routes_);
    }
    for (auto &kv : snapshot) {
      if (kv.second) {
        on_remove(kv.second);
      }
    }
  }

  void clear() {
    clear_with([](HandleT &h) { h->stop(); });
  }

  [[nodiscard]] bool has_stream(std::string_view stream_id) const {
    const std::string                   normalized = normalize_path(stream_id);
    std::shared_lock<std::shared_mutex> lock(registry_mutex_);
    return routes_.find(normalized) != routes_.end();
  }

  [[nodiscard]] size_t active_stream_count() const {
    std::shared_lock<std::shared_mutex> lock(registry_mutex_);
    return routes_.size();
  }

  [[nodiscard]] std::vector<std::string> list_streams() const {
    std::shared_lock<std::shared_mutex> lock(registry_mutex_);
    std::vector<std::string>            list;
    list.reserve(routes_.size());
    for (const auto &kv : routes_) {
      list.push_back(kv.first);
    }
    return list;
  }
};

using StreamRegistry = BasicStreamRegistry<std::shared_ptr<RivetWorker>>;

} //< namespace rivet::core
