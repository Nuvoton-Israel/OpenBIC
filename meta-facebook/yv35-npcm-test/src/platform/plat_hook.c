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

#include <stdio.h>
#include <string.h>
#include "libutil.h"
#include "ast_adc.h"
#include "npcm4xx_adc.h"
#include "sensor.h"
#include "hal_i2c.h"
#include "plat_sensor_table.h"
#include "plat_hook.h"
#include <logging/log.h>
#include "pmbus.h"
K_MUTEX_DEFINE(wait_pm8702_mutex);
LOG_MODULE_REGISTER(plat_hook);

/**************************************************************************************************
 * INIT ARGS
**************************************************************************************************/
adc_npcm4xx_init_arg adc_npcm4xx_init_args[] = { [0] = { .is_init = false } };

pm8702_dimm_init_arg pm8702_dimm_init_args[] = { [0] = { .is_init = false, .dimm_id = 0x41 },
						 [1] = { .is_init = false, .dimm_id = 0x42 },
						 [2] = { .is_init = false, .dimm_id = 0x43 },
						 [3] = { .is_init = false, .dimm_id = 0x44 } };

ina233_init_arg ina233_init_args[] = {
	[0] = {
	.is_init = false,
	.current_lsb = 0.001,
	.r_shunt = 0.005,
	.mfr_config_init = true,
	.mfr_config = {
		.operating_mode =0b111,
		.shunt_volt_time = 0b100,
		.bus_volt_time = 0b100,
		.aver_mode = 0b011,	//set 64 average times
		.rsvd = 0b0100,
	},
	},
	[1] = {
	.is_init = false,
	.current_lsb = 0.001,
	.r_shunt = 0.005,
	.mfr_config_init = true,
	.mfr_config = {
		.operating_mode =0b111,
		.shunt_volt_time = 0b100,
		.bus_volt_time = 0b100,
		.aver_mode = 0b011,
		.rsvd = 0b0100,
	},
	},
};

/**************************************************************************************************
 *  PRE-HOOK/POST-HOOK ARGS
 **************************************************************************************************/
/*typedef struct _isl69254iraz_t_pre_arg_ {
	uint8_t vr_page;
} isl69254iraz_t_pre_arg;*/
isl69254iraz_t_pre_arg isl69254iraz_t_pre_read_args[] = {
	[0] = { 0x0 },
	[1] = { 0x1 },
};

vr_pre_proc_arg vr_page_select[] = {
	[0] = { 0x0 },
	[1] = { 0x1 },
};

ina230_init_arg SQ5220x_init_args[] = {
	[0] = {
	.is_init = false,
	.config = {
		.MODE = 0b111,		// Measure voltage of shunt resistor and bus(default).
		.VSH_CT = 0b100,	// The Vshunt conversion time is 1.1ms(default).
		.VBUS_CT = 0b100,	// The Vbus conversion time is 1.1ms(default).
		.AVG = 0b000,		// Average number is 1(default).
	},
	.alt_cfg = {
		.LEN = 1,			// Alert Latch enabled.
		.POL = 1,			// Enable the Over-Limit Power alert function.
	},
	.r_shunt = 0.005,
	.alert_value = 16.0,	// Unit: Watt
	.i_max = 4.8
	},
	[1] = {
	.is_init = false,
	.config = {
		.MODE = 0b111,		// Measure voltage of shunt resistor and bus(default).
		.VSH_CT = 0b100,	// The Vshunt conversion time is 1.1ms(default).
		.VBUS_CT = 0b100,	// The Vbus conversion time is 1.1ms(default).
		.AVG = 0b000,		// Average number is 1(default).
	},
	.alt_cfg = {
		.LEN = 1,			// Alert Latch enabled.
		.POL = 1,			// Enable the Over-Limit Power alert function.
	},
	.r_shunt = 0.005,
	.alert_value = 16.0,	// Unit: Watt
	.i_max = 4.8
	},
};