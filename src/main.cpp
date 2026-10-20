#include "rivet/config/RivetConfig.hpp"
#include "rivet/core/RivetController.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

using namespace rivet;

namespace {
std::atomic<bool> g_running{true};

void signal_handler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    g_running.store(false, std::memory_order_release);
  }
}

void print_usage(const char *prog_name) {
  std::cout
      << "Usage: " << prog_name << " [options]\n\n"
      << "Options (can also be configured via environment variables or .env file):\n"
      << "  -e, --env-file <path>   Path to custom .env file (default: .env)\n"
      << "  -p, --port <port>       RTSP TCP listening port [ENV: RIVET_RTSP_PORT] (default: "
         "8554)\n"
      << "  -s, --stream <name>     Stream mount path name [ENV: RIVET_STREAM_NAME] (default: "
         "live/test)\n"
      << "  -n, --shards <count>    Number of EgressReactor threads per worker [ENV: "
         "RIVET_EGRESS_SHARDS] (default: 2)\n"
      << "      --rtp-port <port>   Base dedicated UDP RTP port [ENV: RIVET_BASE_RTP_PORT] "
         "(default: 20000)\n"
      << "      --rtcp-port <port>  Base dedicated UDP RTCP port [ENV: RIVET_BASE_RTCP_PORT] "
         "(default: 20001)\n"
      << "  -h, --help              Show this help message and exit\n\n"
      << "Additional Environment Variables for System Settings:\n"
      << "  RIVET_WATCHDOG_TIMEOUT_MS Supervisor watchdog check interval (default: 5000)\n"
      << "  RIVET_NUMA_CAPACITY       Slab allocator capacity in slabs (default: 4096)\n"
      << "  RIVET_NUMA_SLAB_SIZE      Slab size in bytes (default: 2048)\n"
      << "  RIVET_MAX_REACTOR_BYTES   Aggregate memory quota per reactor (default: 16777216)\n"
      << "  RIVET_MAX_PENDING_SENDS   Per-subscriber queue bound (default: 512)\n"
      << "  RIVET_STARTUP_POLICY      Startup mode: FastStartup or FreshnessFirst (default: "
         "FastStartup)\n"
      << "  RIVET_DROP_POLICY         Congestion drop mode: DependencyAware or Aggressive\n"
      << "  RIVET_LOG_LEVEL           Log level: INFO, DEBUG, WARN (default: INFO)\n";
}
} //< namespace

int main(int argc, char **argv) {
  std::string env_path = ".env";
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if ((arg == "-e" || arg == "--env-file") && i + 1 < argc) {
      env_path = argv[i + 1];
    }
  }

  /// Load settings from environment variables and .env file
  auto config = config::RivetConfig::from_env(env_path);

  /// Allow CLI arguments to override environment settings if explicitly passed
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
      config.rtsp_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if ((arg == "-s" || arg == "--stream") && i + 1 < argc) {
      config.stream_name = argv[++i];
    } else if ((arg == "-n" || arg == "--shards") && i + 1 < argc) {
      config.egress_shards = static_cast<uint32_t>(std::atoi(argv[++i]));
    } else if (arg == "--rtp-port" && i + 1 < argc) {
      config.base_rtp_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--rtcp-port" && i + 1 < argc) {
      config.base_rtcp_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else {
      std::cerr << "Unknown or incomplete argument: " << arg << "\n";
      print_usage(argv[0]);
      return 1;
    }
  }

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
  std::signal(SIGPIPE, SIG_IGN);

  std::cout << "\n"
               "██████╗ ██╗ ██╗   ██╗ ███████╗ ████████╗\n"
               "██╔══██╗██║ ██║   ██║ ██╔════╝ ╚══██╔══╝\n"
               "██████╔╝██║ ██║   ██║ █████╗      ██║   \n"
               "██╔══██╗██║ ╚██╗ ██╔╝ ██╔══╝      ██║   \n"
               "██║  ██║██║  ╚████╔╝  ███████╗    ██║   \n"
               "╚═╝  ╚═╝╚═╝   ╚═══╝   ╚══════╝    ╚═╝   \n\n";

  std::cout << "Platform: ";
#ifdef __linux__
  std::cout << "Linux (io_uring zero-copy networking ENABLED)\n";
#else
  std::cout << "macOS (Development Mode)\n";
#endif

  /// Render Settings Dashboard
  config.print_dashboard();

  core::RivetController controller(config.rtsp_port,
      config.watchdog_timeout_ms,
      config.startup_policy,
      config.base_rtp_port,
      config.base_rtcp_port);
  controller.start();

  auto primary_worker = controller.spawn_worker(config.stream_name, config.egress_shards);

  std::cout << "\nServer ready. Accepting connections.\n\n";
  std::cout << "Stream Ingest (Publish) URL:\n";
  std::cout << "  rtsp://<localhost:" << config.rtsp_port << "/" << config.stream_name << "\n\n";

  std::cout << "Stream Egress (Playback) URL:\n";
  std::cout << "  rtsp://<localhost:" << config.rtsp_port << "/" << config.stream_name << "\n\n";
  std::cout << "Press Ctrl+C to stop.\n\n" << std::flush;

  while (g_running.load(std::memory_order_acquire)) {
    controller.supervise();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  std::cout << "\n[RIVET] Shutdown signal received. Quiescing workers...\n";
  primary_worker.reset();
  controller.stop();
  std::cout << "[RIVET] Server shutdown complete.\n";

  return 0;
}