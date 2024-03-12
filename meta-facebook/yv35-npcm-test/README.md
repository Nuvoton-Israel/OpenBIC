Nuvoton NPCM400F Evaluation Board

================

# Contact

For more product questions, please contact us at:
* bmc_marketing@nuvoton.com

# Table of Contents

- [Getting Started](#getting-started)
  * [Build OpenBIC project](#building-openbic-project)
  * [Flash Programing Tool](#flash-programing-tool)

# Getting Started

The documentation's [Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) to start developing.

## Build OpenBIC project

```ruby
west build -p always -b npcm400f_evb meta-facebook/yv35-npcm-test/
```
## Flash Programing Tool

NPCM400f support J-LINK falsh programming.

Please copy [NUVOTON](https://github.com/Nuvoton-Israel/zephyr/tree/openbic-v2.6/boards/arm/npcm400f_evb/NUVOTON) folder to the following path based on your operating system.

OS	| Location |
:-------------|:--------|
Windows	| C:\Users\<USER>\AppData\Roaming\SEGGER\JLinkDevices\ |
Linux	| $HOME/.config/SEGGER/JLinkDevices/
macOS	| $HOME/Library/Application Support/SEGGER/JLinkDevices/|

Flash image to your board
```ruby
cp build/zephyr/Y35NPCM_signed.bin build/zephyr/zephyr_signed.bin
west flash
```

