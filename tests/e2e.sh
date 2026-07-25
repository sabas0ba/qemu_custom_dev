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

TCP_PORT=15501

rm -rf "$TMP"
mkdir -p "$TMP"

# Verify the demo scene in a PPM frame. Background, red rect (40,40 80x60),
# green rect (160,120 100x80), plus points just outside each rect to catch
# off-by-one clipping bugs. Keep in sync with guest/user/demo.c.
check_frame() {
    local ppm="$1"

    [ -f "$ppm" ] || { echo "e2e: missing $ppm" >&2; exit 1; }
    "$BUILD/ppm_check" "$ppm" 5 5 102030
    "$BUILD/ppm_check" "$ppm" 60 60 ff0000
    "$BUILD/ppm_check" "$ppm" 119 99 ff0000
    "$BUILD/ppm_check" "$ppm" 120 100 102030
    "$BUILD/ppm_check" "$ppm" 200 150 00ff00
    "$BUILD/ppm_check" "$ppm" 259 199 00ff00
    "$BUILD/ppm_check" "$ppm" 159 119 102030
}

# --- unix transport ---
mkdir -p "$TMP/unix"
"$BUILD/renderd" --unix "$SOCK" --out "$TMP/unix" --once &
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
check_frame "$TMP/unix/frame-000001.ppm"
echo "e2e: unix transport OK"

# --- tcp transport (same protocol, different transport) ---
mkdir -p "$TMP/tcp"
"$BUILD/renderd" --tcp "$TCP_PORT" --out "$TMP/tcp" --once &
RENDERD_PID=$!
trap 'kill "$RENDERD_PID" 2>/dev/null || true; wait "$RENDERD_PID" 2>/dev/null || true' EXIT

ok=0
for _ in $(seq 1 100); do
    if "$BUILD/demo" --tcp 127.0.0.1 "$TCP_PORT" 2>/dev/null; then
        ok=1
        break
    fi
    sleep 0.05
done
[ "$ok" = 1 ] || { echo "e2e: could not connect over tcp" >&2; exit 1; }
wait "$RENDERD_PID"
trap - EXIT
check_frame "$TMP/tcp/frame-000001.ppm"
echo "e2e: tcp transport OK"

# --- shared-memory transport (Phase 2; over a plain file, no VM) ---
mkdir -p "$TMP/shm"
"$BUILD/renderd" --shm "$TMP/shm/region.bin" --out "$TMP/shm" --once &
RENDERD_PID=$!
trap 'kill "$RENDERD_PID" 2>/dev/null || true; wait "$RENDERD_PID" 2>/dev/null || true' EXIT

for _ in $(seq 1 100); do
    [ -f "$TMP/shm/region.bin" ] && break
    sleep 0.05
done
"$BUILD/demo" --shm-file "$TMP/shm/region.bin"
wait "$RENDERD_PID"
trap - EXIT
check_frame "$TMP/shm/frame-000001.ppm"
echo "e2e: shm transport OK"

echo "e2e: OK"
