#include "rivet/cli/Cli.hpp"
#include "rivet/core/RivetWorker.hpp"
#include <csignal>
#include <string_view>

int main(int argc, char **argv) {
  std::signal(SIGPIPE, SIG_IGN);
  if (argc >= 2 && std::string_view(argv[1]) == "--worker") {
    return rivet::core::run_worker_process(argc, argv);
  }
  return rivet::cli::run(argc, argv);
}
