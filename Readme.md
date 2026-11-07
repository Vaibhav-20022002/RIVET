# RIVET

__RIVET__ (__Real-time Intelligent Video Engine for Transmission__) is a zero-allocation, multi-process RTSP 1.0 / RTP / RTCP media relay server written in C++20 for ultra-low-latency live video and audio fan-out over TCP (RFC 2326 `$` interleaved) and dedicated UDP.

---

## Key Features

- **Protocols & Codec Support Matrix**:
  - **RTSP 1.0 (RFC 2326)** control plane (`OPTIONS`, `ANNOUNCE`, `DESCRIBE`, `SETUP`, `RECORD`, `PLAY`, `GET_PARAMETER`, `TEARDOWN`) with optional RFC 2617 MD5 Digest Authentication for publishers.
  - **RTP/RTCP (RFC 3550 / RFC 4585)** data & feedback plane with 2-packet probation, 128-bit duplicate bitmap, 32-bit wrap-safe jitter estimation, RTCP SR/RR/BYE/PLI, and non-mutating Mode A (`TransparentPassThrough`) / Mode B (`TranslatedContext`) header emission.
  - **H.264 / AVC (RFC 6184)**: Single NAL, STAP-A, and FU-A classification with in-band SPS/PPS tracking and `sprop-parameter-sets` SDP generation.
  - **H.265 / HEVC (RFC 7798)**: 64-entry `consteval` NAL LUT, Single NAL, Aggregation Packets (`AP`, Type 48), Fragmentation Units (`FU`, Type 49), `PACI` (Type 50), `DONL`/`DOND` (`sprop-max-don-diff > 0`, `tx-mode=SRST`), bounded 4 KiB FU parameter-set reassembly, emulation-prevention-aware VPS/SPS/PPS PTL parsing (`profile-space`, `tier-flag`, `profile-id`, `level-id`, `sprop-vps`, `sprop-sps`, `sprop-pps`), Open-GOP `CRA`/`BLA` fast startup with automatic leading-`RASL` gating, and `TemporalId` sub-layer congestion shedding with `TSA`/`STSA` step-up hysteresis.
  - **Multi-Track Audio / Opaque Media**: Simultaneous Video + Audio (`MPEG4-GENERIC` AAC, `OPUS`, `PCMU`, `PCMA`, or opaque RTP) with per-track SoA subscriber partitioning.
- **Process-per-Input-Stream Isolation (`MultiProcess` Default)**:
  - Lightweight non-blocking **`RivetController`** supervisor accepts TCP RTSP connections, validates framing/auth/SDP pre-handoff, spawns an isolated **`RivetWorker`** OS process (`fork()` + `execv("--worker")`) per active stream, and transfers publisher/viewer sockets via `AF_UNIX` `SCM_RIGHTS` with zero steady-state IPC copies.
  - Sub-50 ms worker crash detection (`SIGKILL` / UDS EOF) with complete fault isolation across concurrent streams.
- **Zero-Allocation Multi-Reactor Hot Path**:
  - Dedicated **Thread 0 `IngestReactor`** decoupled from **Threads `1..S` `EgressReactor`** shards via cache-line-padded lock-free SPSC ring queues.
  - Multi-class slab pool (**`MultiClassArena` / `NumaArena`**) with lock-free cross-thread Treiber recycling (`2 KiB` standard + `64 KiB` jumbo slabs) and intrusive 16-byte **`PacketRef`** handles (`0` steady-state heap allocations per forwarded packet).
  - Non-blocking platform event polling (`kqueue` on macOS/BSD, `epoll` on Linux, optional Linux `io_uring`), SIMD (`ARM NEON` / `AVX2`) SoA subscriber eligibility filtering, and whole-Access-Unit (`AuGate`) DPB-preserving congestion shedding.

---

## Architecture Overview

