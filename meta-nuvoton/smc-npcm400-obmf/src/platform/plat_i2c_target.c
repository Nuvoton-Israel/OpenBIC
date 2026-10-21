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

/*
 * I2C target configuration and OBMF I2C Target channel pass-through.
 *
 * The SMC is an I2C target on I2C_BUS1/I2C_BUS2. Every write it receives
 * there (e.g. an MCTP over SMBus response from a device the host talks to
 * through the OBMF I2C Controller channel) is passed through unchanged to
 * the host on the matching OBMF I2C Target channel. The SMC does not parse
 * the payload.
 */

#include <zephyr.h>
#include <logging/log.h>
#include "plat_i2c.h"
#include "plat_i2c_target.h"
#include "obmf.h"

LOG_MODULE_REGISTER(plat_i2c_target);

#define I2C_TARGET_PASSTHROUGH_STACK_SIZE 1024

/* I2C target init-enable table */
const bool I2C_TARGET_ENABLE_TABLE[MAX_TARGET_NUM] = {
	TARGET_ENABLE, TARGET_ENABLE, TARGET_DISABLE, TARGET_DISABLE,
	TARGET_DISABLE, TARGET_DISABLE, TARGET_DISABLE, TARGET_DISABLE,
	TARGET_DISABLE, TARGET_DISABLE, TARGET_DISABLE, TARGET_DISABLE,
	TARGET_DISABLE, TARGET_DISABLE, TARGET_DISABLE, TARGET_DISABLE,
};

/* I2C target init-config table, 8-bit address */
const struct _i2c_target_config I2C_TARGET_CONFIG_TABLE[MAX_TARGET_NUM] = {
	{ 0x20, 0xA }, { 0x20, 0xA }, { 0xFF, 0xA }, { 0xFF, 0xA },
	{ 0xFF, 0xA }, { 0xFF, 0xA }, { 0xFF, 0xA }, { 0xFF, 0xA },
	{ 0xFF, 0xA }, { 0xFF, 0xA }, { 0xFF, 0xA }, { 0xFF, 0xA },
	{ 0xFF, 0xA }, { 0xFF, 0xA }, { 0xFF, 0xA }, { 0xFF, 0xA },
};

struct i2c_target_passthrough {
	uint8_t bus;
	uint8_t channel;
	uint8_t buf[MAX_I2C_TARGET_BUFF];
	struct k_thread thread;
};

static struct i2c_target_passthrough passthroughs[] = {
	{ .bus = I2C_BUS1, .channel = OBMF_ICP_I2C_TARGET_CHANNEL_0 },
	{ .bus = I2C_BUS2, .channel = OBMF_ICP_I2C_TARGET_CHANNEL_1 },
};

K_THREAD_STACK_ARRAY_DEFINE(passthrough_stacks, ARRAY_SIZE(passthroughs),
			    I2C_TARGET_PASSTHROUGH_STACK_SIZE);

static void i2c_target_passthrough_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct i2c_target_passthrough *pt = arg1;
	uint8_t address = I2C_TARGET_CONFIG_TABLE[pt->bus].address >> 1;
	uint16_t len;
	int rc;

	while (1) {
		if (i2c_target_read(pt->bus, pt->buf, sizeof(pt->buf), &len) !=
		    I2C_TARGET_API_NO_ERR) {
			k_msleep(100);
			continue;
		}

		LOG_HEXDUMP_DBG(pt->buf, len, "i2c target rx");

		rc = obmf_i2c_target_write(pt->channel, address, pt->buf, len);
		if (rc != 0 && rc != -ENETDOWN) {
			LOG_ERR("bus %d: pass through to OBMF channel %d failed: %d", pt->bus,
				pt->channel, rc);
		}
	}
}

void plat_i2c_target_passthrough_init(void)
{
	for (int i = 0; i < ARRAY_SIZE(passthroughs); i++) {
		struct i2c_target_passthrough *pt = &passthroughs[i];

		if (!I2C_TARGET_ENABLE_TABLE[pt->bus]) {
			continue;
		}

		obmf_i2c_target_set_address(pt->channel,
					    I2C_TARGET_CONFIG_TABLE[pt->bus].address >> 1);

		k_thread_create(&pt->thread, passthrough_stacks[i],
				K_THREAD_STACK_SIZEOF(passthrough_stacks[i]),
				i2c_target_passthrough_thread, pt, NULL, NULL,
				CONFIG_MAIN_THREAD_PRIORITY, 0, K_NO_WAIT);
		k_thread_name_set(&pt->thread, "i2c_tgt_obmf");
	}
}
