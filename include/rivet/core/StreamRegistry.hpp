#pragma once

#include "rivet/core/RivetWorker.hpp"
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rivet::core {

/// Dynamic Stream Registry routing RTSP mount paths to worker process instances.
/// Provides O(1) concurrent path lookup for incoming DESCRIBE, SETUP, and ANNOUNCE
/// requests.
class StreamRegistry {
  /// Maps stream_id (e.g. "camera1" or "/live/camera1") to its owning worker instance
  std::unordered_map<std::string, std::shared_ptr<RivetWorker>> routes;

  /// Reader-writer lock allowing unlimited concurrent route() reads
  mutable std::shared_mutex registry_mutex;

public:
  StreamRegistry()  = default;
  ~StreamRegistry() = default;

  /// Non-copyable, non-movable
  StreamRegistry(const StreamRegistry &)            = delete;
  StreamRegistry &operator=(const StreamRegistry &) = delete;
  StreamRegistry(StreamRegistry &&)                 = delete;
  StreamRegistry &operator=(StreamRegistry &&)      = delete;

  /// Registers a worker instance under a specific stream identifier
  ///\param stream_id Unique stream identifier or path (e.g. "front_gate")
  ///\param worker Shared pointer to the RivetWorker instance
  ///\return True if registered, false if stream_id is already in use
  bool register_stream(std::string stream_id, std::shared_ptr<RivetWorker> worker) {
    if (!worker || stream_id.empty()) {
      return false;
    }

    std::unique_lock<std::shared_mutex> lock(registry_mutex);
    auto                                normalized = normalize_path(stream_id);
    if (routes.find(normalized) != routes.end()) {
      return false; //< Already registered
    }

    routes[normalized] = std::move(worker);
    return true;
  }

  /// Resolves an incoming RTSP URL path (e.g. "rtsp://0.0.0.0:8554/live/front_gate") to its worker
  ///\param path The requested URL or mount path
  ///\return Shared pointer to RivetWorker, or nullptr if stream not found
  [[nodiscard]] std::shared_ptr<RivetWorker> route(std::string_view path) const {
    std::shared_lock<std::shared_mutex> lock(registry_mutex);
    if (routes.empty()) {
      return nullptr;
    }

    auto normalized = normalize_path(path);
    auto it         = routes.find(normalized);
    if (it != routes.end()) {
      return it->second;
    }

    /// Check if path is a sub-track (e.g. "test/streamid=0" or "test/track0")
    auto slash_pos = normalized.rfind('/');
    if (slash_pos != std::string_view::npos) {
      std::string parent_stream = normalized.substr(0, slash_pos);
      auto        pit           = routes.find(parent_stream);
      if (pit != routes.end()) {
        return pit->second;
      }
    }

    /// Fallback: if only 1 worker is registered, route to it
    if (routes.size() == 1) {
      return routes.begin()->second;
    }

    return nullptr;
  }

  /// Unregisters and tears down a stream path
  ///\param stream_id The identifier to unregister
  ///\return True if removed, false if not found
  bool unregister_stream(std::string_view stream_id) {
    std::unique_lock<std::shared_mutex> lock(registry_mutex);
    auto                                normalized = normalize_path(stream_id);
    auto                                it         = routes.find(normalized);
    if (it != routes.end()) {
      it->second->stop();
      routes.erase(it);
      return true;
    }
    return false;
  }

  /// Clears all routes and terminates worker processes cleanly
  void clear() {
    std::unique_lock<std::shared_mutex> lock(registry_mutex);
    for (auto &pair : routes) {
      if (pair.second) {
        pair.second->stop();
      }
    }
    routes.clear();
  }

  /// Checks if a stream path is currently registered and active
  [[nodiscard]] bool has_stream(std::string_view stream_id) const {
    std::shared_lock<std::shared_mutex> lock(registry_mutex);
    auto                                normalized = normalize_path(stream_id);
    return routes.find(normalized) != routes.end();
  }

  /// Returns total number of active streams registered
  [[nodiscard]] size_t active_stream_count() const {
    std::shared_lock<std::shared_mutex> lock(registry_mutex);
    return routes.size();
  }

  /// Returns a snapshot of all active stream identifiers
  [[nodiscard]] std::vector<std::string> list_streams() const {
    std::shared_lock<std::shared_mutex> lock(registry_mutex);
    std::vector<std::string>            list;
    list.reserve(routes.size());
    for (const auto &pair : routes) {
      list.push_back(pair.first);
    }
    return list;
  }

private:
  /// Normalizes RTSP paths by stripping leading "rtsp://" host prefixes and trailing slashes
  static std::string normalize_path(std::string_view raw) {
    /// Strip rtsp:// or rtsps:// if present
    bool had_scheme = false;
    if (raw.starts_with("rtsp://")) {
      raw.remove_prefix(7);
      had_scheme = true;
    } else if (raw.starts_with("rtsps://")) {
      raw.remove_prefix(8);
      had_scheme = true;
    }
    if (had_scheme) {
      if (raw.starts_with('<')) {
        raw.remove_prefix(1);
      }
      /// Find the first slash after host:port
      auto slash_pos = raw.find('/');
      if (slash_pos != std::string_view::npos) {
        raw = raw.substr(slash_pos);
      }
    }

    /// Strip trailing slashes
    while (raw.size() > 1 && raw.back() == '/') {
      raw.remove_suffix(1);
    }

    /// Strip leading /live/ or leading /
    if (raw.starts_with("/live/")) {
      raw.remove_prefix(6);
    } else if (raw.starts_with('/')) {
      raw.remove_prefix(1);
    }

    return std::string(raw);
  }
};

} // namespace rivet::core
