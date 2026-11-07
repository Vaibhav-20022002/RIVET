#include "rivet/cli/Cli.hpp"

#include "rivet/config/RivetConfig.hpp"
#include "rivet/core/Log.hpp"
#include "rivet/core/RivetController.hpp"
#include "rivet/core/RivetWorker.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace rivet::cli {

namespace {
std::atomic<bool> g_running{true};

void signal_handler(int sig) {
  if (sig == SIGINT || sig == SIGTERM) {
    g_running.store(false, std::memory_order_release);
  }
}
} //< namespace

void print_usage(const char *prog_name) {
  std::cout
      << "Usage: " << prog_name << " [options]\n\n"
      << "Options (can also be configured via environment variables or .env file):\n"
      << "  -e, --env-file <path>      Path to custom .env file (default: .env)\n"
      << "  -p, --port <port>          RTSP TCP listening port [ENV: RIVET_RTSP_PORT] (default: "
         "8554)\n"
      << "  -s, --stream <name>        Default stream mount path [ENV: RIVET_STREAM_NAME] "
         "(default: live/test)\n"
      << "  -n, --shards <count>       EgressReactor shards per worker [ENV: RIVET_EGRESS_SHARDS] "
         "(default: 2)\n"
      << "      --rtp-port <port>      Base dedicated UDP RTP port [ENV: RIVET_BASE_RTP_PORT] "
         "(default: 20000)\n"
      << "      --rtcp-port <port>     Base dedicated UDP RTCP port [ENV: RIVET_BASE_RTCP_PORT] "
         "(default: 20001)\n"
      << "      --multi-process        Process-per-input-stream worker isolation (default)\n"
      << "      --in-process           Run workers in-process (development / TSan testing)\n"
      << "      --pub-user <username>  Require RTSP Digest Auth username on ANNOUNCE\n"
      << "      --pub-pass <password>  RTSP Digest Auth password on ANNOUNCE\n"
      << "      --advertised-ip <ip>   Optional explicit advertised IPv4 address\n"
      << "      --startup-policy <p>   Startup mode: FastStartup or FreshnessFirst\n"
      << "      --drop-policy <p>      Congestion drop mode: DependencyAware or TailDrop\n"
      << "      --log-level <level>    Log level: ERROR, WARN, INFO, DEBUG, TRACE\n"
      << "      --hugepages            Advise MADV_HUGEPAGE on worker slab arenas (Linux)\n"
      << "      --udp-gso              Enable Linux UDP_SEGMENT GSO coalescing when supported\n"
      << "  -h, --help                 Show this help message and exit\n";
}

ParseResult parse_args(int argc, char **argv, config::RivetConfig &config, std::string &env_path) {
  env_path = ".env";
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if ((arg == "-e" || arg == "--env-file") && i + 1 < argc) {
      env_path = argv[i + 1];
    }
  }

  config = config::RivetConfig::from_env(env_path);

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return ParseResult::HelpRequested;
    } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
      auto parsed = config::parse_uint<uint16_t>(argv[++i]);
      if (!parsed.has_value()) {
        std::cerr << "Invalid --port value: " << argv[i] << "\n";
        return ParseResult::Error;
      }
      config.rtsp_port = *parsed;
    } else if ((arg == "-s" || arg == "--stream") && i + 1 < argc) {
      config.stream_name = argv[++i];
    } else if ((arg == "-n" || arg == "--shards") && i + 1 < argc) {
      auto parsed = config::parse_uint<uint32_t>(argv[++i], 1U, 64U);
      if (!parsed.has_value()) {
        std::cerr << "Invalid --shards value (expected 1..64): " << argv[i] << "\n";
        return ParseResult::Error;
      }
      config.egress_shards = *parsed;
    } else if (arg == "--rtp-port" && i + 1 < argc) {
      auto parsed = config::parse_uint<uint16_t>(argv[++i]);
      if (!parsed.has_value()) {
        std::cerr << "Invalid --rtp-port value: " << argv[i] << "\n";
        return ParseResult::Error;
      }
      config.base_rtp_port = *parsed;
    } else if (arg == "--rtcp-port" && i + 1 < argc) {
      auto parsed = config::parse_uint<uint16_t>(argv[++i]);
      if (!parsed.has_value()) {
        std::cerr << "Invalid --rtcp-port value: " << argv[i] << "\n";
        return ParseResult::Error;
      }
      config.base_rtcp_port = *parsed;
    } else if (arg == "--in-process") {
      config.execution_mode = config::ExecutionMode::InProcess;
    } else if (arg == "--multi-process") {
      config.execution_mode = config::ExecutionMode::MultiProcess;
    } else if (arg == "--pub-user" && i + 1 < argc) {
      config.publish_user = argv[++i];
    } else if (arg == "--pub-pass" && i + 1 < argc) {
      config.publish_password = argv[++i];
    } else if (arg == "--advertised-ip" && i + 1 < argc) {
      config.advertised_ip = argv[++i];
    } else if (arg == "--startup-policy" && i + 1 < argc) {
      config.startup_policy = argv[++i];
    } else if (arg == "--drop-policy" && i + 1 < argc) {
      config.drop_policy = argv[++i];
    } else if (arg == "--log-level" && i + 1 < argc) {
      config.log_level = argv[++i];
    } else if (arg == "--hugepages") {
      config.enable_hugepages = true;
    } else if (arg == "--udp-gso") {
      config.enable_udp_gso = true;
    } else if ((arg == "-e" || arg == "--env-file") && i + 1 < argc) {
      ++i;
    } else {
      std::cerr << "Unknown or incomplete argument: " << arg << "\n";
      print_usage(argv[0]);
      return ParseResult::Error;
    }
  }

  return ParseResult::Success;
}

