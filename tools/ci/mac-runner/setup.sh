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

# Homebrew stays out of the builds. CMake's Platform/Darwin.cmake puts `brew --prefix` (without
# brew: /opt/homebrew) first in every find_*, and Sarah's Homebrew has FFmpeg, which Dolphin then
# linked for frame dumps (needs macOS 26 and Homebrew; build-dolphin-macos.sh's check failed the
# build). Namespace's Mac has no FFmpeg, so released builds have none. This brew answers --prefix
# with an empty folder and passes everything else on.
mkdir -p "$R/no-homebrew"
cat >"$R/bin/brew" <<EOF
#!/bin/bash
# ~/runners/setup.sh: CMake asks \`brew --prefix\` where to look for libraries; nowhere.
if [ "\$*" = "--prefix" ]; then echo "$R/no-homebrew"; exit 0; fi
exec /opt/homebrew/bin/brew "\$@"
EOF
chmod 755 "$R/bin/brew"
# CMake and Ninja from PyPI in ~/runners/tools, not Homebrew's (CMake also searches its own
# install prefix).
if [ ! -x "$R/tools/bin/cmake" ]; then
  /usr/bin/python3 -m venv "$R/tools"
  "$R/tools/bin/pip" install -q --upgrade pip
  "$R/tools/bin/pip" install -q "cmake>=4,<5" ninja
fi
for t in cmake ctest cpack ninja; do ln -sf "$R/tools/bin/$t" "$R/bin/$t"; done
# ccache for Dolphin, gpg for the recomp's game code: Homebrew's, linked one by one (programs
# only; nothing searches their prefix). Missing ones are installed without touching the rest.
brew=/opt/homebrew/bin/brew
for f in ccache:ccache gpg:gnupg; do
  tool="${f%%:*}"; formula="${f#*:}"
  if [ ! -x "/opt/homebrew/bin/$tool" ]; then
    HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_UPGRADE=1 "$brew" install "$formula"
  fi
  ln -sf "/opt/homebrew/bin/$tool" "$R/bin/$tool"
done

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
# bootout returns before the old one is gone, and bootstrap fails (5: Input/output error) until
# it is; stopping the runners takes the supervisor up to 15 s.
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
for i in $(seq 1 30); do
  launchctl print "gui/$(id -u)/$LABEL" >/dev/null 2>&1 || break
  sleep 1
done
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "Supervisor loaded. 'ci on' to start the runners."
