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

#include "plat_sensor_table.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "sensor.h"
//#include "ast_adc.h"
#include "npcm4xx_adc.h"
#include "intel_peci.h"
#include "hal_gpio.h"
#include "plat_class.h"
#include "plat_gpio.h"
#include "plat_hook.h"
#include "plat_i2c.h"
//#include "plat_i3c.h"
//#include "plat_dimm.h"
#include "power_status.h"
#include "pmbus.h"
#include "tmp431.h"
#include "libutil.h"
#include "xdpe15284.h"

#include <logging/log.h>

LOG_MODULE_REGISTER(plat_sensor_table);

//SET_GPIO_VALUE_CFG pre_bat_3v = { A_P3V_BAT_SCALED_EN_R, GPIO_HIGH };
//SET_GPIO_VALUE_CFG post_bat_3v = { A_P3V_BAT_SCALED_EN_R, GPIO_LOW };

sensor_poll_time_cfg diff_poll_time_sensor_table[] = {
	// sensor_number, last_access_time
};

dimm_pmic_mapping_cfg dimm_pmic_map_table[] = {
	// dimm_sensor_num, mapping_pmic_sensor_num
	{ SENSOR_NUM_TEMP_DIMM_A0, SENSOR_NUM_PWR_DIMMA0_PMIC },
	{ SENSOR_NUM_TEMP_DIMM_A2, SENSOR_NUM_PWR_DIMMA2_PMIC },
	{ SENSOR_NUM_TEMP_DIMM_A3, SENSOR_NUM_PWR_DIMMA3_PMIC },
	{ SENSOR_NUM_TEMP_DIMM_A4, SENSOR_NUM_PWR_DIMMA4_PMIC },
	{ SENSOR_NUM_TEMP_DIMM_A6, SENSOR_NUM_PWR_DIMMA6_PMIC },
	{ SENSOR_NUM_TEMP_DIMM_A7, SENSOR_NUM_PWR_DIMMA7_PMIC },
};

