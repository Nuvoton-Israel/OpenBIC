/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * OBMF channels exposed by this platform.
 *
 * Each line of plat_obmf_channels[] defines one OBMF-ICP channel: its
 * number, the name the BMC sees, its configuration and the NPCM400 bus
 * behind it. To add a channel, add a line with the matching
 * OBMF_CHANNEL_*() helper from obmf.h; to remove one, delete its line.
 * Channels that are not listed are not advertised to the BMC.
 */

#include <logging/log.h>
#include "obmf.h"
#include "plat_i2c.h"
#include "plat_mctp.h"
#include "plat_obmf.h"

LOG_MODULE_REGISTER(plat_obmf);

static const struct obmf_plat_channel plat_obmf_channels[] = {
	/* ch1: SMC local register state, also carries the POST code region */
	OBMF_CHANNEL_MMIO("MMIO", 64, 64),

	/* ch2/ch16: UARTs bridged by plat_uart_bridge.c */
	OBMF_CHANNEL_SERIAL(OBMF_ICP_SERIAL_CHANNEL_0, "Serial", 115200),
	OBMF_CHANNEL_SERIAL(OBMF_ICP_SERIAL_CHANNEL_1, "Serial_1", 115200),

	/* ch3: host KCS requests forwarded to the BMC */
	OBMF_CHANNEL_IPMI("IPMI"),

	/* ch4: BMC storage used for eSPI flash access (EDAF), 64 MB */
	OBMF_CHANNEL_MEM_DEV("MEM_EDAF", OBMF_MEM_DEV_CAP_WRITEABLE, 0x4000000,
			     4, 960, 256),

	/* ch5-ch8: GPIOs; the entries are filled in by plat_gpio.c,
	 * plat_sgpio.c and plat_vw_gpio.c
	 */
	OBMF_CHANNEL_GPIO(OBMF_ICP_SMC_GPIO_CHANNEL, "SMC_GPIO", 124),
	OBMF_CHANNEL_GPIO(OBMF_ICP_SGPIO0_CHANNEL, "SGPIO0", 128),
	OBMF_CHANNEL_GPIO(OBMF_ICP_SGPIO1_CHANNEL, "SGPIO1", 128),
	OBMF_CHANNEL_GPIO(OBMF_ICP_VW_GPIO_CHANNEL, "VW_GPIO", 128),

	/* ch9: POST code capture on I/O ports 0x80-0x83 */
	OBMF_CHANNEL_IO("IO_PORT",
			BIT(OBMF_ICP_IO_TRANS_SEQ_READ_8) |
			BIT(OBMF_ICP_IO_TRANS_SEQ_WRITE_8) |
			BIT(OBMF_ICP_IO_TRANS_FIXED_READ_8) |
			BIT(OBMF_ICP_IO_TRANS_FIXED_WRITE_8),
			OBMF_IO_RANGE(OBMF_IO_POST_CODE_PORT,
				      OBMF_IO_POST_CODE_PORT + OBMF_IO_POST_CODE_SIZE - 1,
				      OBMF_IO_SERVICE_POST_CODE)),

	/* ch10-ch13: I2C controllers driven by the BMC */
	OBMF_CHANNEL_I2C(OBMF_ICP_I2C_CONTROLLER_CHANNEL_0, "I2C0", I2C_BUS1,
			 OBMF_I2C_SPEED_100KHZ),
	OBMF_CHANNEL_I2C(OBMF_ICP_I2C_CONTROLLER_CHANNEL_1, "I2C1", I2C_BUS2,
			 OBMF_I2C_SPEED_100KHZ),
	OBMF_CHANNEL_I2C(OBMF_ICP_I2C_CONTROLLER_CHANNEL_2, "I2C2", I2C_BUS3,
			 OBMF_I2C_SPEED_100KHZ),
	OBMF_CHANNEL_I2C(OBMF_ICP_I2C_CONTROLLER_CHANNEL_3, "I2C3", I2C_BUS5,
			 OBMF_I2C_SPEED_100KHZ),

	/* ch14/ch15: SMC I2C target writes passed through to the BMC;
	 * addresses are set by plat_i2c_target.c
	 */
	OBMF_CHANNEL_I2C_TARGET(OBMF_ICP_I2C_TARGET_CHANNEL_0, "I2C_TGT0",
				OBMF_I2C_SPEED_100KHZ),
	OBMF_CHANNEL_I2C_TARGET(OBMF_ICP_I2C_TARGET_CHANNEL_1, "I2C_TGT1",
				OBMF_I2C_SPEED_100KHZ),

	/* ch17: SPI flash on FIU0 CS1, served to the BMC */
	OBMF_CHANNEL_SPI("SPI_SHD", OBMF_SPI_FLAG_CPHA | OBMF_SPI_FLAG_CPOL, 1,
			 50000000, 50000000),

	/* ch18/ch19: I3C controllers driven by the BMC */
	OBMF_CHANNEL_I3C(OBMF_ICP_I3C_CONTROLLER_CHANNEL, "I3C0",
			 I3C_BUS_CONTROLLER_TO_HUB),
	OBMF_CHANNEL_I3C(OBMF_ICP_I3C_CONTROLLER_CHANNEL_1, "I3C1",
			 I3C_BUS_CONTROLLER_TO_HUB_1),
};

static const struct obmf_plat_config plat_obmf_config =
	OBMF_PLAT_CONFIG("OpenBIC-OBMF", plat_obmf_channels);

void plat_obmf_init(void)
{
	int ret = obmf_service_init(&plat_obmf_config);

	if (ret) {
		LOG_ERR("OBMF service init failed: %d", ret);
	}
}
