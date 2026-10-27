# RIVET

__RIVET__ (__Real-time Intelligent Video Engine for Transmission__) is a lightweight RTSP/RTP media server written in C++20 for streaming multimedia. It supports stream publishing and playback over both TCP (interleaved) and UDP.

---

## Key Features

- **Protocols & RFC Standards**:
  - Full **RTSP 1.0 (RFC 2326)** control plane (`OPTIONS`, `ANNOUNCE`, `DESCRIBE`, `SETUP`, `RECORD`, `PLAY`, `TEARDOWN`).
  - **RTP/RTCP (RFC 3550)** data transport with rollover-safe 16-bit sequence tracking and jitter estimation.
  - **H.264 Payload (RFC 6184)**: Single NAL units, STAP-A aggregation packets, and FU-A fragmentation units.
- **Multi-Track Media Pipeline**:
  - Simultaneous video (Track 0, 90 kHz clock) and audio (Track 1, 48 kHz clock) multiplexing.
  - In-flight SPS/PPS parameter set inspection and circular GOP caching for instantaneous client catch-up.
  - Access Unit (AU) assembly and frame boundary synchronization.
- **Lock-Free Multi-Reactor Architecture**:
  - Dedicated **Ingest Reactor** decoupled from multi-threaded **Egress Reactors** via wait-free Single-Producer Single-Consumer (SPSC) ring queues.
  - Subscriber sessions sharded across worker threads with isolated transmit queues to prevent cross-core contention.
- **Deterministic Memory Architecture**:
  - Non-blocking fixed-slab memory pool (**NumaArena**) providing deterministic packet buffer allocation without heap fragmentation.
  - Intrusive reference-counted packet handles (**PacketRef**) for zero-copy fan-out to hundreds of concurrent subscribers.
- **Reliability & Backpressure Control**:
  - Per-reactor memory bounds (16 MB default quota) and per-subscriber queue limits.
  - Congestion drop policies: `DependencyAware` (drops non-reference frames first) vs. `Aggressive`.
  - Startup policies: `FreshnessFirst` vs. `FastStartup` (instant IDR burst from keyframe log).
  - Supervisor watchdog thread monitoring worker heartbeats and socket health.
- **Production-Grade Testing Suite**:
  - Enforced build-time test gate requiring 100% test pass rate on compilation.
  - Automated Unit, Component, Conformance, and End-to-End (FFmpeg) test suites.

---

## Architecture Overview

```
                      +-------------------+
                      |   RTSP Publisher  |
                      |   (FFmpeg / OBS)  |
                      +---------+---------+
                                |  TCP 8554 / UDP Ingest
                                v
                      +-------------------+
                      |   IngestReactor   | (Thread 0)
                      +---------+---------+
                                |
                     Lock-Free SPSC Channels
                                |
             +------------------+------------------+
             |                                     |
             v                                     v
   +-------------------+                 +-------------------+
   |  EgressReactor 0  | (Thread 1)      |  EgressReactor 1  | (Thread 2)
   +---------+---------+                 +---------+---------+
             |                                     |
      TCP/UDP Egress                        TCP/UDP Egress
             |                                     |
             v                                     v
      [Subscribers 0..N]                   [Subscribers N..M]
```

- **IngestReactor**: Ingests raw RTP streams from publishers, validates headers, parses parameter sets (SPS/PPS), updates sequence accounting, and logs keyframes to the circular GOP cache.
- **PacketRouter**: Dispatches packet handles to assigned egress reactors across lock-free ring queues with cacheline-padded cursors to eliminate false sharing.
- **EgressReactor**: Manages subscriber sessions, drains transmit queues, formats TCP interleaved (`$ channel length`) or UDP datagrams, and flushes to client sockets.
- **NumaArena & PacketRef**: Pre-allocated slab pool guaranteeing zero runtime heap allocations in hot media loops.

---

## Building

### Requirements
- **Compiler**: C++20 compliant compiler (Clang 15+ or GCC 11+)
- **Build System**: Meson (0.60+) and Ninja (1.10+)
- **Platform Dependencies**:
  - macOS: Native POSIX sockets (Apple Silicon & Intel supported)
  - Linux: `liburing` (required on Linux for high-throughput zero-copy asynchronous I/O)
- **Optional**: Python 3 and FFmpeg (for automated E2E test suite)

### Native Build (macOS / Linux)

```bash
# 1. Setup build directory
meson setup build

# 2. Compile and execute build-time test gate
ninja -C build

# Binary is generated at ./build/rivet
```

### Build Configuration Options

Configure build options using `meson configure` or with `-Doption=value` during setup:

| Option | Type | Default | Description |
|---|---|---|---|
| `tests` | `feature` | `enabled` | Build Unit, Component, Conformance, and E2E test suites |
| `test_on_build` | `boolean` | `true` | Automatically run test gate during `ninja` compilation |
| `e2e` | `feature` | `auto` | Enable automated FFmpeg publisher-to-player E2E integration test |
| `coverage_gate` | `boolean` | `false` | Instrument binaries with coverage mapping (Clang/GCC) |
| `trace_probes` | `boolean` | `false` | Enable micro-latency trace probes throughout the pipeline |
| `tracy` | `boolean` | `false` | Enable live profiling zones with Tracy profiler |