sensor_cfg plat_sensor_config[] = {
	//check openbic/meta-facebook/yv35-gl/src/platform/plat_sensor_table.c
	/* number,                  type,       port,      address,      offset,
	   access check arg0, arg1, sample_count, cache, cache_status, mux_ADDRess, mux_offset,
	   pre_sensor_read_fn, pre_sensor_read_args, post_sensor_read_fn, post_sensor_read_fn  */
#if 0
	// PECI
	{ SENSOR_NUM_TEMP_CPU, sensor_dev_intel_peci, NONE, CPU_PECI_ADDR, PECI_TEMP_CPU,
	  post_access, 0, 0, SAMPLE_COUNT_DEFAULT, POLL_TIME_DEFAULT, ENABLE_SENSOR_POLLING, 0,
	  SENSOR_INIT_STATUS, NULL, NULL, NULL, NULL, NULL },
	{ SENSOR_NUM_TEMP_CPU_MARGIN, sensor_dev_intel_peci, NONE, CPU_PECI_ADDR,
	  PECI_TEMP_CPU_MARGIN, post_access, 0, 0, SAMPLE_COUNT_DEFAULT, POLL_TIME_DEFAULT,
	  ENABLE_SENSOR_POLLING, 0, SENSOR_INIT_STATUS, NULL, NULL, post_cpu_margin_read, NULL,
	  NULL },
	{ SENSOR_NUM_TEMP_CPU_TJMAX, sensor_dev_intel_peci, NONE, CPU_PECI_ADDR,
	  PECI_TEMP_CPU_TJMAX, post_access, 0, 0, SAMPLE_COUNT_DEFAULT, POLL_TIME_DEFAULT,
	  ENABLE_SENSOR_POLLING, 0, SENSOR_INIT_STATUS, NULL, NULL, NULL, NULL, NULL },
	{ SENSOR_NUM_PWR_CPU, sensor_dev_intel_peci, NONE, CPU_PECI_ADDR, PECI_PWR_CPU, post_access,
	  0, 0, SAMPLE_COUNT_DEFAULT, POLL_TIME_DEFAULT, ENABLE_SENSOR_POLLING, 0,
	  SENSOR_INIT_STATUS, NULL, NULL, NULL, NULL, NULL },
#endif
	// adc voltage
	{ SENSOR_NUM_AVSB, sensor_dev_npcm4xx_adc, ADC_PORT0, NONE, NONE, stby_access, 0, 0,
	  SAMPLE_COUNT_DEFAULT, POLL_TIME_DEFAULT, ENABLE_SENSOR_POLLING, 0, SENSOR_INIT_STATUS,
	  NULL, NULL, NULL, NULL, &adc_npcm4xx_init_args[0] },
	{ SENSOR_NUM_VSB, sensor_dev_npcm4xx_adc, ADC_PORT1, NONE, NONE, stby_access, 0, 0,
	  SAMPLE_COUNT_DEFAULT, POLL_TIME_DEFAULT, ENABLE_SENSOR_POLLING, 0, SENSOR_INIT_STATUS,
	  NULL, NULL, NULL, NULL, &adc_npcm4xx_init_args[0] },
	{ SENSOR_NUM_VCC, sensor_dev_npcm4xx_adc, ADC_PORT2, NONE, NONE, stby_access, 0, 0,
	  SAMPLE_COUNT_DEFAULT, POLL_TIME_DEFAULT, ENABLE_SENSOR_POLLING, 0, SENSOR_INIT_STATUS,
	  NULL, NULL, NULL, NULL, &adc_npcm4xx_init_args[0] },
	{ SENSOR_NUM_VHIF, sensor_dev_npcm4xx_adc, ADC_PORT3, NONE, NONE, stby_access, 0, 0,
	  SAMPLE_COUNT_DEFAULT, POLL_TIME_DEFAULT, ENABLE_SENSOR_POLLING, 0, SENSOR_INIT_STATUS,
	  NULL, NULL, NULL, NULL, &adc_npcm4xx_init_args[0] },
#if 0
	// adc thermistor
	{ SENSOR_NUM_THR2, sensor_dev_npcm4xx_adc, ADC_PORT15, NONE, NONE,
	  stby_access, 0, 0, SAMPLE_COUNT_DEFAULT, POLL_TIME_DEFAULT, ENABLE_SENSOR_POLLING, 0,
	  SENSOR_INIT_STATUS, NULL, NULL, NULL, NULL, &adc_npcm4xx_init_args[0] },	
	// adc thermal diode
	{ SENSOR_NUM_TD2P, sensor_dev_npcm4xx_adc, ADC_PORT19, NONE, NONE,
	  stby_access, 0, 0, SAMPLE_COUNT_DEFAULT, POLL_TIME_DEFAULT, ENABLE_SENSOR_POLLING, 0,
	  SENSOR_INIT_STATUS, NULL, NULL, NULL, NULL, &adc_npcm4xx_init_args[0] },
	{ SENSOR_NUM_TD1P, sensor_dev_npcm4xx_adc, ADC_PORT20, NONE, NONE,
	  stby_access, 0, 0, SAMPLE_COUNT_DEFAULT, POLL_TIME_DEFAULT, ENABLE_SENSOR_POLLING, 0,
	  SENSOR_INIT_STATUS, NULL, NULL, NULL, NULL, &adc_npcm4xx_init_args[0] },
#endif
};

const int SENSOR_CONFIG_SIZE = ARRAY_SIZE(plat_sensor_config);

void load_sensor_config(void)
{
	memcpy(sensor_config, plat_sensor_config, sizeof(plat_sensor_config));
	sensor_config_count = ARRAY_SIZE(plat_sensor_config);

	// Fix config table in different system/config
	pal_extend_sensor_config();
}

uint8_t pal_get_extend_sensor_config()
{
	return 0;
#if 0
	uint8_t extend_sensor_config_size = 0;
	uint8_t hsc_module = get_hsc_module();
	switch (hsc_module) {
	case HSC_MODULE_ADM1278:
		extend_sensor_config_size += ARRAY_SIZE(adm1278_sensor_config_table);
		break;
	case HSC_MODULE_MP5990:
		extend_sensor_config_size += ARRAY_SIZE(mp5990_sensor_config_table);
		break;
	case HSC_MODULE_LTC4286:
		extend_sensor_config_size += ARRAY_SIZE(ltc4286_sensor_config_table);
		break;
	case HSC_MODULE_LTC4282:
		extend_sensor_config_size += ARRAY_SIZE(ltc4282_sensor_config_table);
		break;
	default:
		LOG_ERR("Unsupported HSC module, HSC module: 0x%x", hsc_module);
		break;
	}

	// Fix sensor config table if 2ou card is present
	CARD_STATUS _2ou_status = get_2ou_status();
	if (_2ou_status.present) {
		// Add DPV2 config if DPV2_16 is present
		if ((_2ou_status.card_type & TYPE_2OU_DPV2_16) == TYPE_2OU_DPV2_16) {
			extend_sensor_config_size += ARRAY_SIZE(DPV2_sensor_config_table);
		}
	}
	return extend_sensor_config_size;
#endif
}

