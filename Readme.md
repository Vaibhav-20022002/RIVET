# RIVET

__RIVET__ (__Real-time Intelligent Video Engine for Transmission__) is a lightweight RTSP/RTP media server written in C++20 for streaming multimedia. It supports stream publishing and playback over both TCP (interleaved) and UDP.

## Features

- **Protocols**: RTSP 1.0 (RFC 2326) control plane with RTP/RTCP (RFC 3550) data transport.
- **Transports**: TCP interleaved framing (`$ + channel + length`) and dedicated UDP port pairs.
- **Codecs**: H.264 video (single NAL units and FU-A fragmentation units) and AAC audio.
- **Multi-Track**: Simultaneous video and audio streaming with independent sequence tracking.
- **Architecture**: Multi-reactor design with lock-free SPSC channels between ingest and egress shards.
- **Memory**: Fixed-slab allocation pool for network packets with intrusive reference counting.
- **Configuration**: Runtime configuration via `.env` files, environment variables, or command-line arguments.
- **Platforms**: Linux (with optional io_uring support) and macOS.

## Building

### Requirements
- C++20 compiler (Clang 15+ or GCC 11+)
- Meson (0.60+)
- Ninja (1.10+)
- `liburing` (Linux only)

### Native Build (macOS / Linux)

```bash
# Setup build directory
meson setup build

# Compile
ninja -C build
```

The compiled binary will be placed at `./build/rivet`.

### Docker Build (Linux)

A Dockerfile and compose file are provided for containerized Linux builds:

```bash
docker compose up -d --build
docker exec -it rivet-dev-container ninja -C build_linux
```

## Quick Start

### 1. Start the Server

```bash
./build/rivet
```

By default, RIVET listens for RTSP requests on TCP port `8554` and serves the mount path `/live/test`.

### 2. Publish a Stream

You can publish a live stream to RIVET using FFmpeg.

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

**Webcam and Microphone (macOS AVFoundation):**
```bash
ffmpeg -f avfoundation -framerate 30 -video_size 1280x720 -i "0:0" \
  -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p \
  -g 30 -keyint_min 30 -sc_threshold 0 \
  -c:a aac -b:a 128k \
  -f rtsp -rtsp_transport tcp rtsp://localhost:8554/live/test
```

### 3. Play the Stream

Play the live stream using `ffplay`, VLC, or GStreamer.

**Over TCP:**
```bash
ffplay -fflags nobuffer -flags low_delay -rtsp_transport tcp rtsp://localhost:8554/live/test
```

**Over UDP:**
```bash
ffplay -fflags nobuffer -flags low_delay -rtsp_transport udp rtsp://localhost:8554/live/test
```

## Configuration

RIVET can be configured using command-line arguments, environment variables, or a `.env` file. Settings follow this precedence order:
`CLI flags > Shell Environment Variables > .env File > Defaults`.

| CLI Argument | Environment Variable | Default | Description |
|---|---|---|---|
| `-p, --port` | `RIVET_RTSP_PORT` | `8554` | RTSP TCP control port |
| `-s, --stream` | `RIVET_STREAM_NAME` | `live/test` | Stream mount path |
| `-n, --shards` | `RIVET_EGRESS_SHARDS` | `2` | Egress worker threads |
| `--rtp-port` | `RIVET_BASE_RTP_PORT` | `20000` | Base UDP RTP port |
| `--rtcp-port` | `RIVET_BASE_RTCP_PORT` | `20001` | Base UDP RTCP port |
| `-e, --env-file` | — | `.env` | Path to custom `.env` file |
| — | `RIVET_WATCHDOG_TIMEOUT_MS` | `5000` | Watchdog check interval (ms) |
| — | `RIVET_NUMA_CAPACITY` | `4096` | Slab allocator capacity (slabs) |
| — | `RIVET_NUMA_SLAB_SIZE` | `2048` | Slab size in bytes |
| — | `RIVET_MAX_REACTOR_BYTES` | `16777216` | Memory quota per reactor (bytes) |
| — | `RIVET_MAX_PENDING_SENDS` | `512` | Max queued packets per subscriber |
| — | `RIVET_STARTUP_POLICY` | `FreshnessFirst` | Startup mode (`FreshnessFirst` or `FastStartup`) |
| — | `RIVET_DROP_POLICY` | `DependencyAware` | Congestion drop policy (`DependencyAware` or `Aggressive`) |
| — | `RIVET_LOG_LEVEL` | `INFO` | Logging verbosity (`INFO`, `DEBUG`, `WARN`) |

## Architecture Overview

```
                      +-------------------+
                      |   RTSP Publisher  |
                      |   (FFmpeg / OBS)  |
                      +---------+---------+
                                |  TCP 8554 / UDP
                                v
                      +-------------------+
                      |   IngestReactor   | (Thread 0)
                      +---------+---------+
                                |
                   Lock-free SPSC Ring Buffer
                                |
             +------------------+------------------+
             |                                     |
             v                                     v
   +-------------------+                 +-------------------+
   |   EgressReactor 0 | (Thread 1)      |   EgressReactor 1 | (Thread 2)
   +---------+---------+                 +---------+---------+
             |                                     |
      TCP/UDP Egress                        TCP/UDP Egress
             |                                     |
             v                                     v
       [Subscriber A]                        [Subscriber B]
```

- **IngestReactor**: Handles the publisher connection, unwraps RTP sequence numbers, caches SPS/PPS parameter sets, and forwards packets across wait-free Single-Producer Single-Consumer (SPSC) ring buffers.
- **PacketRouter**: Dispatches packet handles to egress reactor queues with cacheline-aligned cursors to prevent false sharing.
- **EgressReactors**: Worker threads that each own a disjoint partition of subscriber sockets, managing client send queues and network dispatch without cross-thread lock contention.
- **Slab Pool (NumaArena)**: Fixed-size buffer allocator providing deterministic packet memory reuse without heap fragmentation.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for details.
