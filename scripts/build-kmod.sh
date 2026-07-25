#!/usr/bin/env bash
# Build the guest kernel module against the *guest* kernel's headers.
#
# The module has to match the kernel it will be loaded into (6.8.0-134 in
# the pinned guest image), which is almost never the kernel of the machine
# running this script. Two ways to get those headers:
#
#   container mode (default when docker is available)
#       Builds inside the image defined by containers/kmod-build.Dockerfile,
#       so the developer's own machine stays untouched — the project rule
#       for toolchains that would otherwise have to be installed system
#       wide.
#
#   direct mode (--direct, or the automatic fallback)
#       Uses headers already installed under /lib/modules. Meant for
#       throwaway environments — CI runners and dev containers — where
#       there is no host to keep clean and pulling an image is just
#       overhead.
#
# Either way the output is guest/kmod/ivshmem_rproto.ko, built with the
# same Kbuild makefile.
#
# Usage:
#   scripts/build-kmod.sh [--direct|--container]
#
# Environment overrides:
#   KVER       guest kernel version (default: pinned below, must match the
#              image built by scripts/make-guest-image.sh)
#   KMOD_MODE  auto (default), direct or container; the flag wins over it.
#              Lets callers that already run somewhere disposable — CI, and
#              tests/vm-e2e.sh through it — skip the container.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Kernel of the pinned guest image (see docs/guest-image.md). Update
# together with the image snapshot.
KVER="${KVER:-6.8.0-134-generic}"

MODE="${KMOD_MODE:-auto}"
case "${1:-}" in
    --direct)    MODE=direct ;;
    --container) MODE=container ;;
    "")          ;;
    *)           echo "usage: $0 [--direct|--container]" >&2; exit 2 ;;
esac

die() { echo "build-kmod: $*" >&2; exit 1; }

have_docker() { command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; }
have_headers() { [ -d "/lib/modules/$KVER/build" ]; }

if [ "$MODE" = auto ]; then
    if have_docker; then
        MODE=container
    elif have_headers; then
        MODE=direct
    else
        die "need either a working docker or linux-headers-$KVER installed
  container: install/start docker, then rerun
  direct:    sudo apt-get install linux-headers-$KVER && $0 --direct"
    fi
fi

if [ "$MODE" = container ]; then
    have_docker || die "docker is not usable; try $0 --direct"
    echo "build-kmod: building in a container for $KVER"
    docker build -q \
        --build-arg "KVER=$KVER" \
        -t "qemu-custom-dev-kmod:$KVER" \
        -f "$ROOT/containers/kmod-build.Dockerfile" \
        "$ROOT/containers" >/dev/null
    docker run --rm \
        -v "$ROOT:/src" -w /src \
        -u "$(id -u):$(id -g)" \
        "qemu-custom-dev-kmod:$KVER" \
        make -C guest/kmod "KDIR=/lib/modules/$KVER/build"
else
    have_headers || die "no headers at /lib/modules/$KVER/build
  install them with: sudo apt-get install linux-headers-$KVER"
    echo "build-kmod: building directly against /lib/modules/$KVER/build"
    make -C "$ROOT/guest/kmod" "KDIR=/lib/modules/$KVER/build"
fi

KO="$ROOT/guest/kmod/ivshmem_rproto.ko"
[ -f "$KO" ] || die "build produced no module"
echo "build-kmod: $KO"
