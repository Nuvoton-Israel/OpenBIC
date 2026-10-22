<img src="./docs/images/OpenBIC.svg" align="center" height="100%" width="100%" >

# OpenBIC for Nuvoton NPCM400

OpenBIC is an open software framework for building complete BridgeIC firmware images. This repository brings OpenBIC to the **Nuvoton NPCM400** SMC, so platform teams can start from a working, open-source SMC firmware instead of writing one from scratch.

> **Note:** This repository is Nuvoton's fork of the upstream [facebook/OpenBIC](https://github.com/facebook/OpenBIC). It tracks upstream and adds Nuvoton NPCM400 SMC support under `meta-nuvoton/`. For general OpenBIC usage and the other platforms, refer to the upstream repository and its [documentation](https://facebook.github.io/OpenBIC/).

## Table of Contents
- [At a Glance](#at-a-glance)
- [Firmware Variants](#firmware-variants)
- [Repository Layout](#repository-layout)
- [Quick Start](#quick-start)
- [Documentation](#documentation)
- [Contact](#contact)
- [License](#license)

---

## At a Glance

| | |
| --- | --- |
| **SoC** | Nuvoton NPCM400 |
| **Reference board** | NPCM400 EVB (Zephyr board `npcm400f_evb`) |
| **RTOS** | Zephyr, through the CMSIS RTOS API wrapper, so features stay RTOS-agnostic |
| **Zephyr tree** | [Nuvoton-Israel/zephyr](https://github.com/Nuvoton-Israel/zephyr), fetched as `zephyr_nuvoton` by `west` |
| **Firmware variants** | `smc-npcm400` (standard SMC) and `smc-npcm400-obmf` (OBMF) |

---

## Firmware Variants

Nuvoton ships two SMC firmware projects. Each is a standalone Zephyr application under `meta-nuvoton/` that builds a complete image for the NPCM400 EVB.

| Project | What it is | Pick it when |
| --- | --- | --- |
| [`smc-npcm400`](meta-nuvoton/smc-npcm400/README.md) | **Standard OpenBIC SMC firmware.** The SMC runs the management stack itself and talks to the BMC over MCTP (USB, I3C target, I3C controller and SMBus), serving PLDM (sensors, FRU, OEM commands, firmware update) and IPMI. | You want the SMC to own sensor monitoring and management logic, and the BMC to consume it through standard PLDM/IPMI. |
| [`smc-npcm400-obmf`](meta-nuvoton/smc-npcm400-obmf/README.md) | **OBMF firmware.** The SMC acts as an OBMF-ICP device that exposes its local interfaces (GPIO/SGPIO, I2C controller and target, I3C controller, serial/UART, SPI, MMIO, I/O, IPMI) to the BMC as channels. PLDM firmware update over MCTP is kept for SMC updates. | You want the BMC to drive the SMC's hardware interfaces directly, keeping the SMC firmware thin. |

Board setup, flashing and verification steps are in each project's README.

---

## Repository Layout

| Path | Contents |
| --- | --- |
| `common/` | Upstream OpenBIC common code: HAL, services (MCTP, PLDM, IPMI, sensors, ...), device drivers, libraries and shell commands |
| `meta-nuvoton/smc-npcm400/` | Standard SMC firmware project |
| `meta-nuvoton/smc-npcm400-obmf/` | OBMF firmware project. Its `patches/` directory holds Zephyr driver changes (for example, OBMF over USB) that its `CMakeLists.txt` applies to `zephyr_nuvoton` with `git am` at configure time, skipping any patch that is already in the history |
| `meta-nuvoton/common/` | Nuvoton code shared by the projects, currently the OBMF-ICP service (`service/obmf/`) with one handler per channel type |
| `meta-facebook/` | Upstream Meta platforms |
| `west.yml` | `west` manifest that pins the `zephyr_nuvoton` revision |

---

## Quick Start

### 1. Prepare the host

The development environment is Ubuntu 18.04 LTS (64-bit) with bash. See the [Zephyr 2.6 Getting Started Guide](https://docs.zephyrproject.org/2.6.0/getting_started/index.html) for background.

Minimum versions:
```
CMake: 3.20.0
Python: 3.6
Devicetree compiler: 1.4.6
```

Install the host packages:
```bash
sudo apt install --no-install-recommends git cmake ninja-build gperf \
ccache dfu-util device-tree-compiler wget \
python3-dev python3-pip python3-setuptools python3-tk python3-wheel xzutils file \
make gcc gcc-multilib g++-multilib libsdl2-dev
```

Install `west` and make sure `~/.local/bin` is on your `PATH`:
```bash
pip3 install --user -U west
echo 'export PATH=~/.local/bin:"$PATH"' >> ~/.bashrc
source ~/.bashrc
```

Install the Zephyr SDK 0.12.4 toolchain into `~/zephyr-sdk-0.12.4`:
```bash
cd ~
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.12.4/zephyr-sdk-0.12.4-x86_64-linux-setup.run
chmod +x zephyr-sdk-0.12.4-x86_64-linux-setup.run
./zephyr-sdk-0.12.4-x86_64-linux-setup.run -- -d ~/zephyr-sdk-0.12.4
```

### 2. Fetch the source

```bash
west init -m https://github.com/Nuvoton-Israel/OpenBIC zephyrproject
cd zephyrproject
west update
```

This checks out OpenBIC as `openbic/` and the Nuvoton Zephyr tree as `zephyr_nuvoton/`.

### 3. Build

From the `zephyrproject` directory, build the variant you need:
```bash
# Standard SMC firmware
west -z zephyr_nuvoton build -p always -b npcm400f_evb openbic/meta-nuvoton/smc-npcm400/

# OBMF firmware
west -z zephyr_nuvoton build -p always -b npcm400f_evb openbic/meta-nuvoton/smc-npcm400-obmf/
```

### 4. Flash and verify

Follow the project README for J-Link or NpcmFwProg flashing and the verification steps:
- [smc-npcm400 README](meta-nuvoton/smc-npcm400/README.md)
- [smc-npcm400-obmf README](meta-nuvoton/smc-npcm400-obmf/README.md)

---

## Documentation

- Upstream OpenBIC documentation: https://facebook.github.io/OpenBIC/
- Upstream repository: https://github.com/facebook/OpenBIC
- Nuvoton Zephyr tree: https://github.com/Nuvoton-Israel/zephyr
- BMC-side OBMF setup: [OpenBMC NPCM845 EVB OBMF README](https://github.com/Nuvoton-Israel/openbmc/blob/npcm-master/meta-nuvoton-obmc/meta-evb-npcm845/README-OBMF.md)

---

## Contact

For product questions or support, please contact:
* **Email**: bmc_marketing@nuvoton.com

---

## License

OpenBIC is [Apache 2.0 licensed](LICENSE).
