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

#include "plat_ipmi.h"

#include <stdio.h>
#include <stdlib.h>

#include "libutil.h"
#include "ipmi.h"
#include "plat_class.h"
#include "plat_ipmb.h"
#include <logging/log.h>
#include "plat_dimm.h"

LOG_MODULE_REGISTER(plat_ipmi);

bool pal_set_dimm_presence_status(uint8_t *buf)
{
	CHECK_NULL_ARG_WITH_RETURN(buf, -1);

	uint8_t dimm_index = buf[DIMM_INDEX_BYTE];
	uint8_t status = buf[DIMM_STATUS_BYTE];

	if (dimm_index < DIMM_INDEX_MIN || dimm_index > DIMM_INDEX_MAX) {
		LOG_ERR("DIMM index is out of range");
		return false;
	}

	set_dimm_presence_status(dimm_index, status);

	return true;
}

void OEM_1S_GET_CARD_TYPE(ipmi_msg *msg)
{
	if (msg == NULL) {
		LOG_ERR("Failed due to parameter *msg is NULL");
		return;
	}

	if (msg->data_len != 1) {
		msg->completion_code = CC_INVALID_LENGTH;
		return;
	}

	CARD_STATUS _1ou_status = get_1ou_status();
	switch (msg->data[0]) {
	case GET_1OU_CARD_TYPE:
		msg->data_len = 2;
		msg->completion_code = CC_SUCCESS;
		msg->data[0] = GET_1OU_CARD_TYPE;
		if (_1ou_status.present) {
			msg->data[1] = _1ou_status.card_type;
		} else {
			msg->data[1] = TYPE_1OU_ABSENT;
		}
		break;
	default:
		msg->data_len = 0;
		msg->completion_code = CC_INVALID_DATA_FIELD;
		break;
	}

	return;
}
