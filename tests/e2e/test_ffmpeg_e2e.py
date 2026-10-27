#!/usr/bin/env python3
"""
RIVET Tier 4 E2E Integration Test: FFmpeg Ingest & Egress Loopback
Validates full real-world streaming pipeline:
  FFmpeg (testsrc) -> RTSP TCP -> RIVET -> RTSP TCP -> FFmpeg (null sink)
"""

import os
import sys
import time
import socket
import signal
import subprocess

def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]

def wait_for_port(port, timeout=5.0):
    start = time.time()
    while time.time() - start < timeout:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=0.2):
                return True
        except (socket.error, ConnectionRefusedError):
            time.sleep(0.05)
    return False

def main():
    if len(sys.argv) < 2:
        print("Usage: test_ffmpeg_e2e.py <path_to_rivet_binary>")
        sys.exit(1)

    rivet_bin = sys.argv[1]
    if not os.path.isfile(rivet_bin):
        print(f"Error: rivet binary not found at {rivet_bin}")
        sys.exit(1)

    ffmpeg_bin = "/opt/homebrew/bin/ffmpeg"
    if not os.path.isfile(ffmpeg_bin):
        res = subprocess.run(["which", "ffmpeg"], capture_output=True, text=True)
        if res.returncode == 0:
            ffmpeg_bin = res.stdout.strip()
        else:
            print("FFmpeg not installed. Skipping Tier 4 E2E test.")
            sys.exit(0)

    rtsp_port = find_free_port()
    base_rtp  = find_free_port()
    base_rtcp = base_rtp + 1

    stream_name = "live/e2e_test"
    rtsp_url = f"rtsp://127.0.0.1:{rtsp_port}/{stream_name}"

    print("================================================================================")
    print("                RIVET Tier 4: FFmpeg End-to-End Integration Test                ")
    print(f"RTSP Port: {rtsp_port} | Base RTP: {base_rtp} | Stream: {stream_name}")
    print("================================================================================")

    # 1. Start RIVET Server
    server_cmd = [
        rivet_bin,
        "-p", str(rtsp_port),
        "-s", stream_name,
        "-n", "1",
        "--rtp-port", str(base_rtp),
        "--rtcp-port", str(base_rtcp)
    ]
    server_proc = subprocess.Popen(server_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    try:
        if not wait_for_port(rtsp_port, timeout=5.0):
            print("Error: RIVET server failed to bind port within 5s")
            server_proc.kill()
            out, _ = server_proc.communicate()
            print("Server output:\n", out)
            sys.exit(1)

        print("[OK] RIVET server started and listening.")

        # 2. Launch FFmpeg Publisher
        # Publishes 30 fps 320x240 testsrc video via interleaved RTSP
        pub_cmd = [
            ffmpeg_bin,
            "-re",
            "-f", "lavfi", "-i", "testsrc=size=320x240:rate=30",
            "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
            "-g", "30",
            "-pix_fmt", "yuv420p",
            "-f", "rtsp",
            "-rtsp_transport", "tcp",
            rtsp_url
        ]
        pub_proc = subprocess.Popen(pub_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)

        # Allow publisher to establish connection and ingest keyframe
        time.sleep(1.5)
        if pub_proc.poll() is not None:
            _, err = pub_proc.communicate()
            print("Error: Publisher failed to start:\n", err)
            sys.exit(1)

        print("[OK] FFmpeg publisher active, publishing video frames.")

        # 3. Launch FFmpeg Subscriber
        # Subscribes to RIVET and decodes frames for 2 seconds
        sub_cmd = [
            ffmpeg_bin,
            "-rtsp_transport", "tcp",
            "-i", rtsp_url,
            "-t", "2",
            "-f", "null", "-"
        ]
        sub_proc = subprocess.run(sub_cmd, capture_output=True, text=True, timeout=10)

        if sub_proc.returncode != 0:
            print("Error: FFmpeg subscriber failed to play stream:")
            print("STDERR:\n", sub_proc.stderr)
            sys.exit(1)

        print("[OK] FFmpeg subscriber successfully received, decoded, and rendered stream.")

    finally:
        # Cleanup processes
        if 'pub_proc' in locals() and pub_proc.poll() is None:
            pub_proc.send_signal(signal.SIGINT)
            try:
                pub_proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                pub_proc.kill()

        if server_proc.poll() is None:
            server_proc.send_signal(signal.SIGINT)
            try:
                server_proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                server_proc.kill()

    print("\n================================================================================")
    print("                TIER 4 E2E INTEGRATION TEST PASSED SUCCESSFULLY                 ")
    print("================================================================================")
    return 0

if __name__ == "__main__":
    main()
