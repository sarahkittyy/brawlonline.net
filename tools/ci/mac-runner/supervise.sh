#!/bin/bash
# Runs the GitHub Actions runners in ~/runners/*/ on Sarah's Mac while they are switched on
# (`ci on`) and the Mac is on a home network (its router's MAC address is in
# ~/runners/state/home-routers). Started by the LaunchAgent net.brawlonline.runners (setup.sh),
# which keeps it running while she is logged in. docs/ci.md "Sarah's Mac".
#
# Every 10 s: wanted = switched on and at home. A runner that is wanted and not running starts
# (./run.sh, which also updates the runner when GitHub asks); one that is running and not wanted
# is stopped (its job, if any, fails). While wanted and `ci awake` is on, caffeinate -s keeps the
# Mac from sleeping on AC power (on battery it sleeps as usual).
# bash 3.2 (macOS's own).
R="$HOME/runners"
S="$R/state"
LOG="$R/supervise.log"
mkdir -p "$S"

log() { echo "$(date '+%F %T') $*" >>"$LOG"; }

router_mac() {
  local gw mac
  gw="$(route -n get default 2>/dev/null | awk '/gateway:/ { print $2 }')"
  [ -n "$gw" ] || return 0
  mac="$(arp -n "$gw" 2>/dev/null | awk '$4 ~ /:/ { print $4; exit }')"
  if [ -z "$mac" ]; then
    ping -c 1 -t 1 "$gw" >/dev/null 2>&1
    mac="$(arp -n "$gw" 2>/dev/null | awk '$4 ~ /:/ { print $4; exit }')"
  fi
  echo "$mac"
}

at_home() {
  local mac
  mac="$(router_mac)"
  [ -n "$mac" ] && grep -qixF "$mac" "$S/home-routers" 2>/dev/null
}

runner_dirs() {
  local d
  for d in "$R"/*/; do
    [ -f "$d.runner" ] && echo "${d%/}"
  done
}

running() { # <dir>
  local p
  p="$(cat "$1/.supervisor-pid" 2>/dev/null)" && [ -n "$p" ] && kill -0 "$p" 2>/dev/null
}

start_runner() { # <dir>
  local d="$1"
  if [ -f "$d/runner.log" ] && [ "$(stat -f %z "$d/runner.log")" -gt 5000000 ]; then
    mv -f "$d/runner.log" "$d/runner.log.1"
  fi
  (cd "$d" && exec ./run.sh) >>"$d/runner.log" 2>&1 &
  echo $! >"$d/.supervisor-pid"
  log "started $(basename "$d") (pid $!)"
}

stop_runner() { # <dir>
  local d="$1" p i kc
  p="$(cat "$d/.supervisor-pid" 2>/dev/null)"
  # SIGTERM: a background job of this (non-interactive) shell ignores SIGINT, and so does the
  # runner it starts.
  pkill -TERM -f "$d/bin/Runner." 2>/dev/null
  for i in $(seq 1 15); do
    pgrep -f "$d/bin/Runner." >/dev/null 2>&1 || break
    sleep 1
  done
  pkill -KILL -f "$d/bin/Runner." 2>/dev/null
  kill "$p" 2>/dev/null
  pkill -f "$d/run-helper.sh" 2>/dev/null
  rm -f "$d/.supervisor-pid"
  # A job stopped halfway never ran its "Remove the signing keychain" step.
  for kc in "$d"/_work/_temp/*.keychain-db; do
    [ -f "$kc" ] && security delete-keychain "$kc" 2>/dev/null && log "deleted $kc"
  done
  log "stopped $(basename "$d")"
}

caffeinate_running() {
  local p
  p="$(cat "$S/caffeinate.pid" 2>/dev/null)" && [ -n "$p" ] && kill -0 "$p" 2>/dev/null
}

stop_all() {
  local d
  for d in $(runner_dirs); do
    running "$d" && stop_runner "$d"
  done
  caffeinate_running && kill "$(cat "$S/caffeinate.pid")" 2>/dev/null
  rm -f "$S/caffeinate.pid"
}

trap 'log "supervisor stopping"; stop_all; exit 0' TERM INT HUP

log "supervisor started (pid $$)"
last=""
while true; do
  if [ -f "$S/enabled" ]; then
    if at_home; then want=yes; why="on, at home"; else want=no; why="on, away from home (router $(router_mac))"; fi
  else
    want=no; why="off"
  fi
  if [ "$why" != "$last" ]; then log "$why"; last="$why"; fi
  echo "$why" >"$S/now"

  for d in $(runner_dirs); do
    if [ "$want" = yes ] && ! running "$d"; then start_runner "$d"; fi
    if [ "$want" = no ] && running "$d"; then stop_runner "$d"; fi
  done

  if [ "$want" = yes ] && [ -f "$S/awake" ]; then
    if ! caffeinate_running; then
      caffeinate -s -w $$ &
      echo $! >"$S/caffeinate.pid"
    fi
  elif caffeinate_running; then
    kill "$(cat "$S/caffeinate.pid")" 2>/dev/null
    rm -f "$S/caffeinate.pid"
  fi

  if [ "$(stat -f %z "$LOG" 2>/dev/null || echo 0)" -gt 1000000 ]; then mv -f "$LOG" "$LOG.1"; fi
  sleep 10 &
  wait $!
done
