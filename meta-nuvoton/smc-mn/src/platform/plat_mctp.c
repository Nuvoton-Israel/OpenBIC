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
#include <sys/printk.h>
#include <logging/log.h>
#include <logging/log_ctrl.h>
#include <stdlib.h>
#include "libutil.h"
#include "mctp.h"
#include "mctp_ctrl.h"
#include "pldm.h"
#include "ipmi.h"
#include "sensor.h"
#include "plat_mctp.h"
#include "plat_gpio.h"
#include "plat_i2c.h"
#include "util_sys.h"
#include "plat_def.h"

LOG_MODULE_REGISTER(plat_mctp);

K_TIMER_DEFINE(send_cmd_timer, send_cmd_to_dev, NULL);
K_WORK_DEFINE(send_cmd_work, send_cmd_to_dev_handler);

uint8_t plat_eid = MCTP_DEFAULT_ENDPOINT;

static mctp_port plat_mctp_port[] = {
	{
#ifdef TEST_I3C_TARGET_BIC
		.conf.i3c_conf.addr = I3C_STATIC_ADDR_BIC_SD,
		.conf.i3c_conf.bus = I3C_BUS_TARGET_TO_BIC,
#else
		.conf.i3c_conf.addr = I3C_STATIC_ADDR_BMC,
		.conf.i3c_conf.bus = I3C_BUS_TARGET_TO_BMC,
#endif
		.medium_type = MCTP_MEDIUM_TYPE_TARGET_I3C
	},
	{
		.conf.smbus_conf.addr = I2C_ADDR_BIC,
		.conf.smbus_conf.bus = I2C_BUS_TARGET_TO_BMC,
		.medium_type = MCTP_MEDIUM_TYPE_SMBUS
	},
	{
		.channel_target = PLDM, 
		.conf.usb_conf.addr = 0,
		.conf.usb_conf.bus = 0,
		.medium_type = MCTP_MEDIUM_TYPE_USB
	},
#ifdef TEST_I3C_CONTROLLER_BIC
	{
		.conf.i3c_conf.addr = I3C_STATIC_ADDR_BIC_WF,
		.conf.i3c_conf.bus = I3C_BUS_CONTROLLER_TO_BIC,
		.medium_type = MCTP_MEDIUM_TYPE_CONTROLLER_I3C
	},
	{
		.conf.i3c_conf.addr = I3C_STATIC_ADDR_BIC_FF,
		.conf.i3c_conf.bus = I3C_BUS_CONTROLLER_TO_BIC,
		.medium_type = MCTP_MEDIUM_TYPE_CONTROLLER_I3C
	},
#endif
};

static mctp_route_entry plat_mctp_route_tbl[] = {
	{ MCTP_EID_BMC_I2C, I2C_BUS_TARGET_TO_BMC, I2C_ADDR_BMC, .set_endpoint = false},
	{ MCTP_EID_BMC_I3C, I3C_BUS_TARGET_TO_BMC, I3C_STATIC_ADDR_BMC, .set_endpoint = false},
	{ MCTP_EID_BMC_SERIAL, 0x0, 0x0, .set_endpoint = false},
#ifdef TEST_I3C_CONTROLLER_BIC
	{ MCTP_EID_BIC_I3C_WF, I3C_BUS_CONTROLLER_TO_BIC, I3C_STATIC_ADDR_BIC_WF, .set_endpoint = true},
	{ MCTP_EID_BIC_I3C_FF, I3C_BUS_CONTROLLER_TO_BIC, I3C_STATIC_ADDR_BIC_FF, .set_endpoint = true},
#endif
};

mctp *find_mctp_by_medium_type(uint8_t type)
{
	uint8_t i;
	for (i = 0; i < ARRAY_SIZE(plat_mctp_port); i++) {
		mctp_port *p = plat_mctp_port + i;
		if (p->medium_type == type)
			return p->mctp_inst;
	}

	return NULL;
}