int set_vr_page(uint8_t bus, uint8_t addr, uint8_t page)
{
	I2C_MSG msg;
	uint8_t retry = 5;

	msg.bus = bus;
	msg.target_addr = addr;
	msg.tx_len = 2;
	msg.data[0] = 0x00;
	msg.data[1] = page;
	if (i2c_master_write(&msg, retry)) {
		LOG_ERR("pre_isl69259_read, set page fail");
		return -1;
	}
	return 0;
}
#if 0
void check_vr_type(uint8_t index)
{
	uint8_t retry = 5;
	I2C_MSG msg;

	isl69259_pre_proc_arg *args = sensor_config[index].pre_sensor_read_args;
	memset(&msg, 0, sizeof(msg));
	msg.bus = sensor_config[index].port;
	msg.target_addr = sensor_config[index].target_addr;
	msg.tx_len = 2;
	msg.data[0] = 0x00;
	msg.data[1] = args->vr_page;
	if (i2c_master_write(&msg, retry)) {
		LOG_ERR("Failed to switch to VR page %d", args->vr_page);
		return;
	}

	/* Get IC Device ID from VR chip
	 * - Command code: 0xAD
	 * - The response data
	 *   byte-1: Block read count
	 *   byte-2: Device ID
	 * For the ISL69259 chip,
	 * the byte-1 of response data is 4 and the byte-2 to 5 is 49D28100h.
	 * For the TPS53689 chip,
	 * the byte-1 of response data is 6 and the byte-2 to 7 is 544953689000h.
	 * For the XDPE15284 chip,
	 * the byte-1 is returned as 2 and the byte-2 is 8Ah(XDPE15284).
	 */
	memset(&msg, 0, sizeof(msg));
	msg.bus = sensor_config[index].port;
	msg.target_addr = sensor_config[index].target_addr;
	msg.tx_len = 1;
	msg.rx_len = 7;
	msg.data[0] = PMBUS_IC_DEVICE_ID;

	if (i2c_master_read(&msg, retry)) {
		LOG_ERR("Failed to read VR IC_DEVICE_ID: register(0x%x)", PMBUS_IC_DEVICE_ID);
		return;
	}

	if ((msg.data[0] == 0x06) && (msg.data[1] == 0x54) && (msg.data[2] == 0x49) &&
	    (msg.data[3] == 0x53) && (msg.data[4] == 0x68) && (msg.data[5] == 0x90) &&
	    (msg.data[6] == 0x00)) {
		sensor_config[index].type = sensor_dev_tps53689;
	} else if ((msg.data[0] == 0x02) && (msg.data[2] == 0x8A)) {
		sensor_config[index].type = sensor_dev_xdpe15284;
		if (sensor_config[index].offset == VR_VOL_CMD) {
			set_vr_page(sensor_config[index].port, sensor_config[index].target_addr, 0);
			xdpe15284_lock_reg(sensor_config[index].port,
					   sensor_config[index].target_addr);
			set_vr_page(sensor_config[index].port, sensor_config[index].target_addr, 1);
			xdpe15284_lock_reg(sensor_config[index].port,
					   sensor_config[index].target_addr);
		}
	} else if ((msg.data[0] == 0x04) && (msg.data[1] == 0x00) && (msg.data[2] == 0x81) &&
		   (msg.data[3] == 0xD2) && (msg.data[4] == 0x49)) {
	} else {
		LOG_ERR("Unknown VR type");
	}
}
#endif
void check_outlet_temp_type(uint8_t index)
{
	if (index >= sensor_config_count) {
		LOG_ERR("Out of sensor_config_count");
		return;
	}

	uint8_t retry = 5;
	I2C_MSG msg;
	uint8_t CID = 0;
	uint8_t VID = 0;

	/* Get Chip ID and Manufacturer ID
	 * - Command code: 0xFD, 0xFE
	 * TMP431: 0x31 0x55
	 * NCT7718W: 0x50 0x50
	 * G788P81U: 0x50 0x47
	 */
	memset(&msg, 0, sizeof(msg));
	msg.bus = sensor_config[index].port;
	msg.target_addr = sensor_config[index].target_addr;
	msg.tx_len = 1;
	msg.rx_len = 1;
	msg.data[0] = NCT7718W_CHIP_ID_OFFSET;

	if (i2c_master_read(&msg, retry)) {
		LOG_ERR("Failed to read Outlet_Temp chip ID: register(0x%x)",
			NCT7718W_CHIP_ID_OFFSET);
		return;
	}
	CID = msg.data[0];

	memset(&msg, 0, sizeof(msg));
	msg.bus = sensor_config[index].port;
	msg.target_addr = sensor_config[index].target_addr;
	msg.tx_len = 1;
	msg.rx_len = 1;
	msg.data[0] = NCT7718W_VENDOR_ID_OFFSET;

	if (i2c_master_read(&msg, retry)) {
		LOG_ERR("Failed to read Outlet_Temp vendor ID: register(0x%x)",
			NCT7718W_VENDOR_ID_OFFSET);
		return;
	}
	VID = msg.data[0];

	if ((CID == 0x31) && (VID == 0x55)) {
		sensor_config[index].type = sensor_dev_tmp431;
	} else if ((CID == 0x50) && (VID == 0x50)) {
		sensor_config[index].type = sensor_dev_nct7718w;
	} else if ((CID == 0x50) && (VID == 0x47)) {
		sensor_config[index].type = sensor_dev_g788p81u;
	} else {
		LOG_ERR("Unknown Outlet_Temp type");
	}
}