int run(int argc, char **argv) {
  if (argc >= 2 && std::string_view(argv[1]) == "--worker") {
    return core::run_worker_process(argc, argv);
  }

  std::signal(SIGPIPE, SIG_IGN);

  config::RivetConfig config;
  std::string         env_path;

  auto res = parse_args(argc, argv, config, env_path);
  if (res == ParseResult::HelpRequested) {
    return 0;
  }
  if (res == ParseResult::Error) {
    return 1;
  }

  core::Logger::set_level(config.parsed_log_level());

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  std::cout << "\n"
               "██████╗ ██╗ ██╗   ██╗ ███████╗ ████████╗\n"
               "██╔══██╗██║ ██║   ██║ ██╔════╝ ╚══██╔══╝\n"
               "██████╔╝██║ ██║   ██║ █████╗      ██║   \n"
               "██╔══██╗██║ ╚██╗ ██╔╝ ██╔══╝      ██║   \n"
               "██║  ██║██║  ╚████╔╝  ███████╗    ██║   \n"
               "╚═╝  ╚═╝╚═╝   ╚═══╝   ╚══════╝    ╚═╝   \n\n";

  std::cout << "Platform: ";
#if defined(__linux__)
#if defined(RIVET_HAS_IO_URING)
  std::cout << "Linux (epoll + sendmmsg/recvmmsg + io_uring support enabled)\n";
#else
  std::cout << "Linux (epoll + sendmmsg/recvmmsg non-blocking reactor)\n";
#endif
#elif defined(__APPLE__)
  std::cout << "macOS (kqueue + EVFILT_USER non-blocking reactor)\n";
#else
  std::cout << "POSIX (non-blocking reactor)\n";
#endif

  config.print_dashboard();

  core::RivetController controller(config);
  if (!controller.start()) {
    std::cerr << "[RIVET] Fatal: supervisor failed to bind/listen on TCP port " << config.rtsp_port
              << ".\n";
    return 1;
  }

  const uint16_t    bound_port = controller.get_bound_rtsp_port();
  const std::string host_str =
      config.advertised_ip.empty() ? std::string("localhost") : config.advertised_ip;

  std::cout << "\nServer ready. Dynamic multi-stream workers spawn automatically on ANNOUNCE.\n\n";
  std::cout << "Example Stream Ingest (Publish) URL:\n";
  std::cout << "  rtsp://<" << host_str << ":" << bound_port << "/" << config.stream_name << "\n\n";

  std::cout << "Example Stream Egress (Playback) URL:\n";
  std::cout << "  rtsp://<" << host_str << ":" << bound_port << "/" << config.stream_name << "\n\n";
  std::cout << "Press Ctrl+C to stop.\n\n" << std::flush;

  while (g_running.load(std::memory_order_acquire)) {
    controller.supervise();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  std::cout << "\n[RIVET] Shutdown signal received. Quiescing workers...\n";
  controller.stop();
  std::cout << "[RIVET] Server shutdown complete.\n";

  return 0;
}

} //< namespace rivet::cli