mctp *find_mctp_by_bus(uint8_t bus)
{
	uint8_t i;
	for (i = 0; i < ARRAY_SIZE(plat_mctp_port); i++) {
		mctp_port *p = plat_mctp_port + i;

		if (p->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
			if (bus == p->conf.smbus_conf.bus) {
				return p->mctp_inst;
			}
		} else if (p->medium_type == MCTP_MEDIUM_TYPE_CONTROLLER_I3C ||
				p->medium_type == MCTP_MEDIUM_TYPE_TARGET_I3C) {
			if (bus == p->conf.i3c_conf.bus) {
				return p->mctp_inst;
			}
		} else if (p->medium_type == MCTP_MEDIUM_TYPE_USB) {
			if (bus == p->conf.usb_conf.bus) {
				return p->mctp_inst;
			}
		} else {
			LOG_ERR("Unknown medium type:0x%x\n", p->medium_type);
			return NULL;
		}
	}

	return NULL;
}

mctp *find_mctp_by_addr_and_bus(uint8_t addr, uint8_t bus)
{
	uint8_t i;
	for (i = 0; i < ARRAY_SIZE(plat_mctp_port); i++) {
		mctp_port *p = plat_mctp_port + i;

		if (p->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
			if ((bus == p->conf.smbus_conf.bus) && (addr == p->conf.smbus_conf.addr)) {
				return p->mctp_inst;
			}
		} else if (p->medium_type == MCTP_MEDIUM_TYPE_TARGET_I3C ||
				p->medium_type == MCTP_MEDIUM_TYPE_CONTROLLER_I3C) {
			if ((bus == p->conf.i3c_conf.bus) && (addr == p->conf.i3c_conf.addr)) {
				return p->mctp_inst;
			}
		} else if (p->medium_type == MCTP_MEDIUM_TYPE_USB) {
			if ((bus == p->conf.usb_conf.bus) && (addr == 0)) {
				return p->mctp_inst;
			}
		} else {
			LOG_ERR("Unknown medium type:0x%x\n", p->medium_type);
			return NULL;
		}
	}

	return NULL;
}

mctp *find_mctp_by_addr(uint8_t addr)
{
	uint8_t i;
	for (i = 0; i < ARRAY_SIZE(plat_mctp_port); i++) {
		mctp_port *p = plat_mctp_port + i;

		if (p->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
			if (addr == p->conf.smbus_conf.addr) {
				return p->mctp_inst;
			}
		} else if (p->medium_type == MCTP_MEDIUM_TYPE_CONTROLLER_I3C ||
				p->medium_type == MCTP_MEDIUM_TYPE_TARGET_I3C) {
			if (addr == p->conf.i3c_conf.addr) {
				return p->mctp_inst;
			}
		} else if (p->medium_type == MCTP_MEDIUM_TYPE_USB){
			if (addr == 0) {
				return p->mctp_inst;
			}
		} else {
			LOG_ERR("Unknown medium type");
			return NULL;
		}
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

static uint8_t get_mctp_route_info(uint8_t dest_endpoint, void **mctp_inst,
				   mctp_ext_params *ext_params)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(ext_params, MCTP_ERROR);

	uint8_t rc = MCTP_ERROR;
	uint32_t i;

	for (i = 0; i < ARRAY_SIZE(plat_mctp_route_tbl); i++) {
		mctp_route_entry *p = plat_mctp_route_tbl + i;
		if (p->endpoint == dest_endpoint) {
			*mctp_inst = find_mctp_by_addr_and_bus(p->addr, p->bus);
			if (p->bus == 0) {
				ext_params->type = MCTP_MEDIUM_TYPE_USB;
				ext_params->usb_ext_params.dummy = p->addr;
			} else if (p->bus == I3C_BUS_CONTROLLER_TO_BIC) {
				ext_params->type = MCTP_MEDIUM_TYPE_CONTROLLER_I3C;
				ext_params->i3c_ext_params.addr = p->addr;
			} else if (p->bus != I3C_BUS_TARGET_TO_BMC) {
				ext_params->type = MCTP_MEDIUM_TYPE_SMBUS;
				ext_params->smbus_ext_params.addr = p->addr;
			} else {
				ext_params->type = MCTP_MEDIUM_TYPE_TARGET_I3C;
				ext_params->i3c_ext_params.addr = p->addr;
			}
			rc = MCTP_SUCCESS;
			break;
		}
	}

	return rc;
}

uint8_t get_mctp_info(uint8_t dest_endpoint, mctp **mctp_inst, mctp_ext_params *ext_params)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(ext_params, MCTP_ERROR);

	uint8_t rc = MCTP_ERROR;
	uint32_t i;

	for (i = 0; i < ARRAY_SIZE(plat_mctp_route_tbl); i++) {
		mctp_route_entry *p = plat_mctp_route_tbl + i;
		if (p->endpoint == dest_endpoint) {
			*mctp_inst = find_mctp_by_bus(p->bus);
			if (p->bus == 0x0) {
				ext_params->type = MCTP_MEDIUM_TYPE_USB;
				ext_params->usb_ext_params.dummy = p->addr;
			} else if (p->bus != I3C_BUS_TARGET_TO_BMC) {
				ext_params->type = MCTP_MEDIUM_TYPE_SMBUS;
				ext_params->smbus_ext_params.addr = p->addr;
			} else {
				ext_params->type = MCTP_MEDIUM_TYPE_TARGET_I3C;
				ext_params->i3c_ext_params.addr = p->addr;
			}
			ext_params->ep = p->endpoint;
			rc = MCTP_SUCCESS;
			break;
		}
	}

	return rc;
}