void pal_extend_sensor_config()
{
	//uint8_t sensor_count = 0;
	//uint8_t hsc_module = get_hsc_module();
	//uint8_t board_revision = get_board_revision();
#if 0
	/* Check the VR sensor type */
	sensor_count = ARRAY_SIZE(plat_sensor_config);
	for (uint8_t index = 0; index < sensor_count; index++) {
		if (sensor_config[index].type == sensor_dev_isl69259) {
			check_vr_type(index);
		}
	}
#endif
	/* Follow the hardware design,
	 * the GPIOA7(HSC_SET_EN_R) should be set to "H"
	 * and the 2OU configuration is set if the 2OU is present.
	 */
#if 0
	CARD_STATUS _2ou_status = get_2ou_status();

	int arg_index = (_2ou_status.present) ? 1 : 0;
	int gpio_state = (_2ou_status.present) ? GPIO_HIGH : GPIO_LOW;
	gpio_set(HSC_SET_EN_R, gpio_state);

	switch (hsc_module) {
	case HSC_MODULE_ADM1278:
		sensor_count = ARRAY_SIZE(adm1278_sensor_config_table);
		for (int index = 0; index < sensor_count; index++) {
			add_sensor_config(adm1278_sensor_config_table[index]);
		}
		break;
	case HSC_MODULE_MP5990:
		sensor_count = ARRAY_SIZE(mp5990_sensor_config_table);
		for (int index = 0; index < sensor_count; index++) {
			mp5990_sensor_config_table[index].init_args = &mp5990_init_args[arg_index];
			add_sensor_config(mp5990_sensor_config_table[index]);
		}
		break;
	case HSC_MODULE_LTC4286:
		sensor_count = ARRAY_SIZE(ltc4286_sensor_config_table);
		for (int index = 0; index < sensor_count; index++) {
			ltc4286_sensor_config_table[index].init_args =
				&ltc4286_init_args[arg_index];
			add_sensor_config(ltc4286_sensor_config_table[index]);
		}
		break;
	case HSC_MODULE_LTC4282:
		sensor_count = ARRAY_SIZE(ltc4282_sensor_config_table);
		for (int index = 0; index < sensor_count; index++) {
			ltc4282_sensor_config_table[index].init_args =
				&ltc4282_init_args[arg_index];
			add_sensor_config(ltc4282_sensor_config_table[index]);
		}
		break;
	default:
		LOG_ERR("Unsupported HSC module, HSC module: 0x%x", hsc_module);
		break;
	}

	switch (board_revision) {
	case SYS_BOARD_EVT3_HOTSWAP:
	case SYS_BOARD_DVT_HOTSWAP:
	case SYS_BOARD_PVT_HOTSWAP:
	case SYS_BOARD_MP_HOTSWAP:
		/* Replace the temperature sensors configuration including "HSC Temp" and "MB Outlet Temp."
		 * For these two sensors, the reading values are read from TMP431 chip.data.num
		 */
		sensor_count = ARRAY_SIZE(evt3_class1_adi_temperature_sensor_table);
		for (int index = 0; index < sensor_count; index++) {
			add_sensor_config(evt3_class1_adi_temperature_sensor_table[index]);
		}

		/* Check outlet temperature sensor type */
		for (uint8_t index = 0; index < sensor_config_count; index++) {
			if (sensor_config[index].type == sensor_dev_tmp431) {
				check_outlet_temp_type(index);
			}
		}
		break;
	default:
		break;
	}

	/* Fix sensor table if 2ou card is present */
	if (_2ou_status.present) {
		// Add DPV2 sensor config if DPV2_16 is present
		if ((_2ou_status.card_type & TYPE_2OU_DPV2_16) == TYPE_2OU_DPV2_16) {
			sensor_count = ARRAY_SIZE(DPV2_sensor_config_table);
			for (int index = 0; index < sensor_count; index++) {
				add_sensor_config(DPV2_sensor_config_table[index]);
			}
		}
	}
#endif
	if (sensor_config_count != sdr_count) {
		LOG_ERR("Extend sensor SDR and config table not match, sdr size: 0x%x, sensor config size: 0x%x",
			sdr_count, sensor_config_count);
	}
}

