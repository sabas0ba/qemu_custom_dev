#!/usr/bin/env bash
# End-to-end test: renderd + demo over an AF_UNIX socket.
#
# Exercises the full protocol path (HELLO, CREATE_SURFACE, CLEAR, FILL_RECT,
# PRESENT, GOODBYE) without QEMU: the unix transport substitutes for vsock,
# which is exactly the transport-swap property the protocol is designed for.
# Pixel expectations must stay in sync with the scene in guest/user/demo.c.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
TMP="$ROOT/.tmp/e2e"
SOCK="$TMP/renderd.sock"

rm -rf "$TMP"
mkdir -p "$TMP"

"$BUILD/renderd" --unix "$SOCK" --out "$TMP" --once &
RENDERD_PID=$!
trap 'kill "$RENDERD_PID" 2>/dev/null || true; wait "$RENDERD_PID" 2>/dev/null || true' EXIT

for _ in $(seq 1 100); do
    [ -S "$SOCK" ] && break
    sleep 0.05
done
[ -S "$SOCK" ] || { echo "e2e: renderd did not create socket" >&2; exit 1; }

"$BUILD/demo" --unix "$SOCK"

wait "$RENDERD_PID"
trap - EXIT

PPM="$TMP/frame-000001.ppm"
[ -f "$PPM" ] || { echo "e2e: missing $PPM" >&2; exit 1; }

# Background, red rect (40,40 80x60), green rect (160,120 100x80),
# plus points just outside each rect to catch off-by-one clipping bugs.
"$BUILD/ppm_check" "$PPM" 5 5 102030
"$BUILD/ppm_check" "$PPM" 60 60 ff0000
"$BUILD/ppm_check" "$PPM" 119 99 ff0000
"$BUILD/ppm_check" "$PPM" 120 100 102030
"$BUILD/ppm_check" "$PPM" 200 150 00ff00
"$BUILD/ppm_check" "$PPM" 259 199 00ff00
"$BUILD/ppm_check" "$PPM" 159 119 102030

echo "e2e: OK"
