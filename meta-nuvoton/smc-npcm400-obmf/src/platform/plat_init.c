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

#include <logging/log.h>
#include "hal_gpio.h"
#include "plat_gpio.h"
#include "plat_class.h"
#include "plat_i2c.h"
#include "plat_i2c_target.h"
#include "plat_kcs.h"
#include "plat_led.h"
#include "plat_mctp.h"
#include "plat_sgpio.h"
#include "plat_vw_gpio.h"
#include "snoop_npcm.h"
#include "obmf.h"

LOG_MODULE_REGISTER(plat_init);

extern void plat_uart_bridge_init(void);
extern void edaf_npcm_init(void);

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

	gpio_set(FM_SMC_FLASH_MUX_SEL, 0);
}

void pal_post_init()
{
	plat_mctp_init();
	snoop_init();
	kcs_init();
	edaf_npcm_init();
	plat_led_init();
	obmf_service_init();
	plat_i2c_target_passthrough_init();
	if (!pal_load_obmf_smc_gpio_config()) {
		LOG_ERR("failed to initialize obmf smc gpio channel");
	}
#ifdef CONFIG_SGPIO_NPCM4XX
	if (!pal_load_sgpio_config()) {
		LOG_ERR("failed to initialize sgpio");
	}
#endif
	if (!pal_load_vw_gpio_config()) {
		LOG_ERR("failed to initialize vw gpio");
	}
	obmf_i2c_set_controller_bus(OBMF_ICP_I2C_CONTROLLER_CHANNEL_0, I2C_BUS1);
	obmf_i2c_set_controller_bus(OBMF_ICP_I2C_CONTROLLER_CHANNEL_1, I2C_BUS2);
	obmf_i2c_set_controller_bus(OBMF_ICP_I2C_CONTROLLER_CHANNEL_2, I2C_BUS3);
	obmf_i2c_set_controller_bus(OBMF_ICP_I2C_CONTROLLER_CHANNEL_3, I2C_BUS5);
	obmf_i3c_set_controller_bus(I3C_BUS_CONTROLLER_TO_HUB);

	gpio_set(FM_SMC_FW_READY, 1);

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
