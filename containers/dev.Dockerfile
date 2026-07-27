# SPDX-License-Identifier: GPL-2.0-only
# Everything needed to build this project AND boot the guest, so nothing
# has to be installed on the machine you are sitting at.
#
# The package list is deliberately the same one .github/workflows/ci.yml
# installs on its runner: if CI can run tests/vm-e2e.sh with these, so can
# you. Keep the two in sync when either changes.
#
# Built and run by scripts/dev-container.sh; see the README for usage.

# ubuntu:24.04 pinned by digest so the toolchain cannot drift underneath us
FROM ubuntu@sha256:4fbb8e6a8395de5a7550b33509421a2bafbc0aab6c06ba2cef9ebffbc7092d90

# Kernel of the pinned guest image. Must match scripts/build-kmod.sh and
# containers/kmod-build.Dockerfile.
ARG KVER=6.8.0-134-generic

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential \
      bc \
      kmod \
      "linux-headers-${KVER}" \
      qemu-system-x86 \
      qemu-utils \
      genisoimage \
      curl \
      ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# There is no docker inside the container, so the module has to be built
# against the headers installed above rather than in a nested container.
ENV KMOD_MODE=direct

WORKDIR /src
CMD ["/bin/bash"]
