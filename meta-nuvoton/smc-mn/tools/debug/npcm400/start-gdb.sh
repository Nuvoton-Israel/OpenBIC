#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

discover_zephyr_sdk_root() {
  local sdk_root=""

  if [[ -n "${ZEPHYR_SDK_INSTALL_DIR:-}" && -d "${ZEPHYR_SDK_INSTALL_DIR}" ]]; then
    sdk_root="${ZEPHYR_SDK_INSTALL_DIR}"
  elif [[ -n "${ZEPHYR_SDK_DIR:-}" && -d "${ZEPHYR_SDK_DIR}" ]]; then
    sdk_root="${ZEPHYR_SDK_DIR}"
  else
    local d
    for d in "$HOME"/zephyr-sdk-*; do
      if [[ -d "$d" ]]; then
        sdk_root="$d"
      fi
    done
  fi

  if [[ -n "$sdk_root" ]]; then
    echo "$sdk_root"
  fi
}

SDK_ROOT="$(discover_zephyr_sdk_root)"
SDK_GDB_NO_PY=""
SDK_GDB=""

if [[ -n "$SDK_ROOT" ]]; then
  SDK_GDB_NO_PY="$SDK_ROOT/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb-no-py"
  SDK_GDB="$SDK_ROOT/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb"
fi

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

if [[ -n "${GDB_BIN:-}" ]]; then
  RESOLVED_GDB_BIN="$GDB_BIN"
elif command -v arm-none-eabi-gdb >/dev/null 2>&1; then
  RESOLVED_GDB_BIN="$(command -v arm-none-eabi-gdb)"
elif command -v arm-zephyr-eabi-gdb-no-py >/dev/null 2>&1; then
  RESOLVED_GDB_BIN="$(command -v arm-zephyr-eabi-gdb-no-py)"
elif command -v arm-zephyr-eabi-gdb >/dev/null 2>&1; then
  RESOLVED_GDB_BIN="$(command -v arm-zephyr-eabi-gdb)"
elif [[ -n "$SDK_GDB_NO_PY" && -x "$SDK_GDB_NO_PY" ]]; then
  RESOLVED_GDB_BIN="$SDK_GDB_NO_PY"
elif [[ -n "$SDK_GDB" && -x "$SDK_GDB" ]]; then
  RESOLVED_GDB_BIN="$SDK_GDB"
else
  echo "GDB not found."
  echo "Set GDB_BIN=/path/to/gdb, or set ZEPHYR_SDK_INSTALL_DIR to your SDK path."
  exit 127
fi

GDB_HOST="${GDB_HOST:-localhost}"
GDB_PORT="${GDB_PORT:-3333}"
RESET_ON_ATTACH="${RESET_ON_ATTACH:-0}"

cmd=(
  "$RESOLVED_GDB_BIN" -q -nx "$ELF"
  -ex "set pagination off"
  -ex "set confirm off"
  -ex "set remotetimeout 10"
  -ex "target extended-remote ${GDB_HOST}:${GDB_PORT}"
  -ex "interrupt"
  -ex "monitor halt"
  -ex "monitor wait_halt 3000"
)

if [[ "$RESET_ON_ATTACH" == "1" ]]; then
  cmd+=(
    -ex "monitor reset init"
    -ex "monitor halt"
    -ex "monitor wait_halt 3000"
  )
fi

cmd+=(
  -ex "thbreak main"
  -ex "continue"
)

exec "${cmd[@]}"
