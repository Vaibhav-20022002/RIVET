#pragma once

#include "rivet/core/Log.hpp"
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace rivet::config {

enum class StartupPolicyKind : uint8_t { FastPictureFirst, FreshnessFirst };

enum class DropPolicyKind : uint8_t { DependencyAware, TailDrop };

enum class ExecutionMode : uint8_t { MultiProcess, InProcess };

enum class IoBackendKind : uint8_t { AutoPoller, IoUring };

/// Strictly parses an unsigned integer in [min_val, max_val] without signs, whitespace, or
/// trailing garbage.
template<typename UInt>
[[nodiscard]] inline std::optional<UInt> parse_uint(std::string_view sv,
    UInt                                                             min_val = 0,
    UInt max_val = std::numeric_limits<UInt>::max()) noexcept {
  if (sv.empty() || sv.front() == '-' || sv.front() == '+') {
    return std::nullopt;
  }
  uint64_t               parsed = 0;
  const char            *begin  = sv.data();
  const char            *end    = sv.data() + sv.size();
  std::from_chars_result res    = std::from_chars(begin, end, parsed, 10);
  if (res.ec != std::errc{} || res.ptr != end) {
    return std::nullopt;
  }
  if (parsed < static_cast<uint64_t>(min_val) || parsed > static_cast<uint64_t>(max_val)) {
    return std::nullopt;
  }
  return static_cast<UInt>(parsed);
}

[[nodiscard]] inline StartupPolicyKind parse_startup_policy(std::string_view s) noexcept {
  auto ieq = [](std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      char ca = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 32) : a[i];
      char cb = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 32) : b[i];
      if (ca != cb) return false;
    }
    return true;
  };
  if (ieq(s, "FreshnessFirst") || ieq(s, "freshness")) {
    return StartupPolicyKind::FreshnessFirst;
  }
  return StartupPolicyKind::FastPictureFirst;
}

[[nodiscard]] inline DropPolicyKind parse_drop_policy(std::string_view s) noexcept {
  auto ieq = [](std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      char ca = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 32) : a[i];
      char cb = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 32) : b[i];
      if (ca != cb) return false;
    }
    return true;
  };
  if (ieq(s, "TailDrop") || ieq(s, "Aggressive")) {
    return DropPolicyKind::TailDrop;
  }
  return DropPolicyKind::DependencyAware;
}

/// System-wide configuration loaded from environment variables and .env file with fallback
/// defaults. Enables full runtime customizability without recompilation.
struct RivetConfig {
  /// [Network & Server]
  uint16_t    rtsp_port{8554};
  std::string stream_name{"live/test"};
  uint32_t    egress_shards{2};
  uint16_t    base_rtp_port{20000};
  uint16_t    base_rtcp_port{20001};
  uint32_t    watchdog_timeout_ms{5000};
  uint32_t    client_timeout_ms{60000};
  std::string advertised_ip{}; //< Optional explicit NAT/container advertised IP

  /// [Security & Publisher Access Control]
  std::string publish_user{};
  std::string publish_password{};

  /// [Memory, Retention & Slab Pool]
  size_t numa_capacity{4096};
  size_t numa_slab_size{2048};
  size_t max_reactor_egress_bytes{16 * 1024 * 1024}; //< 16 MiB per shard
  size_t max_pending_sends{512};
  size_t retention_budget_bytes{16 * 1024 * 1024};    //< 16 MiB GOP ring retention budget
  size_t burst_budget_bytes{2 * 1024 * 1024};         //< 2 MiB max fast-start burst budget
  size_t max_subscriber_queue_bytes{2 * 1024 * 1024}; //< 2 MiB default per-subscriber byte budget
  size_t worker_rlimit_as_mb{0}; //< Optional RLIMIT_AS cap for worker processes
  bool   enable_hugepages{false};
  bool   enable_udp_gso{false};

  /// [Policies, Isolation & Delivery]
  std::string   startup_policy{"FastStartup"};  //< "FastStartup" or "FreshnessFirst"
  std::string   drop_policy{"DependencyAware"}; //< "DependencyAware" or "TailDrop"
  std::string   log_level{"INFO"};
  bool          join_on_open_irap{true}; //< Allow joining on HEVC CRA/BLA_W_LP with RASL gate
  ExecutionMode execution_mode{ExecutionMode::MultiProcess};
  IoBackendKind io_backend{IoBackendKind::AutoPoller};
  std::string   worker_executable{}; //< Optional override for child worker binary path

  /// Tracks path of loaded .env file (if found)
  std::string loaded_env_file;

  [[nodiscard]] StartupPolicyKind startup_policy_kind() const noexcept {
    return parse_startup_policy(startup_policy);
  }

