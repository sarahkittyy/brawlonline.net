#!/bin/sh
# Build the plugin's three files (and optionally upstream Brawlback-Online + sy_core) with the toolchain
# installed by tools/gamecode/setup_toolchain.py into ../toolchains.
# PPOnline.rel is packed: the loader (PPOnlineLoader, what sy_core loads into P+'s Syringe heap)
# followed by the plugin (PPOnline/PPOnlineMain.rel, REL id 20560), which the loader links into
# the Network heap (docs/game-code.md section 2, "Heap budget").
#   ./build.sh            the plugins only (PPOnline.rel, PPOnline-pex.rel, PPOnline-rex.rel)
#   ./build.sh all        a clean build of everything, also the upstream Brawlback-Online.rel +
#                         sy_core.rel
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
# The plugin, one per kind of build (PPOnline/include/variant.h): PPOnline.rel for Project+
# itself, PPOnline-pex.rel for P+ v3.2 with BrawlEx (Project+ Ex, Cobalt), PPOnline-rex.rel for
# REX. A build's entry of server/config/games.json names the one it takes (`plugin`); on the card
# each is PPOnline.rel (the loader reads itself again by that name).
VARIANTS="pplus pex rex"
# What a plugin may take of the Network heap (text + data + bss of PPOnlineMain*.elf, bytes):
# Project+'s biggest costumes are decompressed there too, and 223,336 was the most that left them
# room (224,524 did not: harness/tests/test_online_big_costumes.py, test_special_modes.py's
# costume files; docs/game-code.md section 2). REX's plugin is linked into a pool of its own
# (PPOnlineLoader), so it has no such limit. A build over its budget fails here, not on a costume.
budget() { case "$1" in rex) echo 0;; *) echo 222000;; esac; }
if [ "$1" = "all" ]; then
  for v in $VARIANTS; do $MK -s -C PPOnline VARIANT=$v clean; done
  $MK -s -C PPOnlineLoader clean
fi
$MK -C PPOnlineLoader
PY="${PYTHON:-$(command -v python || command -v python3)}"   # Debian/Ubuntu only have python3
over=0
for v in $VARIANTS; do
  sfx=""; [ "$v" = pplus ] || sfx="-$v"
  $MK -C PPOnline VARIANT=$v
  "$PY" ../tools/gamecode/reltool.py pack PPOnlineLoader/PPOnlineLoader.rel PPOnline/PPOnlineMain$sfx.rel PPOnline/PPOnline$sfx.rel
  # Fail on references to symbols that are neither defined nor in the symbol maps (elf2rel only
  # warns and leaves them 0: a call goes to address 0, or loops), and on relative branches out of
  # the plugin (it runs from MEM2, out of their range). The ELFs name the symbol and its user.
  "$PY" ../tools/gamecode/reltool.py check PPOnline/PPOnline$sfx.rel \
    --elf loader=PPOnlineLoader/PPOnlineLoader.elf --elf main=PPOnline/PPOnlineMain$sfx.elf
  size=$("$LLVMDIR/bin/llvm-size$EXE" PPOnline/PPOnlineMain$sfx.elf | tail -1 | awk '{print $1 + $2 + $3}')
  b=$(budget $v)
  if [ "$b" = 0 ]; then
    echo "PPOnline$sfx.rel: $size bytes in memory (no budget: its own pool)"
  elif [ "$size" -gt "$b" ]; then
    echo "PPOnline$sfx.rel: $size bytes in memory, $((size - b)) OVER its budget of $b (build.sh)" >&2
    over=1
  else
    echo "PPOnline$sfx.rel: $size bytes in memory, $((b - size)) under its budget of $b"
  fi
done
ls -l PPOnline/PPOnline*.rel
[ "$over" = 0 ]
