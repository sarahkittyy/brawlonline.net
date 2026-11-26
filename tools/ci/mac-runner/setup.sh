#!/bin/bash
# Sets up the GitHub Actions runners' supervisor on Sarah's Mac. docs/ci.md "Sarah's Mac".
#
#   tools/ci/mac-runner/setup.sh            from a checkout: copies ci, supervise.sh and itself to
#                                           ~/runners, links the build tools, writes each runner's
#                                           .env/.path and (re)loads the LaunchAgent
#   ~/runners/setup.sh env <runner dir>     only a runner's .env and .path (ci add)
#
# The runners themselves are GitHub's actions-runner, configured in ~/runners/<name>
# (~/runners/brawlonline for sarahkittyy/brawlonline.net). Jobs see only ~/runners/bin and the
# system's folders on PATH, not all of Homebrew. bash 3.2.
set -euo pipefail
R="$HOME/runners"
S="$R/state"
LABEL=net.brawlonline.runners
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

write_env() { # <runner dir>
  local d="$1"
  cat >"$d/.env" <<EOF
LANG=en_US.UTF-8
CI_CACHE=$d/ci-cache
HOMEBREW_NO_AUTO_UPDATE=1
HOMEBREW_NO_INSTALL_UPGRADE=1
EOF
  echo "$R/bin:/usr/bin:/bin:/usr/sbin:/sbin" >"$d/.path"
}

if [ "${1:-}" = env ]; then
  write_env "${2:?runner dir}"
  exit 0
fi

here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$R/bin" "$S"
if [ "$here" != "$R" ]; then
  install -m 755 "$here/ci" "$here/supervise.sh" "$here/setup.sh" "$R/"
fi

# Build tools from Homebrew, linked one by one (cmake, ninja, ccache for Dolphin; gpg for the
# recomp's game code). Missing ones are installed without touching the rest of Homebrew.
brew=/opt/homebrew/bin/brew
for f in cmake:cmake ninja:ninja ccache:ccache gpg:gnupg; do
  tool="${f%%:*}"; formula="${f#*:}"
  if [ ! -x "/opt/homebrew/bin/$tool" ]; then
    HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_UPGRADE=1 "$brew" install "$formula"
  fi
  ln -sf "/opt/homebrew/bin/$tool" "$R/bin/$tool"
done
for t in ctest cpack; do ln -sf "/opt/homebrew/bin/$t" "$R/bin/$t"; done

for d in "$R"/*/; do
  [ -f "$d.runner" ] && write_env "${d%/}"
done

# The network the Mac is on during setup is home; `ci home` adds another.
if [ ! -s "$S/home-routers" ]; then "$R/ci" home; fi
[ -e "$S/awake.set" ] || { touch "$S/awake" "$S/awake.set"; }

mkdir -p "$(dirname "$PLIST")"
cat >"$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array><string>/bin/bash</string><string>$R/supervise.sh</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ProcessType</key><string>Standard</string>
  <key>StandardOutPath</key><string>$R/supervise.out</string>
  <key>StandardErrorPath</key><string>$R/supervise.out</string>
</dict>
</plist>
EOF
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "Supervisor loaded. 'ci on' to start the runners."
