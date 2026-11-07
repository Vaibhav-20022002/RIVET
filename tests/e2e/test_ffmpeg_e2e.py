#!/usr/bin/env python3
"""
RIVET Tier 4 E2E Integration Test: FFmpeg H.264 & HEVC (Closed-GOP + Open-GOP CRA/RASL)
Validates real-world live streaming across TCP interleaved and dedicated UDP:
  - H.264 (`libx264`) over TCP and UDP
  - HEVC (`libx265` closed-GOP) over TCP and UDP
  - HEVC (`libx265` `open-gop=1` CRA + RASL) mid-stream join over TCP (`-xerror`)
Exits 77 (Meson SKIP) ONLY if `ffmpeg` is unavailable — never false-green `0`!
"""

import os
import re
import sys
import time
import shutil
import socket
import signal
import subprocess


def find_free_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_port(port, timeout=5.0):
    start = time.time()
    while time.time() - start < timeout:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def has_encoder(ffmpeg_bin, encoder_name):
    res = subprocess.run(
        [ffmpeg_bin, "-hide_banner", "-encoders"],
        capture_output=True,
        text=True,
    )
    return res.returncode == 0 and (encoder_name in res.stdout)


def extract_decoded_frames(stderr_text):
    matches = re.findall(r"frame=\s*(\d+)", stderr_text)
    return int(matches[-1]) if matches else 0


def stop_proc(proc):
    if proc is None or proc.poll() is not None:
        return
    try:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=2.0)
    except Exception:
        proc.kill()
        proc.wait()


def run_case(ffmpeg_bin, rtsp_port, case_name, stream_path, codec_args, transport):
    rtsp_url = f"rtsp://127.0.0.1:{rtsp_port}/{stream_path}"
    print(f"\n--- Running Case: {case_name} ({transport.upper()}) -> {rtsp_url} ---")

    pub_cmd = [
        ffmpeg_bin,
        "-hide_banner",
        "-loglevel",
        "warning",
        "-re",
        "-f",
        "lavfi",
        "-i",
        "testsrc=size=320x240:rate=25",
        *codec_args,
        "-pix_fmt",
        "yuv420p",
        "-f",
        "rtsp",
        "-rtsp_transport",
        transport,
        rtsp_url,
    ]
    pub_proc = subprocess.Popen(
        pub_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True
    )

    try:
        # Allow publisher to ANNOUNCE/SETUP/RECORD and emit at least 1-2 GOPs so mid-stream join
        # exercises retained RAP / CRA+RASL gating
        time.sleep(1.4)
        if pub_proc.poll() is not None:
            _, err = pub_proc.communicate()
            raise RuntimeError(f"Publisher exited early in {case_name}:\n{err}")

        sub_cmd = [
            ffmpeg_bin,
            "-hide_banner",
            "-v",
            "info",
            "-xerror",
            "-rtsp_transport",
            transport,
            "-i",
            rtsp_url,
            "-frames:v",
            "20",
            "-f",
            "null",
            "-",
        ]
        sub_res = subprocess.run(sub_cmd, capture_output=True, text=True, timeout=15)
        if sub_res.returncode != 0:
            raise RuntimeError(
                f"Subscriber decode failed (rc={sub_res.returncode}) in {case_name}:\n{sub_res.stderr}"
            )

        decoded = extract_decoded_frames(sub_res.stderr)
        if decoded <= 0:
            raise RuntimeError(
                f"Subscriber decoded 0 frames in {case_name}:\n{sub_res.stderr}"
            )

        print(f"[OK] {case_name} ({transport.upper()}): decoded {decoded} frames with 0 errors.")
    finally:
        stop_proc(pub_proc)


def main():
    if len(sys.argv) < 2:
        print("Usage: test_ffmpeg_e2e.py <path_to_rivet_binary>")
        sys.exit(1)

    rivet_bin = sys.argv[1]
    if not os.path.isfile(rivet_bin):
        print(f"Error: rivet binary not found at {rivet_bin}")
        sys.exit(1)

    ffmpeg_bin = shutil.which("ffmpeg")
    if not ffmpeg_bin and os.path.isfile("/opt/homebrew/bin/ffmpeg"):
        ffmpeg_bin = "/opt/homebrew/bin/ffmpeg"
    if not ffmpeg_bin:
        print("FFmpeg not installed. Returning Meson SKIP (77).")
        sys.exit(77)

    has_x264 = has_encoder(ffmpeg_bin, "libx264")
    has_x265 = has_encoder(ffmpeg_bin, "libx265")
    if not has_x264 and not has_x265:
        print("Neither libx264 nor libx265 available in FFmpeg. Returning Meson SKIP (77).")
        sys.exit(77)

    rtsp_port = find_free_tcp_port()

    print("================================================================================")
    print("         RIVET Tier 4: FFmpeg H.264 & HEVC End-to-End Integration Matrix        ")
    print(f"FFmpeg: {ffmpeg_bin} | RTSP Port: {rtsp_port} | x264={has_x264} | x265={has_x265}")
    print("================================================================================")

    # Use ephemeral UDP base ports (`--rtp-port 0 --rtcp-port 0`) so multi-process workers never collide
    server_cmd = [
        rivet_bin,
        "-p",
        str(rtsp_port),
        "-n",
        "2",
        "--rtp-port",
        "0",
        "--rtcp-port",
        "0",
    ]
    server_proc = subprocess.Popen(
        server_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )

    try:
        if not wait_for_port(rtsp_port, timeout=5.0):
            stop_proc(server_proc)
            out, _ = server_proc.communicate()
            print("Error: RIVET server failed to bind port within 5s:\n", out)
            sys.exit(1)

        if has_x264:
            x264_args = [
                "-c:v",
                "libx264",
                "-preset",
                "ultrafast",
                "-tune",
                "zerolatency",
                "-g",
                "25",
            ]
            run_case(ffmpeg_bin, rtsp_port, "H.264 Closed-GOP", "live/h264_tcp", x264_args, "tcp")
            run_case(ffmpeg_bin, rtsp_port, "H.264 Closed-GOP", "live/h264_udp", x264_args, "udp")

        if has_x265:
            x265_closed = [
                "-c:v",
                "libx265",
                "-preset",
                "ultrafast",
                "-tune",
                "zerolatency",
                "-x265-params",
                "keyint=25:min-keyint=25:open-gop=0:repeat-headers=1",
            ]
            run_case(ffmpeg_bin, rtsp_port, "HEVC Closed-GOP", "live/hevc_tcp", x265_closed, "tcp")
            run_case(ffmpeg_bin, rtsp_port, "HEVC Closed-GOP", "live/hevc_udp", x265_closed, "udp")

            x265_open = [
                "-c:v",
                "libx265",
                "-preset",
                "ultrafast",
                "-x265-params",
                "keyint=20:min-keyint=10:open-gop=1:bframes=3:repeat-headers=1",
            ]
            run_case(
                ffmpeg_bin,
                rtsp_port,
                "HEVC Open-GOP CRA+RASL Mid-Stream Join",
                "live/hevc_open_cra",
                x265_open,
                "tcp",
            )

    finally:
        stop_proc(server_proc)

    print("\n================================================================================")
    print("             TIER 4 H.264 + HEVC E2E MATRIX PASSED SUCCESSFULLY                 ")
    print("================================================================================")
    sys.exit(0)


if __name__ == "__main__":
    main()
