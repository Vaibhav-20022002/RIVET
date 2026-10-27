#pragma once

#if defined(RIVET_ENABLE_TRACY)

#include <tracy/Tracy.hpp>

/// Tracy-backed nanosecond execution profiling zones
#define RIVET_ZONE(name) ZoneScopedN(name)
#define RIVET_ZONE_C(name, color) ZoneScopedNC(name, color)
#define RIVET_FRAME_MARK FrameMark

#else

/// Zero-overhead no-ops when Tracy is disabled
#define RIVET_ZONE(name) ((void)0)
#define RIVET_ZONE_C(name, color) ((void)0)
#define RIVET_FRAME_MARK ((void)0)

#endif
