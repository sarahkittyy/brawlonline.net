#!/usr/bin/env bash
# Builds our Dolphin on macOS (Apple silicon: the Namespace runner in CI, or a Mac) and stages it
# for the launcher package (extraResources "dolphin").
#
#   .github/scripts/build-dolphin-macos.sh <version> <out dir> [arm64|x86_64]
#
# <out dir> gets the bundle the launcher copies into <userData>/netplay on first start:
#   Dolphin.app (Contents/MacOS/Dolphin and dolphin-tool, Qt frameworks and plugins deployed by
#   macdeployqt, Sys in Contents/Resources), COPYING, Licenses/, dolphin.json (manifest). Signed
#   with Dolphin's entitlements: with the Developer ID identity in $MAC_SIGN_IDENTITY (a name or
#   SHA-1 hash, its keychain in the search list) when it is set, ad-hoc otherwise. Notarized as part
#   of the launcher app (package-launcher-macos.sh), which carries this bundle.
#
# Needs Xcode's command line tools, CMake, Ninja, ccache, Node (dolphin-manifest.sh) and Qt for
# macOS in $QT_DIR (default $CI_CACHE/qt/<QT_VERSION>/macos; installed with aqtinstall when
# missing). The build tree and ccache live in $CI_CACHE (default ~/ci-cache).
set -euo pipefail
version="${1:?version}"
out="${2:?out dir}"
arch="${3:-arm64}"
repo="$(cd "$(dirname "$0")/../.." && pwd)"
src="$repo/dolphin"
cache="${CI_CACHE:-$HOME/ci-cache}"
build="$cache/dolphin-build-macos-$arch"
jobs="${JOBS:-$(sysctl -n hw.ncpu)}"
qt_version="${QT_VERSION:-6.8.3}"
qt_dir="${QT_DIR:-$cache/qt/$qt_version/macos}"
min_macos=12.0

export CCACHE_DIR="${CCACHE_DIR:-$cache/ccache}"
export CCACHE_BASEDIR="$repo"
export CCACHE_NOHASHDIR=1
export CCACHE_COMPRESS=1
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-2G}"
mkdir -p "$CCACHE_DIR"

if [ ! -d "$qt_dir/lib/QtCore.framework" ]; then
  python3 -m venv "$cache/aqt-venv"
  "$cache/aqt-venv/bin/pip" install -q "aqtinstall==3.3.0"
  "$cache/aqt-venv/bin/aqt" install-qt mac desktop "$qt_version" clang_64 -O "$cache/qt"
fi

# Same submodules as the Linux build; Externals/Qt and FFmpeg-bin are Windows-only binaries.
(
  cd "$repo"
  git -c submodule."dolphin/Externals/Qt".update=none \
      -c submodule."dolphin/Externals/FFmpeg-bin".update=none \
      submodule update --init --recursive --depth 1 --jobs 4 -- dolphin
)

if [ -f "$build/CMakeCache.txt" ] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$src" "$build/CMakeCache.txt"; then
  rm -rf "$build"
fi
# Vulkan (MoltenVK) is off for now: Metal and OpenGL remain. Dolphin's own code signing is off
# because the bundle is changed below; it is signed once at the end.
# Qt 6.8.3's FindWrapOpenGL.cmake links "-framework AGL" unless find_library finds it, and the
# macOS 26 SDK (Xcode 26) has no AGL: WrapOpenGL_AGL names OpenGL.framework (linked anyway)
# instead, so find_library is skipped.
sdk="$(xcrun --sdk macosx --show-sdk-path)"
cmake -S "$src" -B "$build" -G Ninja \
  -DWrapOpenGL_AGL="$sdk/System/Library/Frameworks/OpenGL.framework" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$min_macos" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_OBJC_COMPILER_LAUNCHER=ccache -DCMAKE_OBJCXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_PREFIX_PATH="$qt_dir" \
  -DENABLE_VULKAN=OFF -DMACOS_CODE_SIGNING=OFF -DPOSTPROCESS_BUNDLE=OFF \
  -DENABLE_AUTOUPDATE=OFF -DENABLE_ANALYTICS=OFF -DUSE_DISCORD_PRESENCE=OFF \
  -DENABLE_LLVM=OFF -DENABLE_TESTS=OFF -DENABLE_NOGUI=OFF \
  -DDISTRIBUTOR=brawlonline.net \
  >"$build.configure.log" 2>&1 || { tail -n 60 "$build.configure.log"; exit 1; }

ccache -z >/dev/null
cmake --build "$build" --parallel "$jobs" --target project-plus-dolphin dolphin-tool
ccache -s -v   # verbose: shows why calls are uncacheable

# ---- Bundle -------------------------------------------------------------------------------
# The launcher runs <netplay>/Dolphin.app/Contents/MacOS/Dolphin and looks for dolphin-tool next
# to it (launcher/src/dolphin/install/paths.ts, src/game_assets/setup.ts).
rm -rf "$out"
mkdir -p "$out"
app="$out/Dolphin.app"
cp -R "$build/Binaries/DolphinQt.app" "$app"
mv "$app/Contents/MacOS/DolphinQt" "$app/Contents/MacOS/Dolphin"
/usr/libexec/PlistBuddy -c "Set :CFBundleExecutable Dolphin" "$app/Contents/Info.plist"
cp "$build/Binaries/dolphin-tool" "$app/Contents/MacOS/dolphin-tool"
# Dolphin's CMake copies the cocoa and style plugins next to the executable; macdeployqt deploys
# every plugin and framework into PlugIns/Frameworks and writes qt.conf, so drop those copies.
rm -rf "$app/Contents/MacOS/platforms" "$app/Contents/MacOS/styles"
"$qt_dir/bin/macdeployqt" "$app" -verbose=1

# ---- Sign -------------------------------------------------------------------------------
# Apple silicon refuses unsigned code. Dolphin's own script signs the dylibs and frameworks, then
# the bundle (hardened runtime, Dolphin's entitlements); dolphin-tool, a second executable in
# Contents/MacOS, is signed first by hand. A Developer ID signature also gets a secure timestamp,
# which notarization requires. No docs ship (dolphin-manifest.sh deletes them too, but after
# signing that would break the seal).
find "$app" -type f \( -iname '*.md' -o -iname '*.markdown' \) -print -delete
identity="${MAC_SIGN_IDENTITY:--}"
if [ "$identity" = "-" ]; then
  codesign --force --sign - "$app/Contents/MacOS/dolphin-tool"
  "$src/Tools/mac-codesign.sh" -e "$src/Source/Core/DolphinQt/DolphinEmu.entitlements" - "$app"
else
  # The identity's keychain must be in the search list (mac-codesign.sh has no --keychain).
  codesign --force --sign "$identity" --timestamp --options runtime \
    "$app/Contents/MacOS/dolphin-tool"
  "$src/Tools/mac-codesign.sh" -t -e "$src/Source/Core/DolphinQt/DolphinEmu.entitlements" "$identity" "$app"
fi
codesign --verify --deep --strict "$app"
codesign -dvv "$app" 2>&1 | grep -E '^(Authority|TeamIdentifier|Timestamp|CodeDirectory)' || true

cp "$src/COPYING" "$out/"
cp -R "$src/LICENSES" "$out/Licenses"
"$repo/.github/scripts/dolphin-manifest.sh" "$out" "$version" "Dolphin.app/Contents/MacOS/Dolphin"
file "$app/Contents/MacOS/Dolphin" "$app/Contents/MacOS/dolphin-tool"
du -sh "$out"