  [[nodiscard]] DropPolicyKind drop_policy_kind() const noexcept {
    return parse_drop_policy(drop_policy);
  }

  [[nodiscard]] core::LogLevel parsed_log_level() const noexcept {
    return core::parse_log_level(log_level);
  }

  /// Loads key-value pairs from an ENV file if present, respecting existing process env vars
  static bool load_dotenv(const std::string &path = ".env") {
    std::ifstream file(path);
    if (!file.is_open()) {
      return false;
    }

    std::string line;
    while (std::getline(file, line)) {
      size_t start = line.find_first_not_of(" \t\r\n");
      if (start == std::string::npos || line[start] == '#') {
        continue;
      }

      size_t eq = line.find('=', start);
      if (eq == std::string::npos) {
        continue;
      }

      std::string key     = line.substr(start, eq - start);
      size_t      key_end = key.find_last_not_of(" \t\r\n");
      if (key_end != std::string::npos) {
        key = key.substr(0, key_end + 1);
      }

      std::string val       = line.substr(eq + 1);
      size_t      val_start = val.find_first_not_of(" \t\r\n");
      size_t      val_end   = val.find_last_not_of(" \t\r\n");
      if (val_start != std::string::npos && val_end != std::string::npos) {
        val = val.substr(val_start, val_end - val_start + 1);
        if (val.size() >= 2 &&
            ((val.front() == '"' && val.back() == '"') ||
                (val.front() == '\'' && val.back() == '\''))) {
          val = val.substr(1, val.size() - 2);
        }
      } else {
        val.clear();
      }

      /// POSIX setenv with overwrite=0: existing shell environment variables take precedence
      setenv(key.c_str(), val.c_str(), 0);
    }
    return true;
  }

  /// Loads configuration values from environment variables and optional .env file
  static RivetConfig from_env(const std::string &env_path = ".env") {
    RivetConfig cfg;

    if (load_dotenv(env_path)) {
      cfg.loaded_env_file = env_path;
    }

    if (const char *val = std::getenv("RIVET_RTSP_PORT")) {
      if (auto v = parse_uint<uint16_t>(val, 0, 65535)) cfg.rtsp_port = *v;
    }
    if (const char *val = std::getenv("RIVET_STREAM_NAME")) {
      cfg.stream_name = val;
    }
    if (const char *val = std::getenv("RIVET_EGRESS_SHARDS")) {
      if (auto v = parse_uint<uint32_t>(val, 0, 64)) cfg.egress_shards = *v;
    }
    if (const char *val = std::getenv("RIVET_BASE_RTP_PORT")) {
      if (auto v = parse_uint<uint16_t>(val, 0, 65535)) cfg.base_rtp_port = *v;
    }
    if (const char *val = std::getenv("RIVET_BASE_RTCP_PORT")) {
      if (auto v = parse_uint<uint16_t>(val, 0, 65535)) cfg.base_rtcp_port = *v;
    }
    if (const char *val = std::getenv("RIVET_WATCHDOG_TIMEOUT_MS")) {
      if (auto v = parse_uint<uint32_t>(val, 100, 3600000)) cfg.watchdog_timeout_ms = *v;
    }
    if (const char *val = std::getenv("RIVET_CLIENT_TIMEOUT_MS")) {
      if (auto v = parse_uint<uint32_t>(val, 500, 3600000)) cfg.client_timeout_ms = *v;
    }
    if (const char *val = std::getenv("RIVET_ADVERTISED_IP")) {
      cfg.advertised_ip = val;
    }
    if (const char *val = std::getenv("RIVET_PUBLISH_USER")) {
      cfg.publish_user = val;
    }
    if (const char *val = std::getenv("RIVET_PUBLISH_PASSWORD")) {
      cfg.publish_password = val;
    }
    if (const char *val = std::getenv("RIVET_NUMA_CAPACITY")) {
      if (auto v = parse_uint<size_t>(val, 16, 1048576)) cfg.numa_capacity = *v;
    }
    if (const char *val = std::getenv("RIVET_NUMA_SLAB_SIZE")) {
      if (auto v = parse_uint<size_t>(val, 64, 65536)) cfg.numa_slab_size = *v;
    }
    if (const char *val = std::getenv("RIVET_MAX_REACTOR_BYTES")) {
      if (auto v = parse_uint<size_t>(val, 65536, 4ULL * 1024 * 1024 * 1024)) {
        cfg.max_reactor_egress_bytes = *v;
      }
    }
    if (const char *val = std::getenv("RIVET_MAX_PENDING_SENDS")) {
      if (auto v = parse_uint<size_t>(val, 8, 65536)) cfg.max_pending_sends = *v;
    }
    if (const char *val = std::getenv("RIVET_RETENTION_BUDGET_BYTES")) {
      if (auto v = parse_uint<size_t>(val, 65536, 1024ULL * 1024 * 1024)) {
        cfg.retention_budget_bytes = *v;
      }
    }
    if (const char *val = std::getenv("RIVET_BURST_BUDGET_BYTES")) {
      if (auto v = parse_uint<size_t>(val, 0, 256ULL * 1024 * 1024)) {
        cfg.burst_budget_bytes = *v;
      }
    }
    if (const char *val = std::getenv("RIVET_SUBSCRIBER_QUEUE_BYTES")) {
      if (auto v = parse_uint<size_t>(val, 16384, 256ULL * 1024 * 1024)) {
        cfg.max_subscriber_queue_bytes = *v;
      }
    }
    if (const char *val = std::getenv("RIVET_STARTUP_POLICY")) {
      cfg.startup_policy = val;
    }
    if (const char *val = std::getenv("RIVET_DROP_POLICY")) {
      cfg.drop_policy = val;
    }
    if (const char *val = std::getenv("RIVET_LOG_LEVEL")) {
      cfg.log_level = val;
    }
    if (const char *val = std::getenv("RIVET_JOIN_ON_OPEN_IRAP")) {
      std::string_view sv(val);
      cfg.join_on_open_irap = !(sv == "0" || sv == "false" || sv == "FALSE");
    }
    if (const char *val = std::getenv("RIVET_EXECUTION_MODE")) {
      std::string_view sv(val);
      if (sv == "inprocess" || sv == "InProcess") {
        cfg.execution_mode = ExecutionMode::InProcess;
      } else {
        cfg.execution_mode = ExecutionMode::MultiProcess;
      }
    }

    return cfg;
  }

