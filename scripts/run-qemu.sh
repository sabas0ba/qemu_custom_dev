#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
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
#   GUEST_CID  vsock CID assigned to the guest (default 3; host is always 2;
#              GUEST_CID=none disables the vsock device. It is also skipped
#              with a warning when /dev/vhost-vsock is unavailable, so the
#              guest can still boot on hosts without vhost-vsock, e.g.
#              containers and CI: use the tcp transport there)
#   MEM        guest RAM (default 2G)
#   CPUS       vCPU count (default 2)
#   ACCEL      accelerator (default kvm; set ACCEL=tcg without /dev/kvm)
#   SHARE      directory exported to the guest over 9p as tag "repo"
#              (default: this repository; SHARE=none disables)
#   IVSHMEM    path to a shared-memory file to expose as an ivshmem-plain
#              PCI device (Phase 2). Create and initialize it first with
#              `renderd --shm $IVSHMEM` so host and guest share the region.
#   IVSHMEM_SIZE  region size, must match renderd's and be a power of two
#              (default 4M = RSHM_DEFAULT_SIZE)
#   IVSHMEM_SOCKET  path to an ivshmem server socket to attach as an
#              ivshmem-doorbell PCI device (Phase 3). Start host/ivshmemd
#              on that socket first; the region and the interrupt eventfds
#              both come from the server. Do not combine with IVSHMEM:
#              two ivshmem devices share one PCI ID, which leaves the
#              guest unable to tell them apart.
#   IVSHMEM_VECTORS  MSI-X vectors on that device, must match the server's
#              --vectors (default 1)
#   VHOST_USER  path to a vhost-user backend socket (Phase 4). Start
#              `renderd --vhost-user $VHOST_USER` first; QEMU connects to
#              it and exposes a generic virtio device to the guest. Guest
#              RAM is switched to a shared memfd backing, without which
#              the backend could not map it.
#   VHOST_USER_VIRTIO_ID  virtio device ID the guest sees (default 37).
#              QEMU only accepts IDs it has a name for, so this is not a
#              free choice; guest/kmod/virtio_rproto binds the same
#              number. See docs/vhost-user.md.
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
IVSHMEM="${IVSHMEM:-}"
IVSHMEM_SIZE="${IVSHMEM_SIZE:-4M}"
IVSHMEM_SOCKET="${IVSHMEM_SOCKET:-}"
IVSHMEM_VECTORS="${IVSHMEM_VECTORS:-1}"
VHOST_USER="${VHOST_USER:-}"
VHOST_USER_VIRTIO_ID="${VHOST_USER_VIRTIO_ID:-37}"

args=(
    -machine q35,accel="$ACCEL"
    -cpu max
    -m "$MEM"
    -smp "$CPUS"
    -drive file="$IMG",if=virtio
    -nic user,model=virtio-net-pci
    -nographic
)
if [ -n "$VHOST_USER" ]; then
    [ -S "$VHOST_USER" ] || { echo "run-qemu: no vhost-user socket at" \
        "$VHOST_USER (start renderd --vhost-user first)" >&2; exit 1; }
    # A vhost-user backend maps guest RAM by fd, so the machine's memory
    # has to be a shareable object rather than anonymous memory.
    args+=(
        -object memory-backend-memfd,id=vumem,size="$MEM",share=on
        -machine memory-backend=vumem
        # No reconnect= here: it makes the chardev connect asynchronously,
        # so the socket is not up yet when the device is realised and
        # QEMU fails the first fd-carrying request with
        # "Failed to set msg fds."
        -chardev socket,path="$VHOST_USER",id=vurender
        -device vhost-user-device-pci,chardev=vurender,virtio-id="$VHOST_USER_VIRTIO_ID",num_vqs=1
    )
fi
if [ "$GUEST_CID" = none ]; then
    :
elif [ -e /dev/vhost-vsock ]; then
    args+=( -device vhost-vsock-pci,guest-cid="$GUEST_CID" )
else
    echo "run-qemu: /dev/vhost-vsock not available, starting without vsock" \
         "(try: sudo modprobe vhost_vsock)" >&2
fi
if [ -n "$SEED" ]; then
    args+=( -drive file="$SEED",format=raw,if=virtio,readonly=on )
fi
if [ "$SHARE" != none ]; then
    args+=( -virtfs local,path="$SHARE",mount_tag=repo,security_model=none,readonly=on )
fi
if [ -n "$IVSHMEM" ] && [ -n "$IVSHMEM_SOCKET" ]; then
    echo "run-qemu: set only one of IVSHMEM and IVSHMEM_SOCKET" >&2
    exit 1
fi
if [ -n "$IVSHMEM" ]; then
    [ -f "$IVSHMEM" ] || { echo "run-qemu: IVSHMEM file $IVSHMEM missing" \
        "(start renderd --shm first)" >&2; exit 1; }
    args+=(
        -object memory-backend-file,id=ivshm,share=on,mem-path="$IVSHMEM",size="$IVSHMEM_SIZE"
        -device ivshmem-plain,memdev=ivshm
    )
fi
if [ -n "$IVSHMEM_SOCKET" ]; then
    [ -S "$IVSHMEM_SOCKET" ] || { echo "run-qemu: no ivshmem server socket at" \
        "$IVSHMEM_SOCKET (start ivshmemd first)" >&2; exit 1; }
    args+=(
        -chardev socket,path="$IVSHMEM_SOCKET",id=ivshmem_db
        -device ivshmem-doorbell,chardev=ivshmem_db,vectors="$IVSHMEM_VECTORS"
    )
fi

exec qemu-system-x86_64 "${args[@]}" "$@"
