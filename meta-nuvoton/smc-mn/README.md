# Nuvoton NPCM400 Evaluation Board

This repository provides support for the Nuvoton NPCM400 Evaluation Board (EVB) within the OpenBIC framework.

## Table of Contents
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Building the Project](#building-the-project)
  - [Flash Programming](#flash-programming)
    - [Method 1: J-Link](#j-link)
    - [Method 2: NpcmFwProg](#npcmfwprog)
- [Features & Verification](#features--verification)
  - [PLDM over MCTP over USB](#pldm-over-mctp-over-usb)
  - [PLDM Firmware Update](#pldm-firmware-update)
- [Contact Information](#contact-information)

---

## Getting Started

### Prerequisites
Refer to the [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) to set up your development environment.

### Building the Project
Run the following command to build the OpenBIC firmware for NPCM400:
```bash
west -z zephyr_nuvoton build -p always -b npcm400f_evb openbic/meta-nuvoton/smc-mn/
```

### Flash Programming

#### J-Link
The NPCM400F supports J-Link flash programming.

1. Copy the [NUVOTON folder][def] to the appropriate directory for your OS:
   - **Windows**: `C:\Users\<USER>\AppData\Roaming\SEGGER\JLinkDevices\`
   - **Linux**: `$HOME/.config/SEGGER/JLinkDevices/`
   - **macOS**: `$HOME/Library/Application Support/SEGGER/JLinkDevices/`

2. Flash the image to your board:
   ```bash
   cp build/zephyr/SMCNPCM_signed.bin build/zephyr/zephyr_signed.bin
   west flash
   ```

#### NpcmFwProg
`NpcmFwProg` is a tool to program firmware over a TTY serial connection. Refer to the [npcmFwProg repository](https://github.com/Nuvoton-Israel/npcmFwProg) for details.
> **Note**: Execute the command `flash erase spi_spim0_cs0 0x0 0x4000` in the SMC console before using the `npcmFwProg` tool to update the SMC firmware.

[def]: https://github.com/Nuvoton-Israel/zephyr/tree/openbic-v2.6/boards/arm/npcm400f_evb/NUVOTON

---

## Features & Verification

### PLDM over MCTP over USB

1. **Physical Connection**: Connect the SMC USB device to the BMC USB HOST.
   - Ensure the BMC uses the [latest OpenBMC image](https://github.com/Nuvoton-Israel/openbmc).
   - if you connect the SMC after BMC is booted, you need to run `systemctl restart xyz.openbmc_project.mctpreactor.service` in the OpenBMC.

2. **Verify MCTP Link**:
   ```bash
   mctp link
   # Expected: dev mctpusb0 index 9 address none net 1 mtu 68 up
   ```

3. **Verify MCTP Route**:
   ```bash
   mctp route
   # Expected: eid min 10 max 10 net 1 dev mctpusb0 mtu 68
   ```

4. **Query PLDM Info**:
   ```bash
   # Get PLDM Types
   pldmtool base GetPLDMTypes -m 10

   # Get Firmware Parameters
   pldmtool fw_update GetFwParams -m 10
   ```

5. **Sensor Data (SDR)**:
   If PLDM sensors are enabled in [plat_def.h](https://github.com/Nuvoton-Israel/OpenBIC/blob/npcm_main_rebase/meta-nuvoton/smc-mn/src/platform/plat_def.h), retrieve ADC data via BMC:
   ```bash
   ipmitool sdr
   ```
   *Example Output:*
   ```text
   NPCM_AVSB        | 3.28 Volts        | ok
   NPCM_VCC         | 3.29 Volts        | ok
   NPCM_VHIF        | 1.93 Volts        | cr
   NPCM_VSB         | 3.28 Volts        | ok
   ```

### PLDM Firmware Update

1. **Generate PLDM Package**:
   Use [pldm_fwup_pkg_creator.py](https://github.com/openbmc/pldm/tree/master/tools/fw-update):
   ```bash
   python3 pldm_fwup_pkg_creator.py SMCNPCM_signed_with_header.bin npcm400.json SMCNPCM_signed.bin
   ```
   The `npcm400.json` you can find from "[meta-nuvoton/smc-mn/pldm_fw_package/npcm400.json](https://github.com/Nuvoton-Israel/OpenBIC/blob/npcm_main_rebase/meta-nuvoton/smc-mn/pldm_fw_package/npcm400.json)"

2. **Transfer to BMC**:
   ```bash
   cd /tmp/images/
   tftp -g -r SMCNPCM_signed_with_header.bin <HOST_IP>
   ```

3. **Initiate Update**:
   ```bash
   busctl call \
     xyz.openbmc_project.PLDM \
     /xyz/openbmc_project/software/pldm \
     xyz.openbmc_project.Software.Update \
     StartUpdate \
     hs \
     3 \
     "xyz.openbmc_project.Software.ApplyTime.RequestedApplyTimes.Immediate" \
     3< /tmp/images/SMCNPCM_signed_with_header.bin
   ```

---

## Contact Information
For product questions or support, please contact:
* **Email**: bmc_marketing@nuvoton.com