# NPCM400 Beginner Debug Guide (J-Link + OpenOCD + GDB, JTAG)

This guide is written for first-time bring-up.
Primary goal: get one stable JTAG debug session first, then tune performance.

## 30-second quick start

1. Connect J-Link by JTAG (TCK, TMS, TDI, TDO, GND, VTref, nRESET recommended).
2. Prepare an ELF (for example `build/zephyr/SMCNPCM.elf`).
3. Run:

```bash
tools/debug/npcm400/run-debug.sh build/zephyr/SMCNPCM.elf
```

## Success checklist

1. You see `OpenOCD is ready on localhost:3333`.
2. GDB prints `Reading symbols from ...`.
3. `bt` and `info registers` return valid data.

## VS Code tasks

From `Terminal -> Run Task...`, use:

1. `NPCM400: Run Debug (OpenOCD + GDB Attach)`

When prompted for `Path to ELF`, enter your ELF path.

Important for JTAG users:

- The scripts now default to JTAG.
- You only need to set `TRANSPORT` if you want to override transport type.

Example (force SWD):

```bash
TRANSPORT=swd tools/debug/npcm400/run-debug.sh build/zephyr/SMCNPCM.elf
```

## Useful GDB commands

```gdb
monitor halt
monitor reset
info registers
bt
x/16wx 0x20000000
```

## Common issues

### 1) `LIBUSB_ERROR_BUSY` or `No J-Link device found`

Another process owns the probe.

Fix:

1. Stop old OpenOCD/JLinkGDBServer sessions.
2. Retry `run-debug.sh`.

### 2) `Undefined command: deactivate`

You sourced a shell activate script inside GDB.

Fix:

1. Exit GDB: `quit`
2. In shell, run `source .venv/bin/activate`

### 3) Cannot connect to target

Try lower adapter speed:

```bash
ADAPTER_KHZ=1000 tools/debug/npcm400/start-openocd.sh
```

Then check wiring and board power again.

### 4) `timed out while waiting for target halted` / `TARGET: npcm400.cpu - Not halted`

This means OpenOCD can see the JTAG TAP, but the CPU does not enter halt state reliably after reset.

Try these in order:

1. Lower JTAG speed (for example `ADAPTER_KHZ=1000` or `500`).
2. Ensure `nRESET` is connected and stable.
3. Retry with a fresh session (`run-debug.sh` already restarts stale OpenOCD by default).
4. In GDB, manually run:

```gdb
monitor halt
monitor reset
```

### 5) Cannot find OpenOCD scripts or target cfg

Set explicit paths:

```bash
OPENOCD_SCRIPTS=/path/to/openocd/scripts \
TARGET_CFG=/path/to/npcm400-target.cfg \
tools/debug/npcm400/start-openocd.sh
```

If your board has stable nRESET wiring and you want SRST-based reset behavior:

```bash
RESET_CONFIG='srst_only srst_nogate connect_assert_srst' tools/debug/npcm400/start-openocd.sh
```

## Advanced options

Reuse an existing OpenOCD session (default behavior is restart):

```bash
RESTART_OPENOCD=0 tools/debug/npcm400/run-debug.sh build/zephyr/SMCNPCM.elf
```

Use custom binaries:

- `OPENOCD_BIN=/path/to/openocd`
- `GDB_BIN=/path/to/gdb`

Enable reset during GDB attach (default is off for better compatibility):

- `RESET_ON_ATTACH=1`
