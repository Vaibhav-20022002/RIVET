#pragma once

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace rivet::config {

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

  /// [Memory & Slab Pool]
  size_t numa_capacity{4096};
  size_t numa_slab_size{2048};
  size_t max_reactor_egress_bytes{16 * 1024 * 1024}; //< 16 MB
  size_t max_pending_sends{512};

  /// [Policies & Delivery]
  std::string startup_policy{"FastStartup"};  //< "FastStartup" or "FreshnessFirst"
  std::string drop_policy{"DependencyAware"}; //< "DependencyAware" or "Aggressive"
  std::string log_level{"INFO"};

  /// Tracks path of loaded .env file (if found)
  std::string loaded_env_file;

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
  ///\param env_path Path to .env file to load (default: ".env")
  ///\return Populated RivetConfig instance
  static RivetConfig from_env(const std::string &env_path = ".env") {
    RivetConfig cfg;

    if (load_dotenv(env_path)) {
      cfg.loaded_env_file = env_path;
    }

    if (const char *val = std::getenv("RIVET_RTSP_PORT")) {
      cfg.rtsp_port = static_cast<uint16_t>(std::atoi(val));
    }
    if (const char *val = std::getenv("RIVET_STREAM_NAME")) {
      cfg.stream_name = val;
    }
    if (const char *val = std::getenv("RIVET_EGRESS_SHARDS")) {
      cfg.egress_shards = static_cast<uint32_t>(std::atoi(val));
    }
    if (const char *val = std::getenv("RIVET_BASE_RTP_PORT")) {
      cfg.base_rtp_port = static_cast<uint16_t>(std::atoi(val));
    }
    if (const char *val = std::getenv("RIVET_BASE_RTCP_PORT")) {
      cfg.base_rtcp_port = static_cast<uint16_t>(std::atoi(val));
    }
    if (const char *val = std::getenv("RIVET_WATCHDOG_TIMEOUT_MS")) {
      cfg.watchdog_timeout_ms = static_cast<uint32_t>(std::atoi(val));
    }
    if (const char *val = std::getenv("RIVET_NUMA_CAPACITY")) {
      cfg.numa_capacity = static_cast<size_t>(std::strtoull(val, nullptr, 10));
    }
    if (const char *val = std::getenv("RIVET_NUMA_SLAB_SIZE")) {
      cfg.numa_slab_size = static_cast<size_t>(std::strtoull(val, nullptr, 10));
    }
    if (const char *val = std::getenv("RIVET_MAX_REACTOR_BYTES")) {
      cfg.max_reactor_egress_bytes = static_cast<size_t>(std::strtoull(val, nullptr, 10));
    }
    if (const char *val = std::getenv("RIVET_MAX_PENDING_SENDS")) {
      cfg.max_pending_sends = static_cast<size_t>(std::strtoull(val, nullptr, 10));
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

    print_section("Network");
    print_row("RTSP Port", std::to_string(rtsp_port));
    print_row("Stream Mount", "/" + stream_name);
    print_row("Base UDP Ports",
        std::to_string(base_rtp_port) + " (RTP) / " + std::to_string(base_rtcp_port) + " (RTCP)");
    print_row("Egress Shards", std::to_string(egress_shards) + " threads");
    print_row("Watchdog Timeout", std::to_string(watchdog_timeout_ms) + " ms");
    print_empty();

    print_section("Memory & Slab Pool");
    print_row("NUMA Arena Capacity", std::to_string(numa_capacity) + " slabs");
    print_row("Slab Buffer Size", std::to_string(numa_slab_size) + " bytes");
    print_row("Reactor Quota", std::to_string(max_reactor_egress_bytes / (1024 * 1024)) + " MB");
    print_row("Max Pending Sends", std::to_string(max_pending_sends) + " pkts/sub");
    print_empty();

    print_section("Policies & Delivery");
    print_row("Startup Policy", startup_policy);
    print_row("Drop Policy", drop_policy);
    print_row("Log Level", log_level);
    std::cout << "└──────────────────────────────────────────────────────────┘\n";
  }
};

} //< namespace rivet::config
