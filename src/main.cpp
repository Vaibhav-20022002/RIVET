#include "rivet/media/AuAssembler.hpp"
#include "rivet/media/ConfigGeneration.hpp"
#include "rivet/media/DependencyDropPolicy.hpp"
#include "rivet/media/StartupPolicy.hpp"
#include "rivet/proto/rtp/RtcpReportGenerator.hpp"
#include "rivet/proto/rtp/SequenceTracker.hpp"
#include <iostream>

using namespace rivet;

int main(void) {
  std::cout << "RIVET engine started successfully!\n";
  std::cout << "Platform:";
#ifdef __linux__
  std::cout << "Linux (io_uring zero-copy networking ENABLED)\n";
#else
  std::cout << "macOS (io_uring DISABLED - Development Mode)\n";
#endif

  return 0;
}