#!/bin/sh
# Build PPOnline.rel (and optionally upstream Brawlback-Online + sy_core) with the toolchain
# installed by tools/gamecode/setup_toolchain.py into ../toolchains.
#   ./build.sh            PPOnline only
#   ./build.sh all        also the upstream Brawlback-Online.rel + sy_core.rel (clean build)
set -e
cd "$(dirname "$0")"
TC="$(cd .. && pwd)/toolchains"
LLVMDIR="$(ls -d "$TC"/kuribo-llvm-* | head -1)"
E2R="$(ls "$TC"/elf2rel-*/elf2rel* | head -1)"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) LLVMDIR="$(cygpath -m "$LLVMDIR")"; E2R="$(cygpath -m "$E2R")";; esac
EXE=""; [ "$OS" = "Windows_NT" ] && EXE=".exe"
export TOPDIR="$(pwd)" LIB="$(pwd)/lib" SYRIINGE="$(pwd)/lib/Syriinge" TOOLS="$(pwd)/tools"
MK="make CC=$LLVMDIR/bin/clang$EXE CXX=$LLVMDIR/bin/clang$EXE LD=$LLVMDIR/bin/ld.lld$EXE ELF2REL=$E2R"
if [ "$1" = "all" ]; then
  mkdir -p sd-card/vBrawl/pf/plugins sd-card/vBrawl/pf/module
  make LLVMDIR="$LLVMDIR" ELF2REL="$E2R" clean >/dev/null
  make LLVMDIR="$LLVMDIR" ELF2REL="$E2R"
fi
$MK -C PPOnline
ls -l PPOnline/PPOnline.rel
# Fail on calls to symbols that are neither defined nor in the symbol maps (they would hang).
PY="${PYTHON:-$(command -v python || command -v python3)}"   # Debian/Ubuntu only have python3
"$PY" ../tools/gamecode/reltool.py check PPOnline/PPOnline.rel