bool pal_is_time_to_poll(uint8_t sensor_num, int poll_time)
{
	int i = 0;
	int table_size = sizeof(diff_poll_time_sensor_table) / sizeof(sensor_poll_time_cfg);

	for (i = 0; i < table_size; i++) {
		if (sensor_num == diff_poll_time_sensor_table[i].sensor_num) {
			int64_t current_access_time = k_uptime_get();
			int64_t last_access_time = diff_poll_time_sensor_table[i].last_access_time;
			int64_t diff_time = (current_access_time - last_access_time) / 1000; // sec
			if ((last_access_time != 0) && (diff_time < poll_time)) {
				return false;
			} else {
				diff_poll_time_sensor_table[i].last_access_time =
					current_access_time;
				return true;
			}
		}
	}

	LOG_ERR("Cannot find sensor 0x%x last accest time", sensor_num);
	return true;
}

uint8_t get_hsc_pwr_reading(int *reading)
{
	return get_sensor_reading(sensor_config, sensor_config_count, SENSOR_NUM_PWR_HSCIN, reading,
				  GET_FROM_CACHE);
}

bool disable_dimm_pmic_sensor(uint8_t sensor_num)
{
	uint8_t table_size = ARRAY_SIZE(dimm_pmic_map_table);

	for (uint8_t index = 0; index < table_size; ++index) {
		if (sensor_num == dimm_pmic_map_table[index].dimm_sensor_num) {
			control_sensor_polling(dimm_pmic_map_table[index].dimm_sensor_num,
					       DISABLE_SENSOR_POLLING, SENSOR_NOT_PRESENT);
			control_sensor_polling(dimm_pmic_map_table[index].mapping_pmic_sensor_num,
					       DISABLE_SENSOR_POLLING, SENSOR_NOT_PRESENT);
			return true;
		}
	}

	LOG_ERR("Input sensor 0x%x can't find in dimm pmic mapping table", sensor_num);
	return false;
}

static int sensor_get_idx_by_sensor_num(uint16_t sensor_num)
{
	int sensor_idx = 0;
	for (sensor_idx = 0; sensor_idx < sensor_config_count; sensor_idx++) {
		if (sensor_num == sensor_config[sensor_idx].num)
			return sensor_idx;
	}

	return -1;
}

uint8_t get_dimm_status(uint8_t dimm_index)
{
	int sensor_index =
		sensor_get_idx_by_sensor_num(dimm_pmic_map_table[dimm_index].dimm_sensor_num);
	if (sensor_index < 0) {
		return SENSOR_NOT_SUPPORT;
	}

	return sensor_config[sensor_index].cache_status;
}
