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
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <logging/log.h>
#include "libutil.h"
#include "mctp.h"
#include "mctp_ctrl.h"
#include "pldm.h"
#include "guid.h"
#include "util_sys.h"
#include "plat_def.h"
#include "plat_mctp.h"

LOG_MODULE_REGISTER(plat_mctp);

/* Platform route entry, the medium is explicit instead of guessed from the bus */
typedef struct _plat_mctp_route {
	uint8_t endpoint;
	MCTP_MEDIUM_TYPE medium_type;
	uint8_t bus;
	uint8_t addr;
} plat_mctp_route;

static uint8_t plat_eid = MCTP_DEFAULT_ENDPOINT;

static mctp_port plat_mctp_port[] = {
#ifdef ENABLE_MCTP_I3C
	{
		.conf.i3c_conf.addr = I3C_STATIC_ADDR_BMC,
		.conf.i3c_conf.bus = I3C_BUS_TARGET_TO_BMC,
		.medium_type = MCTP_MEDIUM_TYPE_TARGET_I3C,
	},
#endif
#ifdef ENABLE_MCTP_USB
	{
		.channel_target = PLDM,
		.conf.usb_conf.addr = 0,
		.conf.usb_conf.bus = 0,
		.medium_type = MCTP_MEDIUM_TYPE_USB,
	},
#endif
};

static const plat_mctp_route plat_mctp_route_tbl[] = {
#ifdef ENABLE_MCTP_I3C
	{ MCTP_EID_BMC_I3C, MCTP_MEDIUM_TYPE_TARGET_I3C, I3C_BUS_TARGET_TO_BMC,
	  I3C_STATIC_ADDR_BMC },
#endif
#ifdef ENABLE_MCTP_USB
	{ MCTP_EID_BMC_USB, MCTP_MEDIUM_TYPE_USB, 0, 0 },
#endif
};

static uint8_t mctp_port_bus(const mctp_port *p)
{
	switch (p->medium_type) {
	case MCTP_MEDIUM_TYPE_CONTROLLER_I3C:
	case MCTP_MEDIUM_TYPE_TARGET_I3C:
		return p->conf.i3c_conf.bus;
	case MCTP_MEDIUM_TYPE_USB:
		return p->conf.usb_conf.bus;
	default:
		return p->conf.smbus_conf.bus;
	}
}

static mctp *find_mctp_by_medium_and_bus(uint8_t type, uint8_t bus)
{
	for (uint8_t i = 0; i < ARRAY_SIZE(plat_mctp_port); i++) {
		mctp_port *p = plat_mctp_port + i;
		if (p->medium_type == type && mctp_port_bus(p) == bus)
			return p->mctp_inst;
	}

	return NULL;
}

mctp *find_mctp_by_medium_type(uint8_t type)
{
	for (uint8_t i = 0; i < ARRAY_SIZE(plat_mctp_port); i++) {
		mctp_port *p = plat_mctp_port + i;
		if (p->medium_type == type)
			return p->mctp_inst;
	}

	return NULL;
}

mctp *find_mctp_by_bus(uint8_t bus)
{
	for (uint8_t i = 0; i < ARRAY_SIZE(plat_mctp_port); i++) {
		mctp_port *p = plat_mctp_port + i;
		if (mctp_port_bus(p) == bus)
			return p->mctp_inst;
	}

	return NULL;
}

static uint8_t mctp_msg_recv(void *mctp_p, uint8_t *buf, uint32_t len, mctp_ext_params ext_params)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_p, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(buf, MCTP_ERROR);

	/* first byte is message type and ic */
	uint8_t msg_type = (buf[0] & MCTP_MSG_TYPE_MASK) >> MCTP_MSG_TYPE_SHIFT;

	switch (msg_type) {
	case MCTP_MSG_TYPE_CTRL:
		mctp_ctrl_cmd_handler(mctp_p, buf, len, ext_params);
		break;

	case MCTP_MSG_TYPE_PLDM:
		mctp_pldm_cmd_handler(mctp_p, buf, len, ext_params);
		break;
	default:
		LOG_WRN("Cannot find message receive function!!");
		return MCTP_ERROR;
	}

	return MCTP_SUCCESS;
}

uint8_t get_mctp_info(uint8_t dest_endpoint, mctp **mctp_inst, mctp_ext_params *ext_params)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(ext_params, MCTP_ERROR);

	for (uint32_t i = 0; i < ARRAY_SIZE(plat_mctp_route_tbl); i++) {
		const plat_mctp_route *r = plat_mctp_route_tbl + i;
		if (r->endpoint != dest_endpoint)
			continue;

		*mctp_inst = find_mctp_by_medium_and_bus(r->medium_type, r->bus);
		ext_params->type = r->medium_type;
		ext_params->ep = r->endpoint;

		switch (r->medium_type) {
		case MCTP_MEDIUM_TYPE_CONTROLLER_I3C:
		case MCTP_MEDIUM_TYPE_TARGET_I3C:
			ext_params->i3c_ext_params.addr = r->addr;
			break;
		case MCTP_MEDIUM_TYPE_USB:
			ext_params->usb_ext_params.dummy = r->addr;
			break;
		default:
			ext_params->smbus_ext_params.addr = r->addr;
			break;
		}

		return MCTP_SUCCESS;
	}

	return MCTP_ERROR;
}

static uint8_t get_mctp_route_info(uint8_t dest_endpoint, void **mctp_inst,
				   mctp_ext_params *ext_params)
{
	return get_mctp_info(dest_endpoint, (mctp **)mctp_inst, ext_params);
}

void plat_mctp_init()
{
	for (uint8_t i = 0; i < ARRAY_SIZE(plat_mctp_port); i++) {
		mctp_port *p = plat_mctp_port + i;

		p->mctp_inst = mctp_init();
		if (!p->mctp_inst) {
			LOG_ERR("mctp_init failed!!");
			continue;
		}

		uint8_t rc = mctp_set_medium_configure(p->mctp_inst, p->medium_type, p->conf);
		if (rc != MCTP_SUCCESS) {
			LOG_ERR("mctp set medium configure failed");
		}

		mctp_reg_endpoint_resolve_func(p->mctp_inst, get_mctp_route_info);

		mctp_reg_msg_rx_func(p->mctp_inst, mctp_msg_recv);

		mctp_start(p->mctp_inst);
	}
}

uint8_t plat_get_mctp_port_count()
{
	return ARRAY_SIZE(plat_mctp_port);
}

mctp_port *plat_get_mctp_port(uint8_t index)
{
	return plat_mctp_port + index;
}

void plat_update_mctp_routing_table(uint8_t eid)
{
	LOG_WRN("update eid from 0x%x to 0x%x", plat_eid, eid);

	plat_eid = eid;
}

uint8_t plat_get_eid()
{
	return plat_eid;
}

uint8_t pal_get_bmc_interface()
{
	return BMC_INTERFACE_USB;
}

uint8_t get_system_guid(uint16_t *data_len, uint8_t *data)
{
	/* Fake UUID for reference */
	const uint8_t fake_guid[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
					0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F };

	if (data == NULL || data_len == NULL) {
		return GUID_FAIL_TO_ACCESS;
	}

	*data_len = 16;
	memcpy(data, fake_guid, 16);

	return GUID_READ_SUCCESS;
}
