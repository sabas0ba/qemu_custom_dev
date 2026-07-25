#!/usr/bin/env bash
# Launch a Linux guest with a vhost-vsock device (Phase 1 transport).
#
# Prerequisites on the host:
#   - /dev/vhost-vsock available (modprobe vhost_vsock)
#   - KVM available (falls back to TCG if not, see ACCEL below)
#
# Usage:
#   IMG=/path/to/guest.qcow2 scripts/run-qemu.sh [extra qemu args...]
#
# Environment overrides:
#   IMG        guest disk image (required)
#   GUEST_CID  vsock CID assigned to the guest (default 3; host is always 2)
#   MEM        guest RAM (default 2G)
#   CPUS       vCPU count (default 2)
#   ACCEL      accelerator (default kvm; set ACCEL=tcg without /dev/kvm)
#
# Inside the guest, connect to the host daemon with:
#   demo --vsock 2 <port>        # CID 2 = VMADDR_CID_HOST
# after starting on the host:
#   build/renderd --vsock <port> --out .tmp/frames
set -euo pipefail

IMG="${IMG:?set IMG to the guest disk image path}"
GUEST_CID="${GUEST_CID:-3}"
MEM="${MEM:-2G}"
CPUS="${CPUS:-2}"
ACCEL="${ACCEL:-kvm}"

exec qemu-system-x86_64 \
    -machine q35,accel="$ACCEL" \
    -cpu max \
    -m "$MEM" \
    -smp "$CPUS" \
    -drive file="$IMG",if=virtio \
    -device vhost-vsock-pci,guest-cid="$GUEST_CID" \
    -nographic \
    "$@"
