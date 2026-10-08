#!/usr/bin/env bash
# Run the ppharness test suite against a Linux build inside the container.
#
#   test.sh [pytest args...]          (default: the quick suite, no game needed)
#   test.sh ppharness <command...>    python -m ppharness ... (e.g. probe)
#   test.sh tool <script.py> <args>   a harness/tools script
#
# Expected mounts (see docs/linux-build.md):
#   /work/harness                ppharness + tests (read-only is fine; caches go to /tmp)
#   /work/game/SSBB_NTSC.iso     the Brawl disc (read-only)
#   /work/run/template-user      the Dolphin user dir template (read-only)
#   /build/Binaries              the Linux build (PPHARNESS_DOLPHIN_DIR), or /out/Binaries
# Instance dirs go to PPHARNESS_INSTANCES (default /instances, ideally a tmpfs).
set -euo pipefail

export PPHARNESS_ROOT=${PPHARNESS_ROOT:-/work}
if [ -z "${PPHARNESS_DOLPHIN_DIR:-}" ]; then
  if [ -x /build/Binaries/dolphin-emu-nogui ] || [ ! -d /out/Binaries ]; then
    PPHARNESS_DOLPHIN_DIR=/build/Binaries
  else
    PPHARNESS_DOLPHIN_DIR=/out/Binaries
  fi
fi
export PPHARNESS_DOLPHIN_DIR
export PPHARNESS_INSTANCES=${PPHARNESS_INSTANCES:-/instances}
export PPHARNESS_ISO=${PPHARNESS_ISO:-/work/game/SSBB_NTSC.iso}
export PYTHONDONTWRITEBYTECODE=1
mkdir -p "$PPHARNESS_INSTANCES"

cd "$PPHARNESS_ROOT/harness"
case "${1:-}" in
  ppharness) shift; exec python3 -m ppharness "$@" ;;   # test.sh ppharness probe
  tool) shift; t=$1; shift; exec python3 "tools/$t" "$@" ;;  # test.sh tool xplat_trace.py run ...
  python) shift; exec python3 "$@" ;;
esac
if [ $# -eq 0 ]; then
  set -- -m "not dolphin and not slow"
fi
exec python3 -m pytest -p no:cacheprovider "$@"