  /// Displays settings dashboard on console with exact column alignment
  void print_dashboard() const {
    constexpr size_t WIDTH = 58;

    auto print_row = [&](std::string_view label, const std::string &val) {
      std::string line = "  - ";
      line.append(label);
      if (line.size() < 27) {
        line.append(27 - line.size(), ' ');
      }
      line.append(": ");
      line.append(val);
      if (line.size() < WIDTH) {
        line.append(WIDTH - line.size(), ' ');
      } else if (line.size() > WIDTH) {
        line.resize(WIDTH);
      }
      std::cout << "│" << line << "│\n";
    };

    auto print_section = [&](std::string_view header) {
      std::string line = " [";
      line.append(header);
      line.append("]");
      if (line.size() < WIDTH) {
        line.append(WIDTH - line.size(), ' ');
      }
      std::cout << "│" << line << "│\n";
    };

    auto print_empty = [&]() {
      std::string line(WIDTH, ' ');
      std::cout << "│" << line << "│\n";
    };

    std::cout << "┌──────────────────────────────────────────────────────────┐\n";
    std::cout << "│               RIVET System Settings (ENV)                │\n";
    std::cout << "├──────────────────────────────────────────────────────────┤\n";

    if (!loaded_env_file.empty()) {
      std::string line = "  Configuration File      : " + loaded_env_file;
      if (line.size() < WIDTH) {
        line.append(WIDTH - line.size(), ' ');
      }
      std::cout << "│" << line << "│\n";
      std::cout << "├──────────────────────────────────────────────────────────┤\n";
    }

    print_section("Network & Isolation");
    print_row("RTSP Port", std::to_string(rtsp_port));
    print_row("Stream Mount", "/" + stream_name);
    print_row("Base UDP Ports",
        std::to_string(base_rtp_port) + " (RTP) / " + std::to_string(base_rtcp_port) + " (RTCP)");
    print_row("Egress Shards", std::to_string(egress_shards) + " threads");
    print_row("Isolation Mode",
        execution_mode == ExecutionMode::MultiProcess ? "Process-per-Stream" : "In-Process");
    print_row("Watchdog Timeout", std::to_string(watchdog_timeout_ms) + " ms");
    print_empty();

    print_section("Memory & Retention");
    print_row("NUMA Arena Capacity", std::to_string(numa_capacity) + " slabs");
    print_row("Slab Buffer Size", std::to_string(numa_slab_size) + " bytes");
    print_row("Retention Budget", std::to_string(retention_budget_bytes / (1024 * 1024)) + " MB");
    print_row("Reactor Quota", std::to_string(max_reactor_egress_bytes / (1024 * 1024)) + " MB");
    print_empty();

    print_section("Policies & Delivery");
    print_row("Startup Policy", startup_policy);
    print_row("Drop Policy", drop_policy);
    print_row("Publisher Auth", publish_user.empty() ? "Guards Only" : "RTSP Digest Auth");
    print_row("Log Level", log_level);
    std::cout << "└──────────────────────────────────────────────────────────┘\n";
  }
};

} //< namespace rivet::config