Example with custom options:
```bash
meson setup build -Doptimization=3 -Db_ndebug=true -De2e=enabled
```

### Containerized Build (Docker / Linux)

A multi-stage container configuration is provided for Linux environments:

```bash
# Build dev environment and execute tests
docker compose up -d --build
docker exec -it rivet-dev-container ninja -C build_linux test
```

---

## Testing Suite

RIVET integrates a strict, multi-tiered testing suite verified at build time:

1. **Unit Tests (`tests/unit/`)**:
   - `test_config`: Tests `.env`, CLI flags, and environment variable precedence.
   - `test_core`: Tests `NumaArena` allocations, SPSC queue bounds, and routing.
   - `test_media`: Tests RFC 6184 NAL classification, SPS/PPS parameter set extraction, and AU assembly.
   - `test_memory`: Intrusive reference counting, slab exhaustion, and leak guards.
   - `test_rtp`: Sequence number wrapping (65,535 rollover), loss accounting, and jitter calculation.
   - `test_rtsp`: RTSP request parser, header validation, SDP generation, and transport negotiation.
2. **Component Tests (`tests/component/`)**:
   - Full client lifecycle (`OPTIONS` &rarr; `ANNOUNCE` &rarr; `SETUP` &rarr; `RECORD` &rarr; `TEARDOWN`).
   - Multi-subscriber concurrent fan-out validation with mock transport buffers.
3. **Conformance Tests (`tests/conformance/`)**:
   - RFC 2326 compliance: CSeq echoing, public options, valid transport headers, and status code verification.
4. **End-to-End Tests (`tests/e2e/`)**:
   - Automated integration test driving real FFmpeg processes to publish live video/audio patterns and validating successful playback stream consumption.

Run all tests manually:
```bash
ninja -C build test
```

---

## Quick Start Guide

### 1. Start the Server

```bash
./build/rivet
```

By default, RIVET binds to RTSP port `8554` and serves the mount path `/live/test`.

### 2. Publish a Live Stream

Publish a live stream using FFmpeg:

**Synthetic Test Pattern (Video + Audio):**
```bash
ffmpeg -re \
  -f lavfi -i testsrc=size=1280x720:rate=30 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000 \
  -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
  -g 30 -keyint_min 30 -sc_threshold 0 \
  -c:a aac -b:a 128k \
  -f rtsp -rtsp_transport tcp rtsp://localhost:8554/live/test
```

**Webcam / Microphone (macOS AVFoundation):**
```bash
ffmpeg -f avfoundation -framerate 30 -video_size 1280x720 -i "0:0" \
  -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
  -g 30 -keyint_min 30 -sc_threshold 0 \
  -c:a aac -b:a 128k \
  -f rtsp -rtsp_transport tcp rtsp://localhost:8554/live/test
```

### 3. Play the Stream

Play the live stream using `ffplay`, VLC, or GStreamer:

**TCP Interleaved Transport:**
```bash
ffplay -fflags nobuffer -flags low_delay -rtsp_transport tcp rtsp://localhost:8554/live/test
```

**UDP Transport:**
```bash
ffplay -fflags nobuffer -flags low_delay -rtsp_transport udp rtsp://localhost:8554/live/test
```

---

## Configuration Reference

Configuration can be supplied via command-line arguments, environment variables, or a `.env` file. Settings follow strict precedence:
`CLI flags > Shell Environment Variables > .env File > Defaults`.

| CLI Argument | Environment Variable | Default | Description |
|---|---|---|---|
| `-p, --port <port>` | `RIVET_RTSP_PORT` | `8554` | RTSP TCP control port |
| `-s, --stream <path>` | `RIVET_STREAM_NAME` | `live/test` | Stream mount path |
| `-n, --shards <count>` | `RIVET_EGRESS_SHARDS` | `2` | Number of worker threads for client fan-out |
| `--rtp-port <port>` | `RIVET_BASE_RTP_PORT` | `20000` | Base dedicated UDP RTP port |
| `--rtcp-port <port>` | `RIVET_BASE_RTCP_PORT` | `20001` | Base dedicated UDP RTCP port |
| `-e, --env-file <path>` | — | `.env` | Path to custom `.env` configuration file |
| — | `RIVET_WATCHDOG_TIMEOUT_MS` | `5000` | Supervisor watchdog heartbeat check interval (ms) |
| — | `RIVET_NUMA_CAPACITY` | `4096` | Slab allocator capacity in slabs |
| — | `RIVET_NUMA_SLAB_SIZE` | `2048` | Slab buffer size in bytes |
| — | `RIVET_MAX_REACTOR_BYTES` | `16777216` | Aggregate memory quota per reactor (16 MB) |
| — | `RIVET_MAX_PENDING_SENDS` | `512` | Per-subscriber packet queue bound |
| — | `RIVET_STARTUP_POLICY` | `FastStartup` | Startup mode (`FastStartup` keyframe burst or `FreshnessFirst`) |
| — | `RIVET_DROP_POLICY` | `DependencyAware` | Congestion drop policy (`DependencyAware` or `Aggressive`) |
| — | `RIVET_LOG_LEVEL` | `INFO` | Logging verbosity (`INFO`, `DEBUG`, `WARN`) |

---

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.

