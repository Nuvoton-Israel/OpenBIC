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
 * This platform does not build the common IPMI and sensor services
 * (common/service/ipmi, common/service/sensor, see CMakeLists.txt).
 * IPMI requests from the host are bridged to the BMC over OBMF instead.
 * Provide the symbols the remaining common code still links against.
 */

#include <zephyr.h>
#include <shell/shell.h>
#include <logging/log.h>

#include "ipmi.h"
#include "sensor.h"
#include "pdr.h"
#include "sensor_shell.h"

LOG_MODULE_REGISTER(plat_service_stub);

/* IPMI service */
K_MSGQ_DEFINE(self_ipmi_msgq, sizeof(ipmi_msg), 1, 4);

void ipmi_init(void)
{
	return;
}

ipmb_error notify_ipmi_client(ipmi_msg_cfg *msg_cfg)
{
	ARG_UNUSED(msg_cfg);
	LOG_WRN("IPMI service is not supported, drop message");
	return IPMB_ERROR_FAILURE;
}

bool pal_set_dimm_presence_status(uint8_t *buf)
{
	ARG_UNUSED(buf);
	return true;
}

bool pal_immediate_respond_from_HOST(uint8_t netfn, uint8_t cmd)
{
	ARG_UNUSED(netfn);
	ARG_UNUSED(cmd);
	return false;
}

int pal_record_bios_fw_version(uint8_t *buf, uint8_t size)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(size);
	return -2;
}

/* Sensor service */
sensor_cfg *sensor_config;
uint8_t sensor_config_count;

bool sensor_init(void)
{
	return true;
}

uint8_t get_sensor_reading(sensor_cfg *cfg_table, uint8_t cfg_count, uint8_t sensor_num,
			   int *reading, uint8_t read_mode)
{
	ARG_UNUSED(cfg_table);
	ARG_UNUSED(cfg_count);
	ARG_UNUSED(sensor_num);
	ARG_UNUSED(reading);
	ARG_UNUSED(read_mode);
	return SENSOR_NOT_FOUND;
}

PDR_INFO *get_pdr_info()
{
	return NULL;
}

uint32_t get_record_count()
{
	return 0;
}

int get_pdr_table_via_record_handle(uint8_t *record_data, uint32_t record_handle)
{
	ARG_UNUSED(record_data);
	ARG_UNUSED(record_handle);
	return 0;
}

int get_pdr_with_sensor_id(uint32_t sensorID, PDR_numeric_sensor *numeric_sensor_pdr)
{
	ARG_UNUSED(sensorID);
	ARG_UNUSED(numeric_sensor_pdr);
	return -1;
}

/* Sensor shell commands */
static void sensor_cmd_not_supported(const struct shell *shell)
{
	shell_print(shell, "Sensor service is not supported on this platform");
}

void cmd_sensor_cfg_list_all_table(const struct shell *shell, size_t argc, char **argv)
{
	sensor_cmd_not_supported(shell);
}

void cmd_sensor_cfg_list_all_sensor(const struct shell *shell, size_t argc, char **argv)
{
	sensor_cmd_not_supported(shell);
}

void cmd_sensor_cfg_get_table_all_sensor(const struct shell *shell, size_t argc, char **argv)
{
	sensor_cmd_not_supported(shell);
}

void cmd_sensor_cfg_get_table_single_sensor(const struct shell *shell, size_t argc, char **argv)
{
	sensor_cmd_not_supported(shell);
}

void cmd_control_sensor_polling(const struct shell *shell, size_t argc, char **argv)
{
	sensor_cmd_not_supported(shell);
}
