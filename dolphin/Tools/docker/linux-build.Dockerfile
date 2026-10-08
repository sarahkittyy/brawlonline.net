# Ubuntu 24.04 build + test image for the P+ rollback Dolphin fork.
#
# The image holds only the toolchain, Dolphin's documented Linux build dependencies
# (https://github.com/dolphin-emu/dolphin/wiki/Building-for-Linux) and Python 3.12 for the
# ppharness test suite. Sources, the build tree and the game data are bind-mounted at run time,
# so the image stays small and nothing large is baked into it.
#
#   docker build -t pplus-dolphin-linux:ubuntu24.04 -f Tools/docker/linux-build.Dockerfile Tools/docker
#   docker run --rm -v <dolphin src>:/src:ro -v <build dir>:/build pplus-dolphin-linux:ubuntu24.04 build
#
# See Tools/docker/README.md (or docs/linux-build.md in the workspace) for the full workflow.

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates git cmake ninja-build make gcc g++ pkg-config gettext \
      qt6-base-dev qt6-base-private-dev qt6-svg-dev \
      libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
      libxi-dev libxrandr-dev libudev-dev libevdev-dev libsfml-dev libminiupnpc-dev \
      libmbedtls-dev libcurl4-openssl-dev libhidapi-dev libsystemd-dev libbluetooth-dev \
      libasound2-dev libpulse-dev libpugixml-dev libbz2-dev libzstd-dev liblzo2-dev \
      libpng-dev libusb-1.0-0-dev libgl-dev libegl-dev \
      python3 python3-pytest procps \
 && rm -rf /var/lib/apt/lists/*

COPY build.sh test.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/build.sh /usr/local/bin/test.sh \
 && ln -s /usr/local/bin/build.sh /usr/local/bin/build \
 && ln -s /usr/local/bin/test.sh /usr/local/bin/test

WORKDIR /build
