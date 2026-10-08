#!/usr/bin/env bash
# Installs the parts of the macOS cross toolchain that live in the CI cache (idempotent):
#   $CI_CACHE/toolchains/macos/qt/<QT_VERSION>/{macos,gcc_64}  Qt for macOS (universal frameworks)
#                                                              and the same Qt's Linux host tools
#   $CI_CACHE/toolchains/macos/bin/rcodesign                   ad-hoc code signing on Linux
#   $CI_CACHE/toolchains/macos/bin/dmg                         libdmg-hfsplus's DMG compressor
# and prints the bin folder for PATH. osxcross itself (with Apple's SDK) and LLVM are in the
# runner image (docs/unraid-runner.md).
set -euo pipefail
cache="${CI_CACHE:-$HOME/ci-cache}"
dir="$cache/toolchains/macos"
qt_version="${QT_VERSION:-6.8.3}"
rcodesign_version=0.29.0
mkdir -p "$dir/bin"

if [ ! -d "$dir/qt/$qt_version/macos/lib/QtCore.framework" ] || [ ! -x "$dir/qt/$qt_version/gcc_64/libexec/moc" ]; then
  python3 -m venv "$dir/aqt-venv"
  "$dir/aqt-venv/bin/pip" install -q "aqtinstall==3.3.0"
  (
    cd "$dir"
    [ -d "qt/$qt_version/macos/lib/QtCore.framework" ] \
      || timeout 1200 "$dir/aqt-venv/bin/aqt" install-qt mac desktop "$qt_version" clang_64 -O qt -m qtimageformats
    [ -x "qt/$qt_version/gcc_64/libexec/moc" ] \
      || timeout 1200 "$dir/aqt-venv/bin/aqt" install-qt linux desktop "$qt_version" linux_gcc_64 -O qt
  )
fi

if [ ! -x "$dir/bin/rcodesign" ]; then
  tmp="$(mktemp -d)"
  curl -fsSL "https://github.com/indygreg/apple-platform-rs/releases/download/apple-codesign%2F$rcodesign_version/apple-codesign-$rcodesign_version-x86_64-unknown-linux-musl.tar.gz" \
    | tar -xz -C "$tmp"
  install -m 755 "$tmp"/apple-codesign-*/rcodesign "$dir/bin/rcodesign"
  rm -rf "$tmp"
fi

if [ ! -x "$dir/bin/dmg" ]; then
  tmp="$(mktemp -d)"
  git clone -q --depth 1 -b only_what_core_needs https://github.com/fanquake/libdmg-hfsplus "$tmp/libdmg"
  cmake -S "$tmp/libdmg" -B "$tmp/libdmg/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$tmp/libdmg/build" >/dev/null
  install -m 755 "$tmp/libdmg/build/dmg/dmg" "$dir/bin/dmg"
  rm -rf "$tmp"
fi

"$dir/bin/rcodesign" --version >&2
echo "$dir/bin"
