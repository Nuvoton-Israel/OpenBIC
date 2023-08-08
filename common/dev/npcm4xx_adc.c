/*
 * Copyright (c) Nuvoton Technology Corporation.
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
#include "sensor.h"
#include "npcm4xx_adc.h"
#include "plat_gpio.h"
#include "plat_def.h"
#include <zephyr.h>
#include <drivers/adc.h>
#include <logging/log.h>

LOG_MODULE_REGISTER(dev_npcm4xx_adc);

enum adc_device_idx { adc0, ADC_NUM };

#define ADC_CHANNEL_COUNT 24
#define BUFFER_SIZE 1

#if DT_NODE_EXISTS(DT_NODELABEL(adc0))
#define DEV_ADC0
#endif

#define ADC_RESOLUTION 10
#ifndef ADC_CALIBRATION
#define ADC_CALIBRATION 0
#endif
#define ADC_GAIN ADC_GAIN_1
#define ADC_REFERENCE ADC_REF_INTERNAL
#define ADC_ACQUISITION_TIME ADC_ACQ_TIME_DEFAULT

#define ADC_AVERAGE_DELAY_MSEC 1

/* ADC temperature data is stored in a word.
 * Integer part of ADC temperature data is stored in bits 15-8.
 * Fraction part of ADC temperature data is stored in bits 7-5.
 * The base unit of the fraction part is 0.125 degree Celsius.
 */
#define TEMP_INTEGER_OFFSET 8
#define TEMP_INTEGER_MASK 0xFF00
#define TEMP_FRACTION_OFFSET 5
#define TEMP_FRACTION_MASK 0x00E0
#define TEMP_FRACTION_BASE 125

static const struct device *dev_adc[ADC_NUM];
static int16_t sample_buffer[BUFFER_SIZE];
static int is_ready[1];

static void init_adc_dev()
{
#ifdef DEV_ADC0
	dev_adc[adc0] = device_get_binding("ADC_0");
	if (!(device_is_ready(dev_adc[adc0])))
		LOG_WRN("ADC[%d] device not ready!", adc0);
	else
		is_ready[adc0] = 1;
#endif
}

static bool adc_read_mv(uint8_t sensor_num, uint32_t index, uint32_t channel, int *adc_val)
{
	CHECK_NULL_ARG_WITH_RETURN(adc_val, false);

	if (sensor_num > SENSOR_NUM_MAX) {
		LOG_DBG("Invalid sensor number");
		return false;
	}

	if (index >= ADC_NUM) {
		LOG_ERR("ADC[%d] is invalid device!", index);
		return false;
	}

	if (!is_ready[index]) {
		LOG_ERR("ADC[%d] is not ready to read!", index);
		return false;
	}

	int retval;

	static struct adc_sequence sequence;
	sequence.channels = BIT(channel);
	sequence.buffer = sample_buffer;
	sequence.buffer_size = sizeof(sample_buffer);
	sequence.resolution = ADC_RESOLUTION;

	static struct adc_channel_cfg channel_cfg;
	channel_cfg.gain = ADC_GAIN;
	channel_cfg.reference = ADC_REFERENCE;
	channel_cfg.acquisition_time = ADC_ACQUISITION_TIME;
	channel_cfg.channel_id = channel;
	channel_cfg.differential = 0;

	retval = adc_channel_setup(dev_adc[index], &channel_cfg);

	if (retval) {
		LOG_ERR("ADC[%d] with sensor[0x%x] channel set fail", index, sensor_num);
		return false;
	}

	retval = adc_read(dev_adc[index], &sequence);
	if (retval != 0) {
		LOG_ERR("ADC[%d] with sensor[0x%x] reading fail with error %d", index, sensor_num,
			retval);
		return false;
	}

	int32_t raw_value = sample_buffer[0];
	int32_t ref_mv = adc_ref_internal(dev_adc[index]);
	if (ref_mv <= 0) {
		LOG_ERR("ADC[%d] with sensor[0x%x] ref-mv get fail", index, sensor_num);
		return false;
	}

	*adc_val = raw_value;
	if (channel <= ADC_PORT18) {
		switch (channel) {
			/* Thermistor channels */
			case ADC_PORT7:
			case ADC_PORT9:
			case ADC_PORT11:
			case ADC_PORT13:
			case ADC_PORT15:
				/* Return raw values when dealing
				 * with temperature
				 */
				break;
			default:
				adc_raw_to_millivolts(ref_mv, channel_cfg.gain, sequence.resolution, adc_val);
				break;
		}
	}

	return true;
}

uint8_t npcm4xx_adc_read(sensor_cfg *cfg, int *reading)
{
	CHECK_NULL_ARG_WITH_RETURN(cfg, SENSOR_UNSPECIFIED_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(reading, SENSOR_UNSPECIFIED_ERROR);

	if (cfg->num > SENSOR_NUM_MAX) {
		LOG_DBG("Invalid sensor number");
		return SENSOR_UNSPECIFIED_ERROR;
	}

	int val = 1, i = 0, average_val = 0;

	for (i = 0; i < cfg->sample_count; i++) {
		val = 1;
		if (!adc_read_mv(cfg->num, adc0, cfg->port, &val))
			return SENSOR_FAIL_TO_ACCESS;
		average_val += val;

		// To avoid too busy
		if (cfg->sample_count > SAMPLE_COUNT_DEFAULT) {
			k_msleep(ADC_AVERAGE_DELAY_MSEC);
		}
	}

	average_val = average_val / cfg->sample_count;
	sensor_val *sval = (sensor_val *)reading;

	if (cfg->port <= ADC_PORT18) {
		switch (cfg->port) {
			/* Thermistor channels */
			case ADC_PORT7:
			case ADC_PORT9:
			case ADC_PORT11:
			case ADC_PORT13:
			case ADC_PORT15:
				sval->integer = ((average_val) & TEMP_INTEGER_MASK) >> TEMP_INTEGER_OFFSET;
				sval->fraction = (((average_val) & TEMP_FRACTION_MASK) >> TEMP_FRACTION_OFFSET) * TEMP_FRACTION_BASE;
			       break;
			default:
				sval->integer = (average_val / 1000) & 0xFFFF;
				sval->fraction = (average_val % 1000) & 0xFFFF;
			       break;
		}
	} else {
		/* Thermal diode channels */
		sval->integer = ((average_val) & TEMP_INTEGER_MASK) >> TEMP_INTEGER_OFFSET;
		sval->fraction = (((average_val) & TEMP_FRACTION_MASK) >> TEMP_FRACTION_OFFSET) * TEMP_FRACTION_BASE;
	}

	return SENSOR_READ_SUCCESS;
}

uint8_t npcm4xx_adc_init(sensor_cfg *cfg)
{
	CHECK_NULL_ARG_WITH_RETURN(cfg, SENSOR_INIT_UNSPECIFIED_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(cfg->init_args, SENSOR_INIT_UNSPECIFIED_ERROR);

	if (cfg->num > SENSOR_NUM_MAX) {
		LOG_DBG("Invalid sensor number");
		return SENSOR_INIT_UNSPECIFIED_ERROR;
	}

	adc_npcm4xx_init_arg *init_args = (adc_npcm4xx_init_arg *)cfg->init_args;
	if (init_args->is_init)
		goto skip_init;

	init_adc_dev();

	if (!is_ready[0]) {
		LOG_ERR("ADC0 is not ready to use!");
		return SENSOR_INIT_UNSPECIFIED_ERROR;
	}

	init_args->is_init = true;

skip_init:
	cfg->read = npcm4xx_adc_read;

	return SENSOR_INIT_SUCCESS;
}
