#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Full-stack test: boot the real guest image under QEMU and drive the
# renderer from INSIDE the guest against daemons on the host.
#
# Two boots, because both ivshmem flavours answer to the same PCI ID and a
# guest holding one of each could not tell them apart:
#
#   boot 1  tcp (always), ivshmem-plain polled (always), vsock (when
#           /dev/vhost-vsock exists), plus a latency measurement over the
#           polled ring
#   boot 2  ivshmem-doorbell through the ivshmem_rproto kernel module,
#           plus the same measurement over interrupts — the Phase 2 vs
#           Phase 3 comparison the project set out to make
#
# The guest runs unattended: cloud-init's runcmd executes statically
# linked binaries from the 9p-shared repo and powers the guest off. No
# network access or toolchain is needed inside the guest.
#
# Requires: qemu-system-x86_64, qemu-img, an ISO tool (see
# make-guest-image.sh), kernel headers or docker for the module (see
# scripts/build-kmod.sh), and the ~600 MB base image download (cached
# under .tmp/cache). Takes ~2 min with KVM, considerably longer under TCG.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
TMP="$ROOT/.tmp/vm-e2e"
TCP_PORT=15502
VSOCK_PORT=15503
BOOT_TIMEOUT="${BOOT_TIMEOUT:-1800}"
BENCH_ITERS="${BENCH_ITERS:-300}"

HAVE_VSOCK=0
[ -e /dev/vhost-vsock ] && [ -w /dev/vhost-vsock ] && HAVE_VSOCK=1
ACCEL=tcg
[ -e /dev/kvm ] && [ -w /dev/kvm ] && ACCEL=kvm
echo "vm-e2e: accel=$ACCEL vsock=$HAVE_VSOCK"

rm -rf "$TMP"
mkdir -p "$TMP/frames-tcp" "$TMP/frames-vsock" "$TMP/frames-shm" \
         "$TMP/frames-doorbell"

make -C "$ROOT" all build/demo-static build/bench-static >/dev/null
"$ROOT/scripts/build-kmod.sh"

# Wait for the 9p mount to appear before touching anything on it: cloud-init
# may reach runcmd before the mount unit has settled.
WAIT_MOUNT='for i in $(seq 60); do [ -x /mnt/repo/build/demo-static ] && break; sleep 2; done'

check_frame() {
    local ppm="$1" log="$2"

    if [ ! -f "$ppm" ]; then
        echo "vm-e2e: missing $ppm; last console output:" >&2
        tail -40 "$log" >&2
        exit 1
    fi
    "$BUILD/ppm_check" "$ppm" 5 5 102030
    "$BUILD/ppm_check" "$ppm" 60 60 ff0000
    "$BUILD/ppm_check" "$ppm" 200 150 00ff00
}

# boot_guest <name> <autorun command>; extra QEMU settings come from the
# environment of the caller (IVSHMEM / IVSHMEM_SOCKET).
boot_guest() {
    local name="$1" autorun="$2"
    local out="$TMP/guest-$name" log="$TMP/console-$name.log"
    local rc=0

    AUTORUN_CMD="$autorun" OUT="$out" "$ROOT/scripts/make-guest-image.sh" \
        >"$TMP/image-$name.log" 2>&1
    echo "vm-e2e: booting guest ($name; log: $log)"
    IMG="$out/disk.qcow2" SEED="$out/seed.iso" ACCEL="$ACCEL" SHARE="$ROOT" \
        timeout "$BOOT_TIMEOUT" "$ROOT/scripts/run-qemu.sh" \
        >"$log" 2>&1 || rc=$?
    if [ "$rc" != 0 ]; then
        echo "vm-e2e: qemu exited with $rc; last console output:" >&2
        tail -40 "$log" >&2
        exit 1
    fi
}

# Pull one "key value" line printed by bench out of a console log.
bench_value() {
    tr -d '\r' < "$1" | awk -v k="$2" '$1 == k { print $2; exit }'
}

# ---------------------------------------------------------------- boot 1
"$BUILD/renderd" --tcp "$TCP_PORT" --out "$TMP/frames-tcp" --once &
TCP_PID=$!
"$BUILD/renderd" --shm "$TMP/ivshmem.bin" --out "$TMP/frames-shm" --once &
SHM_PID=$!
VSOCK_PID=""
if [ "$HAVE_VSOCK" = 1 ]; then
    "$BUILD/renderd" --vsock "$VSOCK_PORT" --out "$TMP/frames-vsock" --once &
    VSOCK_PID=$!
fi
cleanup1() {
    kill "$TCP_PID" "$SHM_PID" $VSOCK_PID 2>/dev/null || true
    wait "$TCP_PID" "$SHM_PID" $VSOCK_PID 2>/dev/null || true
}
trap cleanup1 EXIT

