#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CFG_FILE="$SCRIPT_DIR/openocd-jlink-npcm400.cfg"

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
SDK_OPENOCD_BIN=""
SDK_OPENOCD_SCRIPTS=""

if [[ -n "$SDK_ROOT" ]]; then
  SDK_OPENOCD_BIN="$SDK_ROOT/sysroots/x86_64-pokysdk-linux/usr/bin/openocd"
  SDK_OPENOCD_SCRIPTS="$SDK_ROOT/sysroots/x86_64-pokysdk-linux/usr/share/openocd/scripts"
fi

if [[ -n "${OPENOCD_BIN:-}" ]]; then
  RESOLVED_OPENOCD_BIN="$OPENOCD_BIN"
elif command -v openocd >/dev/null 2>&1; then
  RESOLVED_OPENOCD_BIN="$(command -v openocd)"
elif [[ -n "$SDK_OPENOCD_BIN" && -x "$SDK_OPENOCD_BIN" ]]; then
  RESOLVED_OPENOCD_BIN="$SDK_OPENOCD_BIN"
else
  echo "OpenOCD not found."
  echo "Set OPENOCD_BIN=/path/to/openocd, or set ZEPHYR_SDK_INSTALL_DIR to your SDK path."
  exit 127
fi

if [[ -n "${OPENOCD_SCRIPTS:-}" ]]; then
  RESOLVED_OPENOCD_SCRIPTS="$OPENOCD_SCRIPTS"
elif [[ -n "$SDK_OPENOCD_SCRIPTS" && -d "$SDK_OPENOCD_SCRIPTS" ]]; then
  RESOLVED_OPENOCD_SCRIPTS="$SDK_OPENOCD_SCRIPTS"
elif [[ -d "/usr/share/openocd/scripts" ]]; then
  RESOLVED_OPENOCD_SCRIPTS="/usr/share/openocd/scripts"
else
  echo "OpenOCD scripts directory not found."
  echo "Set OPENOCD_SCRIPTS=/path/to/openocd/scripts, or set ZEPHYR_SDK_INSTALL_DIR."
  exit 127
fi

TRANSPORT="${TRANSPORT:-jtag}"
ADAPTER_KHZ="${ADAPTER_KHZ:-4000}"
TARGET_CFG="${TARGET_CFG:-$SCRIPT_DIR/target-generic-cortexm.cfg}"

exec "$RESOLVED_OPENOCD_BIN" \
  -s "$RESOLVED_OPENOCD_SCRIPTS" \
  -c "set TRANSPORT $TRANSPORT" \
  -c "set ADAPTER_KHZ $ADAPTER_KHZ" \
  -c "set TARGET_CFG $TARGET_CFG" \
  -f "$CFG_FILE"
