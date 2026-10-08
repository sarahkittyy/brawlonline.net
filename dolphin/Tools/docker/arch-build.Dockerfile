# Arch Linux build + test image (the closest container to SteamOS 3, which is Arch-based).
#
# Same layout and scripts as linux-build.Dockerfile; only the package list differs. Arch ships
# newer toolchains than Ubuntu (GCC 15, CMake 4, Qt 6.9+), so this also catches breakage that a
# rolling-release distro or a future SteamOS update would hit.
#
#   docker build -t pplus-dolphin-linux:arch -f Tools/docker/arch-build.Dockerfile Tools/docker

FROM archlinux:base

RUN pacman -Syu --noconfirm --needed \
      base-devel git cmake ninja pkgconf \
      qt6-base qt6-svg \
      ffmpeg libxi libxrandr libevdev sfml miniupnpc curl hidapi systemd-libs bluez-libs \
      alsa-lib libpulse pugixml bzip2 zstd lzo libpng libusb mesa \
      python python-pytest procps-ng \
 && pacman -Scc --noconfirm \
 && rm -rf /var/cache/pacman/pkg/* /var/lib/pacman/sync/*

COPY build.sh test.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/build.sh /usr/local/bin/test.sh \
 && ln -s /usr/local/bin/build.sh /usr/local/bin/build \
 && ln -s /usr/local/bin/test.sh /usr/local/bin/test

WORKDIR /build
