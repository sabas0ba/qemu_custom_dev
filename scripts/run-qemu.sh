#!/usr/bin/env bash
# Launch a Linux guest with a vhost-vsock device (Phase 1 transport).
#
# Prerequisites on the host:
#   - /dev/vhost-vsock available (modprobe vhost_vsock)
#   - KVM available (set ACCEL=tcg without /dev/kvm)
#
# Usage:
#   IMG=/path/to/disk.qcow2 scripts/run-qemu.sh [extra qemu args...]
#   IMG=... SEED=.tmp/guest/seed.iso scripts/run-qemu.sh   # first boot
#
# Environment overrides:
#   IMG        guest disk image (required; see scripts/make-guest-image.sh)
#   SEED       cloud-init seed ISO, attached when set
#   GUEST_CID  vsock CID assigned to the guest (default 3; host is always 2)
#   MEM        guest RAM (default 2G)
#   CPUS       vCPU count (default 2)
#   ACCEL      accelerator (default kvm; set ACCEL=tcg without /dev/kvm)
#   SHARE      directory exported to the guest over 9p as tag "repo"
#              (default: this repository; SHARE=none disables)
#
# Inside the guest, the share appears at /mnt/repo (read-only; mounted by
# cloud-init). Build in a writable copy and connect to the host daemon:
#   cp -r /mnt/repo ~/work && cd ~/work && make
#   ./build/demo --vsock 2 <port>     # CID 2 = VMADDR_CID_HOST
# after starting on the host:
#   build/renderd --vsock <port> --out .tmp/frames
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

IMG="${IMG:?set IMG to the guest disk image path}"
SEED="${SEED:-}"
GUEST_CID="${GUEST_CID:-3}"
MEM="${MEM:-2G}"
CPUS="${CPUS:-2}"
ACCEL="${ACCEL:-kvm}"
SHARE="${SHARE:-$ROOT}"

args=(
    -machine q35,accel="$ACCEL"
    -cpu max
    -m "$MEM"
    -smp "$CPUS"
    -drive file="$IMG",if=virtio
    -device vhost-vsock-pci,guest-cid="$GUEST_CID"
    -nic user,model=virtio-net-pci
    -nographic
)
if [ -n "$SEED" ]; then
    args+=( -drive file="$SEED",format=raw,if=virtio,readonly=on )
fi
if [ "$SHARE" != none ]; then
    args+=( -virtfs local,path="$SHARE",mount_tag=repo,security_model=none,readonly=on )
fi

exec qemu-system-x86_64 "${args[@]}" "$@"
