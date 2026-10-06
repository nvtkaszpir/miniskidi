#!/usr/bin/env bash
# Watch the firmware sources; on every change: stop the serial log, compile and upload
# ("task all"), then follow the serial log again. Ctrl+C quits.
#
# Usage: scripts/watch.sh [PORT] [TASK]   (defaults: /dev/ttyUSB0, task)
# Normally started with "task watch". Needs no extra tools: it polls file times once a second.
set -u

port="${1:-/dev/ttyUSB0}"
task_exe="${2:-task}"
cd "$(dirname "$0")/.."

# Changes to these files trigger a new build (web/ is the phone app, not firmware)
fingerprint() {
  stat -c '%n %Y' ./*.ino ./*.h sketch.yaml 2>/dev/null | md5sum
}

logs_pid=""

# The monitor holds the serial port, so it must be stopped before uploading. It runs in its
# own process group (setsid), so killing the group also stops uv and arduino-cli under it.
stop_logs() {
  if [ -n "$logs_pid" ]; then
    kill -TERM -- "-$logs_pid" 2>/dev/null
    wait "$logs_pid" 2>/dev/null
    logs_pid=""
  fi
}

# arduino-cli monitor quits when its input ends, so it gets an input that never ends
start_logs() {
  setsid bash -c 'sleep infinity | exec uv run arduino-cli monitor -p "$0" -c baudrate=115200 --quiet' "$port" &
  logs_pid=$!
}

trap 'stop_logs; echo; exit 0' INT TERM

last=""
while true; do
  now=$(fingerprint)
  if [ "$now" != "$last" ]; then
    last="$now"
    stop_logs
    echo "=== $(date +%T) build and upload to $port ==="
    if "$task_exe" all PORT="$port"; then
      echo "=== $(date +%T) uploaded, following the log (Ctrl+C to quit) ==="
    else
      echo "=== $(date +%T) build or upload failed, waiting for the next change ==="
    fi
    start_logs
  fi
  sleep 1
done
