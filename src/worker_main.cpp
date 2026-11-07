#include "rivet/config/RivetConfig.hpp"
#include "rivet/core/Log.hpp"
#include "rivet/core/RivetWorker.hpp"
#include <csignal>
#include <cstdint>
#include <string>
#include <string_view>
#include <sys/resource.h>

namespace rivet::core {

int run_worker_process(int argc, char **argv) {
  std::signal(SIGPIPE, SIG_IGN);

  std::string env_path = ".env";
  int         uds_fd   = -1;
  std::string stream   = "live/default";

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--env-file" && i + 1 < argc) {
      env_path = argv[++i];
    }
  }

  config::RivetConfig cfg = config::RivetConfig::from_env(env_path);

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "--worker") {
      continue;
    } else if (arg == "--env-file" && i + 1 < argc) {
      ++i;
    } else if (arg == "--uds-fd" && i + 1 < argc) {
      if (auto v = config::parse_uint<uint32_t>(argv[++i], 0U, 1048576U)) {
        uds_fd = static_cast<int>(*v);
      } else {
        return 2;
      }
    } else if (arg == "--stream" && i + 1 < argc) {
      stream = argv[++i];
    } else if (arg == "--shards" && i + 1 < argc) {
      if (auto v = config::parse_uint<uint32_t>(argv[++i], 0U, 64U)) {
        cfg.egress_shards = *v;
      }
    } else if (arg == "--rtp-port" && i + 1 < argc) {
      if (auto v = config::parse_uint<uint16_t>(argv[++i])) {
        cfg.base_rtp_port = *v;
      }
    } else if (arg == "--rtcp-port" && i + 1 < argc) {
      if (auto v = config::parse_uint<uint16_t>(argv[++i])) {
        cfg.base_rtcp_port = *v;
      }
    } else if (arg == "--startup-policy" && i + 1 < argc) {
      cfg.startup_policy = argv[++i];
    } else if (arg == "--numa-capacity" && i + 1 < argc) {
      if (auto v = config::parse_uint<size_t>(argv[++i], 16U, 1048576U)) {
        cfg.numa_capacity = *v;
      }
    } else if (arg == "--retention-bytes" && i + 1 < argc) {
      if (auto v = config::parse_uint<size_t>(argv[++i], 4096U)) {
        cfg.retention_budget_bytes = *v;
      }
    } else if (arg == "--burst-bytes" && i + 1 < argc) {
      if (auto v = config::parse_uint<size_t>(argv[++i])) {
        cfg.burst_budget_bytes = *v;
      }
    } else if (arg == "--sub-queue-bytes" && i + 1 < argc) {
      if (auto v = config::parse_uint<size_t>(argv[++i], 4096U)) {
        cfg.max_subscriber_queue_bytes = *v;
      }
    } else if (arg == "--client-timeout-ms" && i + 1 < argc) {
      if (auto v = config::parse_uint<uint32_t>(argv[++i], 100U)) {
        cfg.client_timeout_ms = *v;
      }
    } else if (arg == "--join-open-irap" && i + 1 < argc) {
      std::string_view b    = argv[++i];
      cfg.join_on_open_irap = !(b == "0" || b == "false");
    } else if (arg == "--pub-user" && i + 1 < argc) {
      cfg.publish_user = argv[++i];
    } else if (arg == "--pub-pass" && i + 1 < argc) {
      cfg.publish_password = argv[++i];
    } else if (arg == "--rlimit-mb" && i + 1 < argc) {
      if (auto v = config::parse_uint<size_t>(argv[++i])) {
        cfg.worker_rlimit_as_mb = *v;
      }
    }
  }

  if (uds_fd < 0) {
    return 2;
  }

  Logger::set_level(cfg.parsed_log_level());

  /// Optional OS virtual memory ceiling per worker process
  if (cfg.worker_rlimit_as_mb > 0) {
    struct rlimit rl{};
    if (::getrlimit(RLIMIT_AS, &rl) == 0) {
      const rlim_t target = static_cast<rlim_t>(cfg.worker_rlimit_as_mb) * 1024ULL * 1024ULL;
      if (rl.rlim_max == RLIM_INFINITY || target <= rl.rlim_max) {
        rl.rlim_cur = target;
        (void)::setrlimit(RLIMIT_AS, &rl);
      }
    }
  }

  cfg.stream_name = stream;
  RivetWorker worker(stream, cfg);
  worker.run_process_loop(uds_fd, true);
  return 0;
}

} //< namespace rivet::core
