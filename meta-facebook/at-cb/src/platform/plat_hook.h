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

#ifndef PLAT_HOOK_H
#define PLAT_HOOK_H

#include "pmbus.h"
#include "sensor.h"
#include "common_i2c_mux.h"

typedef struct _pwr_monitor_pre_proc_arg {
	mux_config mux_configs;
	uint8_t card_id;
} pwr_monitor_pre_proc_arg;

/**************************************************************************************************
 * INIT ARGS
**************************************************************************************************/
extern adc_asd_init_arg adc_asd_init_args[];
extern adm1272_init_arg adm1272_init_args[];
extern ltc4286_init_arg ltc4286_init_args[];
extern ina233_init_arg ina233_init_args[];
extern pex89000_init_arg pex_sensor_init_args[];
extern ina233_init_arg accl_ina233_init_args[];
extern sq52205_init_arg sq52205_init_args[];

/**************************************************************************************************
 *  PRE-HOOK/POST-HOOK ARGS
 **************************************************************************************************/
extern mux_config tca9543_configs[];
extern vr_page_cfg xdpe15284_page[];
extern mux_config pca9548_configs[];
extern mux_config pca9546_configs[];
extern uint8_t plat_monitor_table_arg[];
extern pwr_monitor_pre_proc_arg pwr_monitor_pre_proc_args[];

/**************************************************************************************************
 *  PRE-HOOK/POST-HOOK FUNC
 **************************************************************************************************/
bool pre_ina233_read(sensor_cfg *cfg, void *args);
bool post_ina233_read(sensor_cfg *cfg, void *args, int *reading);
bool pre_pex89000_read(sensor_cfg *cfg, void *args);
bool post_pex89000_read(sensor_cfg *cfg, void *args, int *reading);
bool pre_xdpe15284_read(sensor_cfg *cfg, void *args);
bool post_xdpe15284_read(sensor_cfg *cfg, void *args, int *reading);
bool pre_accl_mux_switch(uint8_t sensor_num, void *arg);
bool post_accl_mux_switch(uint8_t sensor_num, void *arg);
bool pre_accl_nvme_read(sensor_cfg *cfg, void *args);

#endif
