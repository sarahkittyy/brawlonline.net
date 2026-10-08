#!/usr/bin/env bash
# Configure and build Dolphin (Release, Ninja) from /src into /build.
#
#   build.sh [extra cmake args...]
#
# Environment:
#   SRC        source tree (default /src, may be mounted read-only)
#   BUILD      build tree (default /build)
#   TARGETS    ninja targets (default: dolphin-nogui project-plus-dolphin)
#   JOBS       parallel jobs (default: nproc)
#   OUT        if set, Binaries/ is copied there after the build
#
# The version string comes from git (CMake/ScmRevGen.cmake). A git worktree mounted from another
# OS has a .git file that points at a host path; mount the main repo's .git and set GIT_DIR and
# GIT_WORK_TREE so git still works (see docs). Without git the version is "<major>.<minor>".
set -euo pipefail

SRC=${SRC:-/src}
BUILD=${BUILD:-/build}
TARGETS=${TARGETS:-"dolphin-nogui project-plus-dolphin"}
JOBS=${JOBS:-$(nproc)}

extra=()
# CMake 4 dropped compatibility with cmake_minimum_required(< 3.5), which some Externals still use.
if [ "$(cmake --version | sed -n 's/^cmake version \([0-9]*\).*/\1/p')" -ge 4 ]; then
  extra+=(-DCMAKE_POLICY_VERSION_MINIMUM=3.5)
fi

if [ ! -f "$BUILD/build.ninja" ]; then
  cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_AUTOUPDATE=OFF \
    -DENABLE_ANALYTICS=OFF \
    -DUSE_DISCORD_PRESENCE=OFF \
    -DENABLE_LLVM=OFF \
    "${extra[@]}" "$@"
fi

ninja_args=()
[ -n "${KEEP_GOING:-}" ] && ninja_args=(-- -k 0)   # report every failing file, not just the first
# shellcheck disable=SC2086
cmake --build "$BUILD" --parallel "$JOBS" --target $TARGETS "${ninja_args[@]}"
ls -la "$BUILD/Binaries"

# Optionally publish the binaries (e.g. from a fast container volume to a host bind mount).
if [ -n "${OUT:-}" ]; then
  mkdir -p "$OUT/Binaries"
  rm -rf "$OUT/Binaries.new" && cp -a "$BUILD/Binaries" "$OUT/Binaries.new"
  rm -rf "$OUT/Binaries" && mv "$OUT/Binaries.new" "$OUT/Binaries"
  echo "copied binaries to $OUT/Binaries"
fi
