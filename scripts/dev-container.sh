#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Run a command inside the development container defined by
# containers/dev.Dockerfile, with this repository mounted at /src.
#
# The point is that nothing lands on your machine: QEMU, the guest kernel
# headers and the ISO tool all live in the image, and the only thing
# shared is the repository directory. It carries the same packages CI
# installs on its runner, so what passes here passes there.
#
#   scripts/dev-container.sh                      interactive shell
#   scripts/dev-container.sh make test            build and run the tests
#   scripts/dev-container.sh tests/vm-e2e.sh      boot the guest, all phases
#
# The container runs as your uid/gid, so files it writes into the repo
# (build/, .tmp/, the .ko files) stay yours rather than root's.
#
# /dev/kvm is passed through when present — without it QEMU falls back to
# TCG, which works but is several times slower. /dev/vhost-vsock likewise:
# without it the vsock leg of the tests is skipped and the rest still runs.
#
# Environment overrides:
#   KVER   guest kernel version to install in the image (default: the
#          pinned one, must match scripts/build-kmod.sh)
#   IMAGE  tag to build and run (default qemu-custom-dev:$KVER)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KVER="${KVER:-6.8.0-134-generic}"
IMAGE="${IMAGE:-qemu-custom-dev:$KVER}"

command -v docker >/dev/null 2>&1 || {
    echo "dev-container: docker not found." >&2
    echo "  Without it, install the packages listed in" \
         "containers/dev.Dockerfile and run the commands directly." >&2
    exit 1
}
docker info >/dev/null 2>&1 || {
    echo "dev-container: docker is installed but not usable (is the" \
         "daemon running, and are you in the docker group?)" >&2
    exit 1
}

echo "dev-container: building $IMAGE (first run pulls and installs; later" \
     "runs are cached)" >&2
docker build -q \
    --build-arg "KVER=$KVER" \
    -t "$IMAGE" \
    -f "$ROOT/containers/dev.Dockerfile" \
    "$ROOT/containers" >/dev/null

run=(
    docker run --rm
    -v "$ROOT:/src" -w /src
    -u "$(id -u):$(id -g)"
    -e HOME=/tmp
)

# Interactive only when there is a terminal to be interactive with, so
# the same script works from a script or from CI.
[ -t 0 ] && [ -t 1 ] && run+=( -it )

for dev in /dev/kvm /dev/vhost-vsock; do
    if [ -e "$dev" ]; then
        run+=( --device "$dev" )
        # Passing the device is not enough: the unprivileged user inside
        # needs to be in the group that owns it.
        run+=( --group-add "$(stat -c %g "$dev")" )
    else
        echo "dev-container: $dev not present, continuing without it" >&2
    fi
done

exec "${run[@]}" "$IMAGE" "${@:-/bin/bash}"
