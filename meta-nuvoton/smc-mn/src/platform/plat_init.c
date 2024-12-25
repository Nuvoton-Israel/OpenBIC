/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hal_gpio.h"
#include "plat_gpio.h"

#include "plat_class.h"
#include "plat_isr.h"
#include "plat_vw_gpio.h"
#include "plat_i2c_target.h"
#include "ipmi.h"
#include "pldm.h"
#include "rg3mxxb12.h"
#include "plat_mctp.h"
#include "plat_i3c.h"
#include "plat_kcs.h"
#include "snoop_npcm.h"
#include <logging/log.h>
#include <drivers/uart.h>
#include <usb/usb_device.h>

extern void plat_uart_bridge_init(void);
extern void edaf_npcm_init(void);

#define LOG_LEVEL CONFIG_USB_DEVICE_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(palt_init);

void pal_pre_init()
{
	/* Init platform config */
	init_platform_config();

	/* init i2c target */
	for (int index = 0; index < MAX_TARGET_NUM; index++) {
		if (I2C_TARGET_ENABLE_TABLE[index])
			i2c_target_control(
				index, (struct _i2c_target_config *)&I2C_TARGET_CONFIG_TABLE[index],
				1);
	}

	if (!pal_load_vw_gpio_config()) {
		printk("failed to initialize vw gpio\n");
	}

#if 0
	/* Since i3c hub only support 1.0v ~ 1.8v, default use I3C_BUS_CONTROLLER_TO_HUB i3c bus */
	I3C_MSG i3c_msg = { 0 };
	i3c_msg.bus = I3C_BUS_CONTROLLER_TO_HUB;
	i3c_msg.target_addr = I3C_STATIC_ADDR_HUB;

	const int rstdaa_count = 2;
	int ret = 0;

	for (int i = 0; i < rstdaa_count; i++) {
		ret = i3c_brocast_ccc(&i3c_msg, I3C_CCC_RSTDAA, I3C_BROADCAST_ADDR);
		if (ret != 0) {
			printf("Error to reset daa. count = %d\n", i);
		}
	}

	ret = i3c_brocast_ccc(&i3c_msg, I3C_CCC_SETAASA, I3C_BROADCAST_ADDR);
	if (ret != 0) {
		printf("Error to set daa\n");
	}

	i3c_attach(&i3c_msg);

	// Initialize I3C HUB
	if (!rg3mxxb12_i3c_mode_only_init(&i3c_msg, LDO_VOLT)) {
		printk("failed to initialize 1ou rg3mxxb12\n");
	}
#endif
}

void pal_post_init()
{
	plat_mctp_init();
	snoop_init();
	kcs_init();
	plat_isr_init();
	edaf_npcm_init();
	plat_uart_bridge_init();
}

void pal_device_init()
{
}

void pal_set_sys_status()
{
}


#define DEF_PROJ_GPIO_PRIORITY 61

DEVICE_DEFINE(PRE_DEF_PROJ_GPIO, "PRE_DEF_PROJ_GPIO_NAME", &gpio_init, NULL, NULL, NULL,
	      POST_KERNEL, DEF_PROJ_GPIO_PRIORITY, NULL);
