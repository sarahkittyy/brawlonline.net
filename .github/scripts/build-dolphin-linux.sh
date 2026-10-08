#!/usr/bin/env bash
# Builds our Dolphin for Linux and stages it for the launcher package (extraResources "dolphin").
#
#   .github/scripts/build-dolphin-linux.sh <version> <out dir>
#
# <out dir> gets the bundle the launcher copies into <userData>/netplay on first start:
#   usr/bin/project-plus-dolphin, usr/bin/dolphin-tool, usr/bin/Sys/, usr/lib/ (bundled libraries,
#   linuxdeploy + its Qt plugin), COPYING, Licenses/, dolphin.json (manifest).
#
# Incremental: the build tree and ccache live in $CI_CACHE (default ~/ci-cache), outside the
# checkout. Low priority (nice 19, idle IO) and $JOBS jobs: this runs on the production box.
# Needs Dolphin's Linux build dependencies (see docs/ci.md) and stamp-version.sh run first.
set -euo pipefail
version="${1:?version}"
out="${2:?out dir}"
repo="$(cd "$(dirname "$0")/../.." && pwd)"
src="$repo/dolphin"
cache="${CI_CACHE:-$HOME/ci-cache}"
build="$cache/dolphin-build-linux"
tools="$cache/tools"
jobs="${JOBS:-3}"

export CCACHE_DIR="$cache/ccache"
export CCACHE_BASEDIR="$repo"
export CCACHE_NOHASHDIR=1
export CCACHE_COMPRESS=1
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-3G}"
mkdir -p "$CCACHE_DIR" "$tools"

# Submodules Dolphin needs on Linux. Externals/Qt and Externals/FFmpeg-bin are Windows-only
# prebuilt binaries (hundreds of MB): skipped.
(
  cd "$repo"
  git -c submodule."dolphin/Externals/Qt".update=none \
      -c submodule."dolphin/Externals/FFmpeg-bin".update=none \
      submodule update --init --recursive --depth 1 --jobs 4 -- dolphin
)

# Rebuild the tree from scratch if it was configured for another source folder.
if [ -f "$build/CMakeCache.txt" ] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$src" "$build/CMakeCache.txt"; then
  rm -rf "$build"
fi
if [ ! -f "$build/build.ninja" ]; then
  extra=()
  if [ "$(cmake --version | sed -n 's/^cmake version \([0-9]*\).*/\1/p')" -ge 4 ]; then
    extra+=(-DCMAKE_POLICY_VERSION_MINIMUM=3.5)
  fi
  nice -n 19 cmake -S "$src" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON \
    -DENABLE_AUTOUPDATE=OFF -DENABLE_ANALYTICS=OFF -DUSE_DISCORD_PRESENCE=OFF \
    -DENABLE_LLVM=OFF -DENABLE_TESTS=OFF -DENABLE_NOGUI=OFF \
    -DDISTRIBUTOR=brawlonline.net \
    "${extra[@]}"
else
  # Re-run CMake so the revision header picks up the new tag (ScmRevGen runs at configure time).
  nice -n 19 cmake "$build" >/dev/null
fi

ccache -z >/dev/null
nice -n 19 ionice -c 3 cmake --build "$build" --parallel "$jobs" --target project-plus-dolphin dolphin-tool
ccache -s | sed -n '1,12p'

# ---- AppDir (linuxdeploy bundles the shared libraries and Qt plugins) ----------------------
LD_TAG=1-alpha-20251107-1
LD_SHA=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
LDQT_TAG=1-alpha-20250213-1
LDQT_SHA=15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
fetch() { # url sha256 dest
  if [ ! -f "$3" ] || ! echo "$2  $3" | sha256sum -c --status; then
    curl -fsSL --retry 3 -o "$3.part" "$1"
    echo "$2  $3.part" | sha256sum -c --status || { echo "sha256 mismatch for $1" >&2; rm -f "$3.part"; exit 1; }
    mv "$3.part" "$3"
  fi
  chmod +x "$3"
}
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LD_TAG/linuxdeploy-x86_64.AppImage" \
  "$LD_SHA" "$tools/linuxdeploy-x86_64.AppImage"
fetch "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/$LDQT_TAG/linuxdeploy-plugin-qt-x86_64.AppImage" \
  "$LDQT_SHA" "$tools/linuxdeploy-plugin-qt-x86_64.AppImage"

appdir="$(mktemp -d "${RUNNER_TEMP:-/tmp}/dolphin-appdir.XXXXXX")/AppDir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" "$appdir/usr/share/icons/hicolor/256x256/apps"
install -m 0755 "$build/Binaries/project-plus-dolphin" "$build/Binaries/dolphin-tool" "$appdir/usr/bin/"
cp -r "$src/Data/Sys" "$appdir/usr/bin/Sys"
cp "$src/Data/project-plus-dolphin.desktop" "$appdir/usr/share/applications/"
cp "$src/Data/project-plus-dolphin.png" "$appdir/usr/share/icons/hicolor/256x256/apps/"
(
  export APPIMAGE_EXTRACT_AND_RUN=1 QMAKE="$(command -v qmake6)"
  export PATH="$tools:$PATH"
  cd "$(dirname "$appdir")"
  nice -n 19 "$tools/linuxdeploy-x86_64.AppImage" --appdir "$appdir" \
    --executable "$appdir/usr/bin/project-plus-dolphin" \
    --executable "$appdir/usr/bin/dolphin-tool" \
    --desktop-file "$appdir/usr/share/applications/project-plus-dolphin.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/256x256/apps/project-plus-dolphin.png" \
    --plugin qt >"$(dirname "$appdir")/linuxdeploy.log" 2>&1 ||
    { tail -n 50 "$(dirname "$appdir")/linuxdeploy.log"; exit 1; }
)

rm -rf "$out"
mkdir -p "$out"
cp -a "$appdir/usr" "$out/usr"
rm -rf "$out/usr/share/applications" "$out/usr/share/icons" "$out/usr/share/doc" "$out/usr/share/metainfo"
cp "$src/COPYING" "$out/COPYING"
cp -r "$src/LICENSES" "$out/Licenses"
rm -rf "$(dirname "$appdir")"

"$repo/.github/scripts/dolphin-manifest.sh" "$out" "$version" usr/bin/project-plus-dolphin
# The version string is compiled in ("Project+ Dolphin v<version>"); the binary needs a display to run.
grep -a -q "Project+ Dolphin v$version" "$out/usr/bin/project-plus-dolphin" ||
  { echo "the build does not report v$version" >&2; exit 1; }
