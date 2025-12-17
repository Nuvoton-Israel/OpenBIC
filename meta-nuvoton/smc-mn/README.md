# Nuvoton NPCM400 Evaluation Board

## Contact

For product questions, please contact us at:
* bmc_marketing@nuvoton.com

## Table of Contents

- [Getting Started](#getting-started)
  * [Build OpenBIC Project](#build-openbic-project)
  * [Flash Programming Tools](#flash-programming-tools)
    * [J-Link](#j-link)
    * [NpcmFwProg](#npcmfwprog)
  * [Features](#features)
    * [PLDM Over MCTP Over USB](#pldm-over-mctp-over-usb)

## Getting Started

Refer to the [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) to set up your development environment.

### Build OpenBIC Project

Run the following command to build the project:

```bash
west -z zephyr_nuvoton build -p always -b npcm400f_evb openbic/meta-nuvoton/smc-mn/
```

### Flash Programming Tools

#### J-Link
The NPCM400F supports J-Link flash programming.

Please copy the [NUVOTON folder][def] to the appropriate directory for your operating system:

| OS | Location |
|:---|:---|
| Windows | `C:\Users\<USER>\AppData\Roaming\SEGGER\JLinkDevices\` |
| Linux | `$HOME/.config/SEGGER/JLinkDevices/` |
| macOS | `$HOME/Library/Application Support/SEGGER/JLinkDevices/` |

**Flash image to your board:**

```bash
cp build/zephyr/SMCNPCM_signed.bin build/zephyr/zephyr_signed.bin
west flash
```

#### NpcmFwProg

`NpcmFwProg` is a tool to program the SMC firmware under Windows/Linux host over a TTY serial connection.
Please follow up the [npcmFwProg](https://github.com/Nuvoton-Israel/npcmFwProg) for more details.


[def]: https://github.com/Nuvoton-Israel/zephyr/tree/openbic-v2.6/boards/arm/npcm400f_evb/NUVOTON


### Features

#### PLDM Over MCTP Over USB

To verify MCTP over USB functionality:

1.  Connect the SMC USB device to the BMC USB HOST via a USB cable.
    *  Ensure the BMC is booted with the latest OpenBMC image: [https://github.com/Nuvoton-Israel/openbmc](https://github.com/Nuvoton-Israel/openbmc/commit/4f6ca14b691fd7d7dbe5d6305fcd8787f240899d)
    *  Ensure the SMC is connected before BMC is booted, the udev rule in lastest OpenBMC is still under improvement.

2.  From the BMC console, verify the MCTP USB link is up:
    ```bash
    mctp link
    ```
    Expected output:
    ```
    dev mctpusb0 index 9 address none net 1 mtu 68 up
    ```
3.  From the BMC console, verify the MCTP EID is set up:
    ```bash
    mctp route
    ```
    Expected output:
    ```
    eid min 10 max 10 net 1 dev mctpusb0 mtu 68
    ```
4.  From the BMC console, use `pldmtool` to send PLDM messages to the SMC:

    *   **Get PLDM Types:**
        ```bash
        pldmtool base GetPLDMTypes -m 10
        ```
        Expected output:
        ```json
        {
            "CompletionCode": "SUCCESS",
            "PLDMTypes": [
                {
                    "PLDM Type": "base",
                    "PLDM Type Code": 0
                },
                {
                    "PLDM Type": "platform",
                    "PLDM Type Code": 2
                },
                {
                    "PLDM Type": "firmware update",
                    "PLDM Type Code": 5
                }
            ]
        }
        ```
    *   **Get Firmware Parameters:**
        ```bash
        pldmtool fw_update GetFwParams -m 10
        ```
        Expected output:
        ```json
        {
            "CapabilitiesDuringUpdate": {
                "Component Update Failure Recovery Capability": "Device will revert to previous component image upon failure, timeout or cancellation of the transfer.",
                "Component Update Failure Retry Capability": " Device can have component updated again without exiting update mode and restarting transfer via RequestUpdate command.",
                "Firmware Device Host Functionality during Firmware Update": "Device will revert to previous component image upon failure, timeout or cancellation of the transfer",
                "Firmware Device Partial Updates": "Firmware Device cannot accept a partial update and all components present on the FD shall be updated.",
                "Firmware Device Update Mode Restrictions": "No host OS environment restriction for update mode"
            },
            "ComponentCount": 0,
            "ActiveComponentImageSetVersionString": "2025.22.02",
            "PendingComponentImageSetVersionString": "",
            "ComponentParameterEntries": null
        }
        ```
  5.  If the PLDM sensor is enabled in SMC (see [plat_def.h](https://github.com/Nuvoton-Israel/OpenBIC/blob/npcm_main_rebase/meta-nuvoton/smc-mn/src/platform/plat_def.h#L26)), use the following command to retrieve SMC's ADC sensor data:
        ```bash
        ipmitool sdr
        ```

        Expected output:
        ```json
            root@evb-npcm845-stage:~# ipmitool sdr
            ........
            NPCM_AVSB        | 3.28 Volts        | ok
            NPCM_VCC         | 3.29 Volts        | ok
            NPCM_VHIF        | 1.93 Volts        | cr
            NPCM_VSB         | 3.28 Volts        | ok
        ```