#include "rivet/core/RivetWorker.hpp"
#include <csignal>
#include <gtest/gtest.h>
#include <string_view>

int main(int argc, char **argv) {
  std::signal(SIGPIPE, SIG_IGN);
  /// Allow RivetController in ExecutionMode::MultiProcess to fork() + execv() the test binary
  /// itself with `--worker --uds-fd <fd> ...`!
  if (argc >= 2 && std::string_view(argv[1]) == "--worker") {
    return rivet::core::run_worker_process(argc, argv);
  }

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
