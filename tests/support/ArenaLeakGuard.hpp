#pragma once

#include "rivet/memory/NumaArena.hpp"
#include <gtest/gtest.h>

namespace rivet::test {

/// RAII Guard that asserts that zero memory slabs remain allocated/leaked
/// upon leaving scope (e.g. at the end of a test case).
class ArenaLeakGuard {
  const memory::NumaArena &arena;
  size_t                   initial_in_flight;

public:
  explicit ArenaLeakGuard(const memory::NumaArena &a)
      : arena(a)
      , initial_in_flight(a.in_flight()) {}

  ~ArenaLeakGuard() {
    EXPECT_EQ(arena.in_flight(), initial_in_flight)
        << "Memory slab leak detected: in_flight slabs before=" << initial_in_flight
        << ", after=" << arena.in_flight();
  }
};

} //< namespace rivet::test
