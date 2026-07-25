# Build environment for the guest kernel module.
#
# Pinned to the distribution and kernel of the guest image produced by
# scripts/make-guest-image.sh, so the resulting module's vermagic matches
# the kernel it gets loaded into. Update KVER here and in
# scripts/build-kmod.sh together with the image snapshot.
#
# Built and run by scripts/build-kmod.sh; not meant to be used directly.

# ubuntu:24.04 pinned by digest so the toolchain cannot drift underneath us
FROM ubuntu@sha256:4fbb8e6a8395de5a7550b33509421a2bafbc0aab6c06ba2cef9ebffbc7092d90

ARG KVER=6.8.0-134-generic

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential \
      bc \
      kmod \
      "linux-headers-${KVER}" \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