# renderd --shm creates and initializes the region; QEMU maps it as BAR2.
for _ in $(seq 1 100); do
    [ -f "$TMP/ivshmem.bin" ] && break
    sleep 0.05
done
[ -f "$TMP/ivshmem.bin" ] || { echo "vm-e2e: shm region not created" >&2; exit 1; }

AUTORUN="$WAIT_MOUNT; /mnt/repo/build/demo-static --tcp 10.0.2.2 $TCP_PORT"
if [ "$HAVE_VSOCK" = 1 ]; then
    AUTORUN="$AUTORUN; /mnt/repo/build/demo-static --vsock 2 $VSOCK_PORT"
fi
# One session against the shared region: bench draws and presents the
# scene itself, so the frame check and the timings come from the same
# connection and no second session has to reuse the region.
AUTORUN="$AUTORUN; /mnt/repo/build/bench-static --shm-pci --iters $BENCH_ITERS --scene"
AUTORUN="$AUTORUN; poweroff"

IVSHMEM="$TMP/ivshmem.bin" boot_guest plain "$AUTORUN"
cleanup1
trap - EXIT

LOG1="$TMP/console-plain.log"
check_frame "$TMP/frames-tcp/frame-000001.ppm" "$LOG1"
echo "vm-e2e: tcp transport OK"
check_frame "$TMP/frames-shm/frame-000001.ppm" "$LOG1"
echo "vm-e2e: shm (polled) transport OK"
if [ "$HAVE_VSOCK" = 1 ]; then
    check_frame "$TMP/frames-vsock/frame-000001.ppm" "$LOG1"
    echo "vm-e2e: vsock transport OK"
else
    echo "vm-e2e: vsock transport SKIPPED (/dev/vhost-vsock not available)"
fi

# ---------------------------------------------------------------- boot 2
IVSOCK="$TMP/ivshmem.sock"
"$BUILD/ivshmemd" --socket "$IVSOCK" --shm "$TMP/doorbell.bin" \
    --size 4194304 --vectors 1 >"$TMP/ivshmemd.log" 2>&1 &
IVD_PID=$!
cleanup2() {
    kill "$IVD_PID" ${DB_PID:-} 2>/dev/null || true
    wait "$IVD_PID" ${DB_PID:-} 2>/dev/null || true
}
trap cleanup2 EXIT

for _ in $(seq 1 100); do
    [ -S "$IVSOCK" ] && break
    sleep 0.05
done
[ -S "$IVSOCK" ] || { echo "vm-e2e: ivshmemd did not start" >&2; exit 1; }

"$BUILD/renderd" --ivshmem "$IVSOCK" --out "$TMP/frames-doorbell" --once &
DB_PID=$!

# The guest loads the module, draws the scene, then measures the same
# round trip the polled run measured in boot 1.
AUTORUN="$WAIT_MOUNT; insmod /mnt/repo/guest/kmod/ivshmem_rproto.ko"
AUTORUN="$AUTORUN; dmesg | grep -i ivshmem_rproto"
AUTORUN="$AUTORUN; /mnt/repo/build/bench-static --doorbell --iters $BENCH_ITERS --scene"
AUTORUN="$AUTORUN; poweroff"

IVSHMEM_SOCKET="$IVSOCK" boot_guest doorbell "$AUTORUN"
cleanup2
trap - EXIT

LOG2="$TMP/console-doorbell.log"
if ! grep -q "doorbell transport ready" "$LOG2"; then
    echo "vm-e2e: the guest never reached the doorbell transport;" \
         "last console output:" >&2
    tail -40 "$LOG2" >&2
    exit 1
fi
check_frame "$TMP/frames-doorbell/frame-000001.ppm" "$LOG2"
echo "vm-e2e: doorbell transport OK"

POLL_MEAN="$(bench_value "$LOG1" mean_us || true)"
POLL_P50="$(bench_value "$LOG1" p50_us || true)"
DB_MEAN="$(bench_value "$LOG2" mean_us || true)"
DB_P50="$(bench_value "$LOG2" p50_us || true)"
echo "vm-e2e: round-trip latency over $BENCH_ITERS iterations (accel=$ACCEL)"
printf 'vm-e2e:   %-22s mean %8s us   p50 %8s us\n' \
    "shm polled (Phase 2)" "${POLL_MEAN:-n/a}" "${POLL_P50:-n/a}"
printf 'vm-e2e:   %-22s mean %8s us   p50 %8s us\n' \
    "doorbell (Phase 3)" "${DB_MEAN:-n/a}" "${DB_P50:-n/a}"
[ -n "$POLL_MEAN" ] && [ -n "$DB_MEAN" ] || \
    echo "vm-e2e: WARNING benchmark numbers incomplete" >&2

echo "vm-e2e: OK"