```
                     +----------------------------------+
                     |         RivetController          | (Master Supervisor Process)
                     |  Non-blocking RTSP Accept/Auth   |
                     +--------+----------------+--------+
         AF_UNIX SCM_RIGHTS   |                |   AF_UNIX SCM_RIGHTS
         (FD Handoff Only)    v                v   (FD Handoff Only)
             +--------------------+        +--------------------+
             |   RivetWorker A    |        |   RivetWorker B    | (Isolated OS Processes)
             |  /live/cam_front   |        |   /live/cam_rear   |
             +---------+----------+        +--------------------+
                       |
        +--------------+--------------+
        |   Thread 0: IngestReactor   |  (Non-blocking TCP `$` / UDP recvmmsg,
        |   + Byte-Budget PacketRing  |   SPS/VPS/PPS staging, RAP Ring)
        +--------------+--------------+
                       |  Lock-Free SPSC Queues (Zero-Copy PacketRef)
            +----------+----------+
            v                     v
   +-----------------+   +-----------------+
   | EgressReactor 0 |   | EgressReactor 1 |  (Dedicated Shard OS Threads,
   |  NEON/AVX2 SoA  |   |  NEON/AVX2 SoA  |   AuGate Whole-AU Drop,
   +--------+--------+   +--------+--------+   writev / sendmmsg / io_uring)
            |                     |
            v                     v
     [Viewers 0..N]        [Viewers N..M]
```

---

## Building

### Requirements
- **Compiler**: C++20 compiler (Clang 15+ or GCC 12+)
- **Build System**: Meson (0.63+) and Ninja (1.10+)
- **Platform**: macOS (`kqueue`, Apple Silicon NEON & x86_64) and Linux (`epoll`, optional `liburing` with `-Dio_uring=enabled`)
- **Optional**: Python 3 + FFmpeg (`libx264` / `libx265`) for Tier 4 E2E integration tests

### Release Build, Unit/Component/Multi-Process Tests & Zero-Alloc Benchmarks

```bash
# 1. Configure Release build (-O3 -DNDEBUG by default) with benchmarks
meson setup build-release --buildtype=release -Dbenchmarks=true

# 2. Compile server, tests, and benchmarks
meson compile -C build-release

# 3. Run Unit, Conformance, Component, Multi-Process, Fuzz Smoke, and FFmpeg E2E suites
meson test -C build-release --print-errorlogs

# 4. Run micro & pipeline benchmarks (verifies 0 steady-state heap allocations/packet)
./build-release/bench/rivet_bench --benchmark_min_time=0.05s
```

### Sanitizer Builds (`ASan + UBSan` and `TSan`)

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
meson setup build-asan --buildtype=debugoptimized -Db_sanitize=address,undefined -Db_lundef=false
meson compile -C build-asan
meson test -C build-asan --print-errorlogs

# ThreadSanitizer
meson setup build-tsan --buildtype=debugoptimized -Db_sanitize=thread
meson compile -C build-tsan
meson test -C build-tsan --print-errorlogs
```

### Build Options (`meson_options.txt`)

| Option | Type | Default | Description |
|---|---|---|---|
| `tests` | `feature` | `enabled` | Build Unit, Conformance, Component, Multi-Process, and Fuzz Smoke tests |
| `test_on_build` | `boolean` | `false` | Run test gate automatically as a build target |
| `e2e` | `feature` | `auto` | Enable FFmpeg H.264 & HEVC publisher-to-viewer E2E test matrix |
| `benchmarks` | `boolean` | `false` | Build `rivet_bench` Google Benchmark micro & zero-alloc pipeline suite |
| `fuzz` | `boolean` | `false` | Build standalone `-fsanitize=fuzzer` binaries (Clang/libFuzzer) |
| `io_uring` | `feature` | `disabled` | Optional Linux `liburing` asynchronous I/O backend |
| `tracy` | `boolean` | `false` | Enable Tracy profiler instrumentation (`-Dtracy_dir=/path/to/tracy`) |

---

## Quick Start Guide

### 1. Start RIVET

```bash
./build-release/rivet -p 8554 -n 2
```

### 2. Publish Live H.264 or HEVC (`libx265`) via FFmpeg

**H.264 + AAC over TCP Interleaved:**
```bash
ffmpeg -re \
  -f lavfi -i testsrc=size=1280x720:rate=30 \
  -f lavfi -i sine=frequency=1000:sample_rate=48000 \
  -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 30 \
  -c:a aac -b:a 128k \
  -f rtsp -rtsp_transport tcp rtsp://127.0.0.1:8554/live/cam1