static void set_endpoint_resp_handler(void *args, uint8_t *buf, uint16_t len)
{
	ARG_UNUSED(args);
	CHECK_NULL_ARG(buf);
	//TODO: support set device endpoint
	LOG_HEXDUMP_DBG(buf, len, __func__);
}

static void set_endpoint_resp_timeout(void *args)
{
	CHECK_NULL_ARG(args);
	//TODO: support set device endpoint
	mctp_route_entry *p = (mctp_route_entry *)args;
	LOG_DBG("Endpoint 0x%x set endpoint failed on bus %d", p->endpoint, p->bus);
}

static void set_dev_endpoint(void)
{
	// We only need to set FF BIC EID and WF BIC EID.
	for (uint8_t i = 0; i < ARRAY_SIZE(plat_mctp_route_tbl); i++) {
		mctp_route_entry *p = plat_mctp_route_tbl + i;
		if (!p->set_endpoint)
			continue;

		for (uint8_t j = 0; j < ARRAY_SIZE(plat_mctp_port); j++) {
			if (p->addr != plat_mctp_port[j].conf.i3c_conf.addr)
				continue;

			struct _set_eid_req req = { 0 };
			req.op = SET_EID_REQ_OP_SET_EID;
			req.eid = p->endpoint;

			mctp_ctrl_msg msg;
			memset(&msg, 0, sizeof(msg));
			msg.ext_params.type = plat_mctp_port[j].medium_type;
			msg.ext_params.i3c_ext_params.addr = p->addr;

			msg.hdr.cmd = MCTP_CTRL_CMD_SET_ENDPOINT_ID;
			msg.hdr.rq = 1;

			msg.cmd_data = (uint8_t *)&req;
			msg.cmd_data_len = sizeof(req);

			msg.recv_resp_cb_fn = set_endpoint_resp_handler;
			msg.timeout_cb_fn = set_endpoint_resp_timeout;
			msg.timeout_cb_fn_args = p;

			uint8_t rc = mctp_ctrl_send_msg(find_mctp_by_addr(p->addr), &msg);
			if (rc)
				LOG_ERR("Fail to set endpoint %d", p->endpoint);
		}
	}
}

void send_cmd_to_dev_handler(struct k_work *work)
{
	/* init the device endpoint */
	set_dev_endpoint();
}

void send_cmd_to_dev(struct k_timer *timer)
{
	k_work_submit(&send_cmd_work);
}

void plat_mctp_init()
{
	int ret = 0;

	/* init the mctp/pldm instance */
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

		ret = mctp_start(p->mctp_inst);
	}

#ifdef TEST_I3C_CONTROLLER_BIC
	k_timer_start(&send_cmd_timer, K_MSEC(3000), K_NO_WAIT);
#endif
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

	// Set platform eid
	plat_eid = eid;

	return;
}

uint8_t plat_get_eid()
{
	return plat_eid;
}

uint8_t pal_get_bmc_interface()
{
	return BMC_INTERFACE_USB;
}
