#!/usr/bin/env python3
import sys
import subprocess
import argparse
import os

def main():
    parser = argparse.ArgumentParser(description="RIVET Build-Time Test Gate Runner")
    parser.add_argument("--stamp", required=True, help="Path to output stamp file on success")
    parser.add_argument("--suites", default="unit", help="Comma-separated suite names")
    parser.add_argument("executables", nargs="+", help="Test executables to run")
    args = parser.parse_args()

    print("================================================================================")
    print("                    RIVET Build-Time Test Gate Runner                           ")
    print("================================================================================")

    failed = False
    for exe in args.executables:
        if not os.path.isfile(exe):
            print(f"[GATE ERROR] Executable not found: {exe}", file=sys.stderr)
            sys.exit(1)

        name = os.path.basename(exe)
        print(f"\n[RUNNING SUITE] {name}...")
        sys.stdout.flush()

        res = subprocess.run([exe, "--gtest_brief=1"])
        if res.returncode != 0:
            print(f"[GATE FAILED] Suite {name} failed with return code {res.returncode}", file=sys.stderr)
            failed = True
            break
        else:
            print(f"[GATE PASSED] {name} completed successfully.")

    if failed:
        if os.path.exists(args.stamp):
            os.remove(args.stamp)
        sys.exit(1)

    with open(args.stamp, "w") as f:
        f.write("PASSED\n")

    print("\n================================================================================")
    print("                  ALL BUILD-TIME TESTS PASSED SUCCESSFULLY                      ")
    print("================================================================================")
    return 0

if __name__ == "__main__":
    sys.exit(main())
