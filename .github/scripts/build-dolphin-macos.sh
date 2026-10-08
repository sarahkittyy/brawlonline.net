#!/usr/bin/env bash
# Cross-compiles our Dolphin for macOS on Linux (osxcross) and stages it for the launcher package
# (extraResources "dolphin").
#
#   .github/scripts/build-dolphin-macos.sh <version> <out dir> [arm64|x86_64]
#
# <out dir> gets the bundle the launcher copies into <userData>/netplay on first start:
#   Dolphin.app (Contents/MacOS/Dolphin and dolphin-tool, Qt frameworks in Contents/Frameworks,
#   Sys in Contents/Resources), COPYING, Licenses/, dolphin.json (manifest). Ad-hoc signed with
#   rcodesign; notarization needs Apple credentials and is not done here.
#
# Needs the macOS toolchain (docs/ci.md "macOS"): osxcross in $OSXCROSS_TARGET_DIR (default
# /opt/osxcross) built against Apple's SDK, with its <triple>-ranlib wrapped to drop Apple's
# "-no_warning_for_no_symbols -c" (CMake's Darwin rules pass them, llvm-ranlib refuses them),
# LLVM 20, rcodesign, and Qt for macOS plus the same
# Qt's Linux host tools in $CI_CACHE/toolchains/macos/qt/<version> (setup-macos-toolchain.sh).
# Incremental: the build tree and ccache live in $CI_CACHE. Low priority (nice 19, idle IO).
set -euo pipefail
version="${1:?version}"
out="${2:?out dir}"
arch="${3:-arm64}"
repo="$(cd "$(dirname "$0")/../.." && pwd)"
src="$repo/dolphin"
cache="${CI_CACHE:-$HOME/ci-cache}"
build="$cache/dolphin-build-macos-$arch"
jobs="${JOBS:-3}"
osx="${OSXCROSS_TARGET_DIR:-/opt/osxcross}"
qt_version="${QT_VERSION:-6.8.3}"
qt_root="$cache/toolchains/macos/qt/$qt_version"
qt_mac="$qt_root/macos"
qt_host="$qt_root/gcc_64"
min_macos=12.0

export PATH="$osx/bin:/usr/lib/llvm-20/bin:$PATH"
triple="$(ls "$osx/bin" | sed -n "s/^\(${arch}-apple-darwin[0-9.]*\)-clang\$/\1/p" | head -1)"
[ -n "$triple" ] || { echo "no osxcross clang for $arch in $osx/bin" >&2; exit 1; }
[ -d "$qt_mac/lib/QtCore.framework" ] || { echo "Qt for macOS missing: $qt_mac" >&2; exit 1; }
[ -x "$qt_host/libexec/moc" ] || { echo "Qt host tools missing: $qt_host" >&2; exit 1; }
sdk="$(ls -d "$osx"/SDK/MacOSX*.sdk | head -1)"
# Qt 6.8's FindWrapOpenGL links "-framework AGL" unless WrapOpenGL_AGL names a library; Apple's
# SDK 26 has no AGL (Dolphin doesn't use it), so it names OpenGL, which is linked anyway.
export OSXCROSS_HOST="$triple" OSXCROSS_TARGET_DIR="$osx" MACOSX_DEPLOYMENT_TARGET="$min_macos"

export CCACHE_DIR="$cache/ccache"
export CCACHE_BASEDIR="$repo"
export CCACHE_NOHASHDIR=1
export CCACHE_COMPRESS=1
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-6G}"
mkdir -p "$CCACHE_DIR"

if [ -d "$repo/.git" ]; then
  # Same submodules as the Linux build; Externals/Qt and FFmpeg-bin are Windows-only binaries.
  (
    cd "$repo"
    git -c submodule."dolphin/Externals/Qt".update=none \
        -c submodule."dolphin/Externals/FFmpeg-bin".update=none \
        submodule update --init --recursive --depth 1 --jobs 4 -- dolphin
  )
fi

if [ -f "$build/CMakeCache.txt" ] && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$src" "$build/CMakeCache.txt"; then
  rm -rf "$build"
fi
# osxcross's cmake wrapper sets the compilers, the SDK and the target for its toolchain file.
# Vulkan (MoltenVK) is off: Dolphin builds MoltenVK with xcodebuild. Metal and OpenGL remain.
nice -n 19 "$osx/bin/$triple-cmake" -S "$src" -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$osx/toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$min_macos" \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_OBJC_COMPILER_LAUNCHER=ccache -DCMAKE_OBJCXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON \
  -DCMAKE_PREFIX_PATH="$qt_mac" -DCMAKE_FIND_ROOT_PATH="$qt_mac" -DQT_HOST_PATH="$qt_host" \
  -DCMAKE_INSTALL_NAME_TOOL="$osx/bin/$triple-install_name_tool" \
  -DWrapOpenGL_AGL="$sdk/System/Library/Frameworks/OpenGL.framework" \
  -DENABLE_VULKAN=OFF -DMACOS_CODE_SIGNING=OFF -DPOSTPROCESS_BUNDLE=OFF \
  -DENABLE_AUTOUPDATE=OFF -DENABLE_ANALYTICS=OFF -DUSE_DISCORD_PRESENCE=OFF \
  -DENABLE_LLVM=OFF -DENABLE_TESTS=OFF -DENABLE_NOGUI=OFF \
  -DDISTRIBUTOR=brawlonline.net \
  >"$build.configure.log" 2>&1 || { tail -n 60 "$build.configure.log"; exit 1; }

