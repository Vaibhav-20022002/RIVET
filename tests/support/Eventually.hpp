#pragma once

#include <chrono>
#include <cstdlib>
#include <thread>

namespace rivet::test {

inline int timeout_scale_factor() noexcept {
  if (const char *env = std::getenv("RIVET_TIMEOUT_SCALE")) {
    const int v = std::atoi(env);
    if (v >= 1 && v <= 20) {
      return v;
    }
  }
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
  return 3;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
  return 3;
#endif
#endif
  return 1;
}

/// Polls a predicate until it returns true or until the (sanitizer-scaled) timeout is reached.
template<typename Predicate>
bool eventually(Predicate   &&pred,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(2000),
    std::chrono::milliseconds step    = std::chrono::milliseconds(5)) {
  const auto scaled_timeout = timeout * timeout_scale_factor();
  const auto start          = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < scaled_timeout) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(step);
  }
  return pred();
}

} //< namespace rivet::test
