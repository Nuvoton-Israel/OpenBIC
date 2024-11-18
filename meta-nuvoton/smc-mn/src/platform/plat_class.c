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

#include "plat_class.h"
#include <logging/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hal_gpio.h"
#include "hal_i3c.h"
#include "libutil.h"
#include "plat_gpio.h"
#include "mctp.h"
#include "mctp_ctrl.h"
#include "plat_mctp.h"

LOG_MODULE_REGISTER(plat_class);

uint8_t slot_eid = 0;
uint8_t slot_id = 0;
uint16_t slot_pid = 0;

uint8_t get_slot_id()
{
	return slot_id;
}

bool pal_get_slot_pid(uint16_t *pid)
{
	*pid = slot_pid;
	return true;
}

void init_platform_config(void)
{
	I3C_MSG i3c_msg;

	i3c_msg.bus = I3C_BUS_BMC;

	slot_eid = plat_get_eid();

	switch (slot_eid) {
		case SLOT1_EID:
			slot_id = 1;
			slot_pid = SLOT1_PID;
			break;
		case SLOT2_EID:
			slot_id = 2;
			slot_pid = SLOT2_PID;
			break;
		case SLOT3_EID:
			slot_id = 3;
			slot_pid = SLOT3_PID;
			break;
		case SLOT4_EID:
			slot_id = 4;
			slot_pid = SLOT4_PID;
			break;
		case SLOT5_EID:
			slot_id = 5;
			slot_pid = SLOT5_PID;
			break;
		case SLOT6_EID:
			slot_id = 6;
			slot_pid = SLOT6_PID;
			break;
		case SLOT7_EID:
			slot_id = 7;
			slot_pid = SLOT7_PID;
			break;
		case SLOT8_EID:
			slot_id = 8;
			slot_pid = SLOT8_PID;
			break;
		default:
			break;
	}

	LOG_INF("Slot EID = %d, Slot ID = %d Slot PID = 0x%x\n", slot_eid, slot_id, slot_pid);

	i3c_set_pid(&i3c_msg, slot_pid);
}