ccache -z >/dev/null
build_dolphin() { nice -n 19 ionice -c 3 cmake --build "$build" --parallel "$1" --target project-plus-dolphin dolphin-tool; }
# Resumed with fewer jobs on a failure, as in build-dolphin-linux.sh (the Unraid runner's
# compilers crash now and then under parallel load).
attempt=1
until build_dolphin "$jobs"; do
  attempt=$((attempt + 1))
  [ "$attempt" -le 6 ] || { echo "Dolphin build failed 6 times" >&2; exit 1; }
  jobs=$(( jobs > 4 ? jobs * 2 / 3 : jobs ))
  echo "::warning::Dolphin build failed; retry $attempt with $jobs jobs"
done
ccache -s | sed -n '1,12p'

# ---- Bundle -------------------------------------------------------------------------------
# The launcher runs <netplay>/Dolphin.app/Contents/MacOS/Dolphin and looks for dolphin-tool next
# to it (launcher/src/dolphin/install/paths.ts, src/game_assets/setup.ts).
rm -rf "$out"
mkdir -p "$out"
app="$out/Dolphin.app"
cp -R "$build/Binaries/DolphinQt.app" "$app"
mv "$app/Contents/MacOS/DolphinQt" "$app/Contents/MacOS/Dolphin"
sed -i 's|<string>DolphinQt</string>|<string>Dolphin</string>|' "$app/Contents/Info.plist"
grep -A1 CFBundleExecutable "$app/Contents/Info.plist" | grep -q '<string>Dolphin</string>' \
  || { echo "CFBundleExecutable was not renamed" >&2; exit 1; }
cp "$build/Binaries/dolphin-tool" "$app/Contents/MacOS/dolphin-tool"

# Qt frameworks and plugins. Qt's own install names are @rpath/<Name>.framework/..., so copying
# the frameworks into Contents/Frameworks and adding that rpath is enough.
mkdir -p "$app/Contents/Frameworks" "$app/Contents/PlugIns"
otool="$osx/bin/$triple-otool"
[ -x "$otool" ] || otool=llvm-otool
install_name_tool="$osx/bin/$triple-install_name_tool"
[ -x "$install_name_tool" ] || install_name_tool=llvm-install-name-tool
needed_frameworks() {
  "$otool" -L "$@" 2>/dev/null | sed -n 's|^[[:space:]]*@rpath/\(Qt[A-Za-z0-9]*\)\.framework/.*|\1|p' | sort -u
}
for plugin in platforms/libqcocoa.dylib styles/libqmacstyle.dylib imageformats/libqsvg.dylib \
              iconengines/libqsvgicon.dylib; do
  mkdir -p "$app/Contents/PlugIns/$(dirname "$plugin")"
  cp "$qt_mac/plugins/$plugin" "$app/Contents/PlugIns/$plugin"
done
# Dolphin's CMake also copies the cocoa and style plugins into MacOS/ (qt.conf is empty, so Qt
# looks next to the executable); keep one copy, under PlugIns, and point qt.conf there.
rm -rf "$app/Contents/MacOS/platforms" "$app/Contents/MacOS/styles"
printf '[Paths]\nPlugins = PlugIns\n' > "$app/Contents/Resources/qt.conf"
queue=("$app/Contents/MacOS/Dolphin")
while IFS= read -r -d '' p; do queue+=("$p"); done < <(find "$app/Contents/PlugIns" -name '*.dylib' -print0)
seen=" "
while [ "${#queue[@]}" -gt 0 ]; do
  bin="${queue[0]}"; queue=("${queue[@]:1}")
  for fw in $(needed_frameworks "$bin"); do
    case "$seen" in *" $fw "*) continue ;; esac
    seen="$seen$fw "
    cp -R "$qt_mac/lib/$fw.framework" "$app/Contents/Frameworks/"
    rm -rf "$app/Contents/Frameworks/$fw.framework/Headers" "$app/Contents/Frameworks/$fw.framework/Versions/A/Headers"
    queue+=("$app/Contents/Frameworks/$fw.framework/Versions/A/$fw")
  done
done
echo "Qt frameworks:$seen"
"$install_name_tool" -add_rpath "@executable_path/../Frameworks" "$app/Contents/MacOS/Dolphin" 2>/dev/null || true

# ---- Sign (ad-hoc) ------------------------------------------------------------------------
# Apple silicon refuses unsigned code. rcodesign signs nested code (frameworks, dylibs, the
# helper tool) before the bundle. The JIT entitlement only matters under the hardened runtime,
# which needs a real identity (not done yet), but it is embedded already.
rcodesign sign --entitlements-xml-file "$src/Source/Core/DolphinQt/DolphinEmu.entitlements" \
  "$app" >"$out.sign.log" 2>&1 || { tail -n 40 "$out.sign.log"; exit 1; }
grep -q "signing" "$out.sign.log" || { echo "rcodesign signed nothing" >&2; exit 1; }

cp "$src/COPYING" "$out/"
cp -R "$src/LICENSES" "$out/Licenses"
"$repo/.github/scripts/dolphin-manifest.sh" "$out" "$version" "Dolphin.app/Contents/MacOS/Dolphin"
file "$app/Contents/MacOS/Dolphin" "$app/Contents/MacOS/dolphin-tool"
du -sh "$out"
