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
#include <stdlib.h>
#include "ipmi.h"
#include "libutil.h"
#include "plat_ipmi.h"
#include "plat_ipmb.h"
#include "expansion_board.h"
#include <logging/log.h>
#include "oem_1s_handler.h"
#include "cci.h"
#include "mctp.h"
#include "plat_mctp.h"
#include "plat_fru.h"
#include "eeprom.h"

LOG_MODULE_REGISTER(plat_ipmi);

bool pal_request_msg_to_BIC_from_HOST(uint8_t netfn, uint8_t cmd)
{
	if (netfn == NETFN_OEM_1S_REQ) {
		if ((cmd == CMD_OEM_1S_FW_UPDATE) || (cmd == CMD_OEM_1S_RESET_BMC) ||
		    (cmd == CMD_OEM_1S_GET_BIC_STATUS) || (cmd == CMD_OEM_1S_RESET_BIC) ||
		    (cmd == CMD_OEM_1S_GET_BIC_FW_INFO))
			return true;
	} else if (netfn == NETFN_APP_REQ) {
		if ((cmd == CMD_APP_GET_SYSTEM_GUID) ||
		    (cmd == CMD_APP_GET_SYSTEM_INTERFACE_CAPABILITIES) ||
		    (cmd == CMD_APP_CLEAR_MSG_FLAGS) || (cmd == CMD_APP_SET_BMC_GLOBAL_ENABLES) ||
		    (cmd == CMD_APP_GET_BMC_GLOBAL_ENABLES) || (cmd == CMD_APP_GET_DEVICE_GUID) ||
		    (cmd == CMD_APP_GET_DEVICE_ID) || (cmd == CMD_APP_GET_CHANNEL_INFO)) {
			return true;
		}
	}

	return false;
}

void OEM_1S_GET_FW_VERSION(ipmi_msg *msg)
{
	CHECK_NULL_ARG(msg);
	if (msg->data_len != 1) {
		msg->completion_code = CC_INVALID_LENGTH;
		return;
	}
	bool ret = false;
	uint8_t component = msg->data[0];
	EEPROM_ENTRY get_cxl_ver = { 0 };

	switch (component) {
	case RF_COMPNT_BIC:
		msg->data[0] = BIC_FW_YEAR_MSB;
		msg->data[1] = BIC_FW_YEAR_LSB;
		msg->data[2] = BIC_FW_WEEK;
		msg->data[3] = BIC_FW_VER;
		msg->data[4] = BIC_FW_platform_0;
		msg->data[5] = BIC_FW_platform_1;
		msg->data[6] = BIC_FW_platform_2;
		msg->data_len = 7;
		msg->completion_code = CC_SUCCESS;
		break;
	case RF_COMPNT_CXL:
		ret = get_cxl_version(&get_cxl_ver);
		if (ret == false) {
			msg->completion_code = CC_UNSPECIFIED_ERROR;
		} else {
			memcpy(&msg->data[0], get_cxl_ver.data, CXL_VERSION_LEN);
			msg->data_len = CXL_VERSION_LEN;
			msg->completion_code = CC_SUCCESS;
		}
		break;
	default:
		msg->completion_code = CC_UNSPECIFIED_ERROR;
		break;
	}

	return;
}
