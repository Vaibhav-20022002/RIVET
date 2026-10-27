#pragma once

#include <chrono>
#include <thread>

namespace rivet::test {

/// Polls a predicate until it returns true or until the timeout is reached.
/// Avoids non-deterministic thread sleep sleeps in integration and concurrency tests.
template<typename Predicate>
bool eventually(Predicate   &&pred,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(2000),
    std::chrono::milliseconds step    = std::chrono::milliseconds(5)) {
  auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < timeout) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(step);
  }
  return pred();
}

} //< namespace rivet::test
