#!/usr/bin/env bash
# Full-stack test: boot the real guest image under QEMU and run the demo
# from INSIDE the guest against renderd on the host.
#
# Always tests the tcp transport (works without KVM or vhost-vsock, e.g.
# in containers; the guest reaches the host loopback via the slirp gateway
# 10.0.2.2) and the Phase 2 ivshmem shared-memory transport (ivshmem-plain
# needs no vhost support either). When /dev/vhost-vsock is available (bare
# metal, GitHub Actions runners after `modprobe vhost_vsock`), the vsock
# transport — the Phase 1 transport — is tested in the same boot.
#
# The guest runs unattended: cloud-init's runcmd executes the statically
# linked demo from the 9p-shared repo and powers the guest off. No network
# access or toolchain is needed inside the guest.
#
# Requires: qemu-system-x86_64, qemu-img, an ISO tool (see
# make-guest-image.sh), and the ~600 MB base image download (cached under
# .tmp/cache). Takes ~1 min with KVM, several minutes under TCG.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
TMP="$ROOT/.tmp/vm-e2e"
TCP_PORT=15502
VSOCK_PORT=15503
BOOT_TIMEOUT="${BOOT_TIMEOUT:-1500}"

HAVE_VSOCK=0
[ -e /dev/vhost-vsock ] && [ -w /dev/vhost-vsock ] && HAVE_VSOCK=1
ACCEL=tcg
[ -e /dev/kvm ] && [ -w /dev/kvm ] && ACCEL=kvm
echo "vm-e2e: accel=$ACCEL vsock=$HAVE_VSOCK"

rm -rf "$TMP"
mkdir -p "$TMP/frames-tcp" "$TMP/frames-vsock" "$TMP/frames-shm"

make -C "$ROOT" all build/demo-static >/dev/null

# The demo binary appears via the 9p mount; wait for it in case the mount
# races cloud-init. Each transport leg has its own renderd instance and
# output directory, so identical frame ids don't collide.
AUTORUN="for i in \$(seq 60); do [ -x /mnt/repo/build/demo-static ] && break; sleep 2; done; /mnt/repo/build/demo-static --tcp 10.0.2.2 $TCP_PORT; /mnt/repo/build/demo-static --shm-pci"
if [ "$HAVE_VSOCK" = 1 ]; then
    AUTORUN="$AUTORUN; /mnt/repo/build/demo-static --vsock 2 $VSOCK_PORT"
fi
AUTORUN="$AUTORUN; poweroff"

AUTORUN_CMD="$AUTORUN" OUT="$TMP/guest" "$ROOT/scripts/make-guest-image.sh"

"$BUILD/renderd" --tcp "$TCP_PORT" --out "$TMP/frames-tcp" --once &
TCP_PID=$!
"$BUILD/renderd" --shm "$TMP/ivshmem.bin" --out "$TMP/frames-shm" --once &
SHM_PID=$!
VSOCK_PID=""
if [ "$HAVE_VSOCK" = 1 ]; then
    "$BUILD/renderd" --vsock "$VSOCK_PORT" --out "$TMP/frames-vsock" --once &
    VSOCK_PID=$!
fi
cleanup() {
    kill "$TCP_PID" "$SHM_PID" $VSOCK_PID 2>/dev/null || true
    wait "$TCP_PID" "$SHM_PID" $VSOCK_PID 2>/dev/null || true
}
trap cleanup EXIT

# renderd --shm must have created and initialized the region before QEMU
# maps it.
for _ in $(seq 1 100); do
    [ -f "$TMP/ivshmem.bin" ] && break
    sleep 0.05
done
[ -f "$TMP/ivshmem.bin" ] || { echo "vm-e2e: shm region not created" >&2; exit 1; }

echo "vm-e2e: booting guest (log: $TMP/console.log)"
rc=0
IMG="$TMP/guest/disk.qcow2" SEED="$TMP/guest/seed.iso" ACCEL="$ACCEL" \
    SHARE="$ROOT" IVSHMEM="$TMP/ivshmem.bin" \
    timeout "$BOOT_TIMEOUT" "$ROOT/scripts/run-qemu.sh" \
    > "$TMP/console.log" 2>&1 || rc=$?
if [ "$rc" != 0 ]; then
    echo "vm-e2e: qemu exited with $rc; last console output:" >&2
    tail -30 "$TMP/console.log" >&2
    exit 1
fi

check_frame() {
    local ppm="$1"

    if [ ! -f "$ppm" ]; then
        echo "vm-e2e: missing $ppm; last console output:" >&2
        tail -30 "$TMP/console.log" >&2
        exit 1
    fi
    "$BUILD/ppm_check" "$ppm" 5 5 102030
    "$BUILD/ppm_check" "$ppm" 60 60 ff0000
    "$BUILD/ppm_check" "$ppm" 200 150 00ff00
}

check_frame "$TMP/frames-tcp/frame-000001.ppm"
echo "vm-e2e: tcp transport OK"
check_frame "$TMP/frames-shm/frame-000001.ppm"
echo "vm-e2e: shm transport OK"
if [ "$HAVE_VSOCK" = 1 ]; then
    check_frame "$TMP/frames-vsock/frame-000001.ppm"
    echo "vm-e2e: vsock transport OK"
else
    echo "vm-e2e: vsock transport SKIPPED (/dev/vhost-vsock not available)"
fi
echo "vm-e2e: OK"