```

**HEVC / H.265 (Closed-GOP or Open-GOP `CRA`) over TCP or UDP:**
```bash
ffmpeg -re \
  -f lavfi -i testsrc=size=1280x720:rate=30 \
  -c:v libx265 -preset ultrafast -pix_fmt yuv420p \
  -x265-params "keyint=30:min-keyint=15:open-gop=1:repeat-headers=1" \
  -f rtsp -rtsp_transport tcp rtsp://127.0.0.1:8554/live/hevc_cam
```

### 3. Play Any Active Stream

```bash
# TCP Interleaved
ffplay -fflags nobuffer -flags low_delay -rtsp_transport tcp rtsp://127.0.0.1:8554/live/hevc_cam

# Dedicated UDP
ffplay -fflags nobuffer -flags low_delay -rtsp_transport udp rtsp://127.0.0.1:8554/live/hevc_cam
```

---

## Configuration Reference

Settings follow strict precedence: `CLI Flags > Environment Variables > .env File > Defaults`.

| CLI Argument | Environment Variable | Default | Description |
|---|---|---|---|
| `-p, --port <1..65535>` | `RIVET_RTSP_PORT` | `8554` | RTSP TCP listener port |
| `-s, --stream <path>` | `RIVET_STREAM_NAME` | `live/test` | Default stream mount path |
| `-n, --shards <1..64>` | `RIVET_EGRESS_SHARDS` | `2` | Number of `EgressReactor` shard threads per worker |
| `--rtp-port <port>` | `RIVET_BASE_RTP_PORT` | `20000` | Base dedicated UDP RTP port (`0` = OS ephemeral) |
| `--rtcp-port <port>` | `RIVET_BASE_RTCP_PORT` | `20001` | Base dedicated UDP RTCP port |
| `--in-process` | `RIVET_EXECUTION_MODE` | `MultiProcess` | `MultiProcess` (`fork`+`execv`) or `InProcess` |
| `-e, --env-file <path>` | — | `.env` | Path to `.env` configuration file |
| — | `RIVET_PUBLISH_USER` | *(empty)* | Optional RFC 2617 Digest Auth publisher username |
| — | `RIVET_PUBLISH_PASSWORD` | *(empty)* | Optional RFC 2617 Digest Auth publisher password |
| — | `RIVET_ADVERTISED_IP` | *(empty)* | Explicit SDP `c=IN IP4` address (`0.0.0.0` if unset) |
| — | `RIVET_NUMA_CAPACITY` | `4096` | Standard `2 KiB` slab count per worker arena |
| — | `RIVET_RETENTION_BUDGET_BYTES` | `16777216` | Per-stream `PacketRing` GOP retention byte budget (16 MiB) |
| — | `RIVET_BURST_BUDGET_BYTES` | `2097152` | Max historical keyframe catch-up burst per join (2 MiB) |
| — | `RIVET_MAX_SUBSCRIBER_QUEUE_BYTES` | `2097152` | Per-subscriber egress queue byte ceiling (2 MiB) |
| — | `RIVET_MAX_REACTOR_BYTES` | `16777216` | Aggregate per-shard egress byte quota (16 MiB) |
| — | `RIVET_JOIN_ON_OPEN_IRAP` | `true` | Permit fast join on HEVC `CRA`/`BLA_W_LP` with `RASL` gate |
| — | `RIVET_STARTUP_POLICY` | `FastStartup` | `FastStartup` (`FastPictureFirst`) or `FreshnessFirst` |
| — | `RIVET_DROP_POLICY` | `DependencyAware` | `DependencyAware` (`AuGate` whole-AU) or `TailDrop` |
| — | `RIVET_CLIENT_TIMEOUT_MS` | `60000` | Idle RTSP viewer session timeout (ms) |
| — | `RIVET_LOG_LEVEL` | `INFO` | Logging verbosity (`DEBUG`, `INFO`, `WARN`, `ERROR`) |

---

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
