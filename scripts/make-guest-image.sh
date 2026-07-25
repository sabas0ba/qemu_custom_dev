#!/usr/bin/env bash
# Build the Phase 1 guest disk image and cloud-init seed ISO.
#
# Downloads a pinned Ubuntu cloud image (verified against a pinned SHA256),
# creates a qcow2 overlay so the base image stays pristine, and generates a
# NoCloud cloud-init seed that sets up a login user and mounts the repo
# shared from the host via 9p (see scripts/run-qemu.sh).
#
# Host dependencies: curl, sha256sum, qemu-img, and one of
# cloud-localds / genisoimage / xorriso / mkisofs for the seed ISO.
#
# Usage:
#   scripts/make-guest-image.sh
#
# Outputs (under .tmp/guest/ by default):
#   disk.qcow2   overlay disk to pass as IMG= to run-qemu.sh
#   seed.iso     cloud-init seed to pass as SEED= to run-qemu.sh
#
# Environment overrides:
#   OUT         output directory        (default .tmp/guest)
#   CACHE       base image cache dir    (default .tmp/cache)
#   DISK_SIZE   overlay disk size       (default 10G)
#   GUEST_USER  login user name         (default dev)
#   GUEST_PASS  login password          (default dev; local experiments only)
#   SSH_PUBKEY  optional SSH public key string for GUEST_USER
#   AUTORUN_CMD optional shell command run (as root) by cloud-init at the
#               end of first boot. Used by tests/vm-e2e.sh for unattended
#               runs. When set, package installation is skipped so the
#               boot needs no external network access.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$ROOT/.tmp/guest}"
CACHE="${CACHE:-$ROOT/.tmp/cache}"
DISK_SIZE="${DISK_SIZE:-10G}"
GUEST_USER="${GUEST_USER:-dev}"
GUEST_PASS="${GUEST_PASS:-dev}"
SSH_PUBKEY="${SSH_PUBKEY:-}"
AUTORUN_CMD="${AUTORUN_CMD:-}"

# Pinned base image. Update deliberately: pick a snapshot from
# https://cloud-images.ubuntu.com/releases/noble/ and take the matching
# hash from its SHA256SUMS file (see docs/guest-image.md).
RELEASE="noble"
SNAPSHOT="release-20260705"
BASE_IMG="ubuntu-24.04-server-cloudimg-amd64.img"
BASE_URL="https://cloud-images.ubuntu.com/releases/${RELEASE}/${SNAPSHOT}/${BASE_IMG}"
BASE_SHA256="ffe6203da54deeb6db5d2a98a83f9ec8e55f149d3f7ba622e1abe5fa966ee3d6"

die() { echo "make-guest-image: $*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || die "missing dependency: $1"; }
need curl
need sha256sum
need qemu-img

verify() {
    echo "${BASE_SHA256}  $1" | sha256sum --check --quiet -
}

fetch_base() {
    local base="$CACHE/$BASE_IMG"

    mkdir -p "$CACHE"
    if [ -f "$base" ] && verify "$base"; then
        echo "make-guest-image: base image already cached and verified"
        return
    fi
    echo "make-guest-image: downloading $BASE_URL"
    curl -fSL --proto '=https' -o "$base.part" "$BASE_URL"
    verify "$base.part" || die "SHA256 mismatch for downloaded base image"
    mv "$base.part" "$base"
}

make_overlay() {
    qemu-img create -f qcow2 -b "$CACHE/$BASE_IMG" -F qcow2 \
        "$OUT/disk.qcow2" "$DISK_SIZE"
}

write_cloud_init() {
    cat > "$OUT/meta-data" <<EOF
instance-id: renderguest-001
local-hostname: renderguest
EOF

    cat > "$OUT/user-data" <<EOF
#cloud-config
hostname: renderguest
users:
  - name: ${GUEST_USER}
    groups: sudo
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    lock_passwd: false
    plain_text_passwd: ${GUEST_PASS}
EOF
    if [ -n "$SSH_PUBKEY" ]; then
        cat >> "$OUT/user-data" <<EOF
    ssh_authorized_keys:
      - ${SSH_PUBKEY}
EOF
    fi
    cat >> "$OUT/user-data" <<EOF
ssh_pwauth: true
chpasswd:
  expire: false
EOF
    if [ -z "$AUTORUN_CMD" ]; then
        cat >> "$OUT/user-data" <<EOF
packages:
  - build-essential
EOF
    fi
    cat >> "$OUT/user-data" <<EOF
# Mount the repo shared by run-qemu.sh (-virtfs ... mount_tag=repo).
# nofail keeps boot working when the guest is started without the share.
mounts:
  - [repo, /mnt/repo, 9p, "trans=virtio,version=9p2000.L,ro,nofail", "0", "0"]
EOF
    if [ -n "$AUTORUN_CMD" ]; then
        cat >> "$OUT/user-data" <<EOF
runcmd:
  - [sh, -c, '${AUTORUN_CMD}']
EOF
    fi
}

make_seed() {
    if command -v cloud-localds >/dev/null 2>&1; then
        cloud-localds "$OUT/seed.iso" "$OUT/user-data" "$OUT/meta-data"
        return
    fi
    local isotool
    for isotool in genisoimage xorriso mkisofs; do
        command -v "$isotool" >/dev/null 2>&1 && break
        isotool=""
    done
    [ -n "$isotool" ] || die "need cloud-localds or genisoimage/xorriso/mkisofs"
    if [ "$isotool" = xorriso ]; then
        xorriso -as mkisofs -output "$OUT/seed.iso" -volid cidata \
            -joliet -rock "$OUT/user-data" "$OUT/meta-data"
    else
        "$isotool" -output "$OUT/seed.iso" -volid cidata \
            -joliet -rock "$OUT/user-data" "$OUT/meta-data"
    fi
}

mkdir -p "$OUT"
fetch_base
make_overlay
write_cloud_init
make_seed

echo
echo "make-guest-image: done"
echo "  disk: $OUT/disk.qcow2"
echo "  seed: $OUT/seed.iso"
echo "boot with:"
echo "  IMG=$OUT/disk.qcow2 SEED=$OUT/seed.iso scripts/run-qemu.sh"
