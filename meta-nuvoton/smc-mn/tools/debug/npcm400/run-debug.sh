#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPENOCD_SCRIPT="$SCRIPT_DIR/start-openocd.sh"
GDB_SCRIPT="$SCRIPT_DIR/start-gdb.sh"

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <path-to-elf>"
  echo "Example: $0 build/zephyr/SMCNPCM.elf"
  exit 1
fi

ELF="$1"

if [[ ! -f "$ELF" ]]; then
  echo "ELF not found: $ELF"
  exit 1
fi

GDB_HOST="${GDB_HOST:-localhost}"
GDB_PORT="${GDB_PORT:-3333}"
OPENOCD_READY_TIMEOUT="${OPENOCD_READY_TIMEOUT:-12}"
RESTART_OPENOCD="${RESTART_OPENOCD:-1}"

STARTED_OPENOCD=0
OPENOCD_PID=""

# By default restart old NPCM400 OpenOCD sessions to avoid stale/broken listeners.
if [[ "$RESTART_OPENOCD" == "1" ]]; then
  if pgrep -f "openocd.*tools/debug/npcm400/openocd-jlink-npcm400.cfg" >/dev/null 2>&1; then
    echo "Stopping previous NPCM400 OpenOCD session(s)."
    pkill -f "openocd.*tools/debug/npcm400/openocd-jlink-npcm400.cfg" || true
    for _ in {1..20}; do
      if ! pgrep -f "openocd.*tools/debug/npcm400/openocd-jlink-npcm400.cfg" >/dev/null 2>&1; then
        break
      fi
      sleep 0.1
    done
  fi
elif (exec 3<>"/dev/tcp/${GDB_HOST}/${GDB_PORT}") 2>/dev/null; then
  exec 3>&-
  exec 3<&-
  echo "Detected existing OpenOCD on ${GDB_HOST}:${GDB_PORT}; reusing it."
fi

"$OPENOCD_SCRIPT" >/tmp/npcm400-openocd.log 2>&1 &
OPENOCD_PID=$!
STARTED_OPENOCD=1

cleanup() {
  if [[ "$STARTED_OPENOCD" -eq 1 ]] && kill -0 "$OPENOCD_PID" 2>/dev/null; then
    kill "$OPENOCD_PID" 2>/dev/null || true
    wait "$OPENOCD_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

# Wait until OpenOCD GDB server socket is ready, or timeout.
start_ts=$SECONDS
while :; do
  if [[ "$STARTED_OPENOCD" -eq 1 ]] && ! kill -0 "$OPENOCD_PID" 2>/dev/null; then
    echo "OpenOCD exited unexpectedly. Log: /tmp/npcm400-openocd.log"
    tail -n 80 /tmp/npcm400-openocd.log || true
    if grep -q "LIBUSB_ERROR_BUSY" /tmp/npcm400-openocd.log 2>/dev/null; then
      echo "Hint: J-Link is busy. Stop any existing OpenOCD/JLinkGDBServer session, then retry."
    fi
    exit 1
  fi

  if (exec 3<>"/dev/tcp/${GDB_HOST}/${GDB_PORT}") 2>/dev/null; then
    exec 3>&-
    exec 3<&-
    break
  fi

  if (( SECONDS - start_ts >= OPENOCD_READY_TIMEOUT )); then
    echo "Timed out waiting for OpenOCD on ${GDB_HOST}:${GDB_PORT}."
    echo "Check log: /tmp/npcm400-openocd.log"
    tail -n 80 /tmp/npcm400-openocd.log || true
    exit 1
  fi
done

echo "OpenOCD is ready on ${GDB_HOST}:${GDB_PORT}."

"$GDB_SCRIPT" "$ELF"
