# Nuvoton NPCM400 Evaluation Board

<img src="npcm400_evb.png" alt="NPCM400 EVB" width="50%">

This repository provides support for the Nuvoton NPCM400 Evaluation Board (EVB) within the OpenBIC framework.

## Table of Contents
- [Getting Started](#getting-started)
  - [Prerequisites](#prerequisites)
  - [Building the Project](#building-the-project)
  - [Flash Programming](#flash-programming)
    - [Method 1: J-Link](#j-link)
    - [Method 2: NpcmFwProg](#npcmfwprog)
- [Features & Verification](#features--verification)
  - [OBMF](#obmf)
  - [OBMF Channel Mapping](#obmf-channel-mapping)
- [Contact Information](#contact-information)

---

## Getting Started

### Prerequisites
Refer to the [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) to set up your development environment.

### Power Supply Configuration
The NPCM400 EVB provides two power supply options, configurable via the **JP6** jumper:
1. **USB Power**: If selected, power must be supplied by connecting a cable to the **USB Type-B** port.
2. **DC IN 5V Power**: If selected, power must be supplied by plugging a **DC 5V Adapter** into the DC IN jack.

### Debug Console (UART)
To view the debug log, connect a UART-to-USB adapter to the UART pins on the board. Please refer to the EVB image at the top of this document for the exact location of the UART header (labeled as UART: GND, TX, RX).

### Building the Project
Run the following command to build the OpenBIC firmware for NPCM400:
```bash
west -z zephyr_nuvoton build -p always -b npcm400f_evb openbic/meta-nuvoton/smc-npcm400-obmf/
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

### OBMF
For OBMF setup and verification steps, refer to the [OpenBMC NPCM845 EVB OBMF README](https://github.com/Nuvoton-Israel/openbmc/blob/npcm-master/meta-nuvoton-obmc/meta-evb-npcm845/README-OBMF.md).

### OBMF Channel Mapping
The table below lists every OBMF-ICP channel this firmware exposes and the NPCM400 hardware behind it. Channel numbers are defined in [`meta-nuvoton/common/service/obmf/obmf.h`](../common/service/obmf/obmf.h); the channel table and bus bindings are defined in [`src/platform/plat_obmf.c`](src/platform/plat_obmf.c) and [`boards/npcm400f_evb.overlay`](boards/npcm400f_evb.overlay).

**Direction:** *BMC → SMC* means the BMC sends requests and the SMC executes them on local hardware. *SMC → BMC* means the SMC sends requests to the BMC.

| Ch | Type | Macro | NPCM400 hardware | Direction | Notes |
| --- | --- | --- | --- | --- | --- |
| 0 | Config | `OBMF_ICP_CONTROL_CHANNEL` | - | - | Channel discovery and configuration |
| 1 | MMIO | `OBMF_ICP_MMIO_CHANNEL` | SMC local register state | Both | Also carries a POST code region |
| 2 | Serial | `OBMF_ICP_SERIAL_CHANNEL_0` | `uart1` (eSPI UART B, `ext-loopback`) | Both | Bridged by `plat_uart_bridge.c` via the `openbic,obmf-serial-0` chosen node |
| 3 | IPMI | `OBMF_ICP_IPMI_CHANNEL_NUM` | Host KCS (`KCS1`) | SMC → BMC | Host IPMI requests received on KCS are forwarded to the BMC |
| 4 | Memory Device | `OBMF_ICP_MEM_DEV_CHANNEL` | Storage device exposed by the BMC (eSPI flash access path, EDAF) | SMC → BMC | Byte-addressable read/write/erase; 0x4000000 address range, LRU page cache |
| 5 | GPIO | `OBMF_ICP_SMC_GPIO_CHANNEL` | SMC GPIOs | Both | Every GPIO enabled (`is_init`) in `plat_gpio.c`, indexed in table order |
| 6 | GPIO | `OBMF_ICP_SGPIO0_CHANNEL` | SGPIO group 0 | Both | Requires `CONFIG_SGPIO_NPCM4XX` |
| 7 | GPIO | `OBMF_ICP_SGPIO1_CHANNEL` | SGPIO group 1 | Both | Requires `CONFIG_SGPIO_NPCM4XX` |
| 8 | GPIO | `OBMF_ICP_VW_GPIO_CHANNEL` | eSPI Virtual Wire GPIOs | Both | `plat_vw_gpio.c` |
| 9 | I/O | `OBMF_ICP_IO_CHANNEL` | I/O ports 0x80-0x83 | BMC → SMC | POST code capture |
| 10 | I2C Controller | `OBMF_ICP_I2C_CONTROLLER_CHANNEL_0` | `I2C_BUS1` (`i2c1a`) | BMC → SMC | |
| 11 | I2C Controller | `OBMF_ICP_I2C_CONTROLLER_CHANNEL_1` | `I2C_BUS2` (`i2c2a`) | BMC → SMC | |
| 12 | I2C Controller | `OBMF_ICP_I2C_CONTROLLER_CHANNEL_2` | `I2C_BUS3` (`i2c3a`) | BMC → SMC | |
| 13 | I2C Controller | `OBMF_ICP_I2C_CONTROLLER_CHANNEL_3` | `I2C_BUS5` (`i2c5a`) | BMC → SMC | |
| 14 | I2C Target | `OBMF_ICP_I2C_TARGET_CHANNEL_0` | `I2C_BUS1` (`i2c1a`), target address 0x24 (8-bit) | SMC → BMC | Writes from external controllers are passed through to the BMC |
| 15 | I2C Target | `OBMF_ICP_I2C_TARGET_CHANNEL_1` | `I2C_BUS2` (`i2c2a`), target address 0x28 (8-bit) | SMC → BMC | Same as channel 14 |
| 16 | Serial | `OBMF_ICP_SERIAL_CHANNEL_1` | `uart2` | Both | Bridged via the `openbic,obmf-serial-1` chosen node |
| 17 | SPI Controller | `OBMF_ICP_SPI_CHANNEL` | SPI flash on FIU0 CS1 (`spi_fiu0_cs1`) | BMC → SMC | JEDEC ID, read, status, page program, 4K/32K/64K erase |
| 18 | I3C Controller | `OBMF_ICP_I3C_CONTROLLER_CHANNEL` | `i3c4` (datasheet I3C5, balls A9/B9) | BMC → SMC | Hot-Join enabled |
| 19 | I3C Controller | `OBMF_ICP_I3C_CONTROLLER_CHANNEL_1` | `i3c5` (datasheet I3C6, balls A5/B5) | BMC → SMC | Hot-Join enabled |

#### Defining OBMF channels
Each exposed channel is one entry in `plat_obmf_channels[]` in [`src/platform/plat_obmf.c`](src/platform/plat_obmf.c). The OBMF core builds the discovery structure from this table, so no core code needs to change:

```c
static const struct obmf_plat_channel plat_obmf_channels[] = {
	OBMF_CHANNEL_SERIAL(OBMF_ICP_SERIAL_CHANNEL_0, "Serial", 115200),
	OBMF_CHANNEL_I2C(OBMF_ICP_I2C_CONTROLLER_CHANNEL_0, "I2C0", I2C_BUS1, 100000),
	OBMF_CHANNEL_I3C(OBMF_ICP_I3C_CONTROLLER_CHANNEL, "I3C0", I3C_BUS_CONTROLLER_TO_HUB),
	/* ... */
};
```

- To add a channel, add an `OBMF_CHANNEL_*()` entry. The helper macros are in [`obmf.h`](../common/service/obmf/obmf.h).
- To bind a controller channel to a different bus, change the `bus` argument.
- To hide a channel, remove its entry. The BMC then sees offset 0 for that channel and gets `UNKNOWN_CHANNEL` for any request to it.

> **Note:** The SMC's own firmware flash (`spi_spim0_cs0`) is not exposed through OBMF. It is updated through PLDM firmware update over MCTP.

---

## Contact Information
For product questions or support, please contact:
* **Email**: bmc_marketing@nuvoton.com