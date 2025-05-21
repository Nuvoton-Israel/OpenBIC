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

K_TIMER_DEFINE(send_discovery_notify_cmd_timer, send_discovery_notify_cmd, NULL);
K_WORK_DEFINE(send_discovery_notify_cmd_work, send_discovery_notify_cmd_handler);

uint8_t plat_eid = MCTP_DEFAULT_ENDPOINT;
mctp_reg_eid_work reg_eid_work[MAX_WORK_ITEMS];

static mctp_port plat_mctp_port[] = {
	/*{
#ifdef TEST_I3C_TARGET_BIC
		.conf.i3c_conf.addr = I3C_STATIC_ADDR_BIC_SD,
		.conf.i3c_conf.bus = I3C_BUS_TARGET_TO_BIC,
#else
		.conf.i3c_conf.addr = I3C_STATIC_ADDR_BMC,
		.conf.i3c_conf.bus = I3C_BUS_TARGET_TO_BMC,
#endif
		.medium_type = MCTP_MEDIUM_TYPE_TARGET_I3C,
		.support_bridge = true,
		.bus_owner = true,
		.eid_pool_size = 0,
		.eid_pool_first_eid = 0,
		.required_eid_pool_from_BO = 0,
	},*/
	{
		.conf.i3c_conf.addr = I3C_MNG_ADDR,
		.conf.i3c_conf.bus = I3C_BUS_CONTROLLER_TO_HUB, //i3c5 is as a controller
		.medium_type = MCTP_MEDIUM_TYPE_CONTROLLER_I3C,
		.support_bridge = true,
		.bus_owner = false, //true,
		.eid_pool_size = 0,
		.eid_pool_first_eid = 0,
		.required_eid_pool_from_BO = 3,
	},
	{
		.conf.smbus_conf.addr = I2C_ADDR_BIC,
		.conf.smbus_conf.bus = I2C_BUS_TARGET_TO_BMC,
		.medium_type = MCTP_MEDIUM_TYPE_SMBUS,
		.support_bridge = true,
		.bus_owner = true,
		.eid_pool_size = 0,
		.eid_pool_first_eid = 0,
		.required_eid_pool_from_BO = 0,
	},
	{
		.channel_target = PLDM, 
		.conf.usb_conf.addr = 0,
		.conf.usb_conf.bus = 0,
		.medium_type = MCTP_MEDIUM_TYPE_USB,
		.support_bridge = true,
		.bus_owner = true,
		.eid_pool_size = 3,
		.eid_pool_first_eid = 0x10,
		.required_eid_pool_from_BO = 0,
	},
/*
#ifdef TEST_I3C_CONTROLLER_BIC
	{
		.conf.i3c_conf.addr = I3C_STATIC_ADDR_BIC_WF,
		.conf.i3c_conf.bus = I3C_BUS_CONTROLLER_TO_BIC,
		.medium_type = MCTP_MEDIUM_TYPE_CONTROLLER_I3C,
		.support_bridge = true,
		.bus_owner = true,
		.eid_pool_size = 0,
		.eid_pool_first_eid = 0,
		.required_eid_pool_from_BO = 0,
	},
	{
		.conf.i3c_conf.addr = I3C_STATIC_ADDR_BIC_FF,
		.conf.i3c_conf.bus = I3C_BUS_CONTROLLER_TO_BIC,
		.medium_type = MCTP_MEDIUM_TYPE_CONTROLLER_I3C,
		.support_bridge = true,
		.bus_owner = true,
		.eid_pool_size = 0,
		.eid_pool_first_eid = 0,
		.required_eid_pool_from_BO = 0,
	},
#endif
*/
};

#if SUPPORT_DYNAMIC_MCTP_ROUTE_TBL
size_t plat_mctp_route_tbl_size = MCTP_DEFAULT_ROUTE_TBL_SIZE;
static mctp_route_entry *plat_mctp_route_tbl = NULL;
#else
static mctp_route_entry plat_mctp_route_tbl[] = {
	{ MCTP_EID_BMC_I2C, I2C_BUS_TARGET_TO_BMC, I2C_ADDR_BMC, .set_endpoint = false},
	//{ MCTP_EID_BMC_I3C, I3C_BUS_TARGET_TO_BMC, I3C_STATIC_ADDR_BMC, .set_endpoint = false},
	{ MCTP_EID_BMC_SERIAL, 0x0, 0x0, .set_endpoint = false},
/*
#ifdef TEST_I3C_CONTROLLER_BIC
	{ MCTP_EID_BIC_I3C_WF, I3C_BUS_CONTROLLER_TO_BIC, I3C_STATIC_ADDR_BIC_WF, .set_endpoint = true},
	{ MCTP_EID_BIC_I3C_FF, I3C_BUS_CONTROLLER_TO_BIC, I3C_STATIC_ADDR_BIC_FF, .set_endpoint = true},
#endif
*/
	{ MCTP_EID_MNG_I3C, I3C_BUS_CONTROLLER_TO_HUB, I3C_MNG_ADDR, .set_endpoint = false},
};
#endif

mctp_port *find_port_by_mctp_inst(mctp *mctp_inst)
{
	uint8_t i;
	for (i = 0; i < ARRAY_SIZE(plat_mctp_port); i++) {
		mctp_port *p = plat_mctp_port + i;
		if (p->mctp_inst == mctp_inst)
			return p;
	}

	return NULL;
}

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
#ifdef ENABLE_EDAF_OVER_MCTP
	case MCTP_MSG_TYPE_FLASH:
		mctp_flash_msg_handler(mctp_p, buf, len, ext_params);
		break;
#endif
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

#if SUPPORT_DYNAMIC_MCTP_ROUTE_TBL
	for (i = 0; i < plat_mctp_route_tbl_size; i++) {
#else
	for (i = 0; i < ARRAY_SIZE(plat_mctp_route_tbl); i++) {
#endif
		mctp_route_entry *r = plat_mctp_route_tbl + i;
		if (r->endpoint == dest_endpoint) {
			*mctp_inst = find_mctp_by_addr_and_bus(r->addr, r->bus);

			mctp_port *p = find_port_by_mctp_inst(*mctp_inst);
			if (p != NULL) {
				ext_params->type = p->medium_type;
				if (ext_params->type == MCTP_MEDIUM_TYPE_USB) {
					ext_params->usb_ext_params.dummy = r->addr;
				} else if (ext_params->type == MCTP_MEDIUM_TYPE_SMBUS) {
					ext_params->smbus_ext_params.addr = r->addr;
				} else {
					ext_params->i3c_ext_params.addr = r->addr;
				}
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

#if SUPPORT_DYNAMIC_MCTP_ROUTE_TBL
	for (i = 0; i < plat_mctp_route_tbl_size; i++) {
#else
	for (i = 0; i < ARRAY_SIZE(plat_mctp_route_tbl); i++) {
#endif
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
#if SUPPORT_DYNAMIC_MCTP_ROUTE_TBL
	for (uint8_t i = 0; i < plat_mctp_route_tbl_size; i++) {
#else
	for (uint8_t i = 0; i < ARRAY_SIZE(plat_mctp_route_tbl); i++) {
#endif
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

void mctp_reg_eid_handler(struct k_work *work)
{
	mctp_reg_eid_work *reg_eid_work_p = CONTAINER_OF(work, mctp_reg_eid_work, work);

	register_endpoint(reg_eid_work_p->mctp_inst, reg_eid_work_p->eid, SIMPLE_ENDPOINT);
}

static void get_mctp_ver_support_resp_handler(void *args, uint8_t *buf, uint16_t len)
{
	ARG_UNUSED(args);
	CHECK_NULL_ARG(buf);

	if (buf[0] == MCTP_CTRL_CC_SUCCESS) {
		//TODO: support get mctp version support
		LOG_HEXDUMP_DBG(buf, len, __func__);
	}
}

bool get_mctp_ver_support_ctrl_cmd(mctp *mctp_inst, uint8_t dest_eid, uint8_t msg_type_no)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);

	mctp_medium_conf *conf = &mctp_inst->medium_conf;

	struct _get_mctp_ver_support_req req = { 0 };
	req.msg_type_number = msg_type_no;

	mctp_ctrl_msg msg = { 0 };
	msg.ext_params.type = mctp_inst->medium_type;

	if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_USB) {
		msg.ext_params.usb_ext_params.dummy = conf->usb_conf.addr;
	} else if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
		msg.ext_params.smbus_ext_params.addr = conf->smbus_conf.addr;
	} else {
		msg.ext_params.i3c_ext_params.addr = conf->i3c_conf.addr;
	}
	msg.ext_params.ep = dest_eid; //destination

	msg.hdr.cmd = MCTP_CTRL_CMD_GET_MCTP_VERSION_SUPPORT;
	msg.hdr.rq = MCTP_REQUEST;
	msg.cmd_data = (uint8_t *)&req;
	msg.cmd_data_len = sizeof(req);

	msg.recv_resp_cb_fn = get_mctp_ver_support_resp_handler;

	uint8_t ret = mctp_ctrl_send_msg(mctp_inst, &msg);
	if (ret) {
		LOG_ERR("Get MCTP version support failed.");
	}

	return ret;
}

bool get_eid_ctrl_cmd(mctp *mctp_inst, uint8_t dest_eid, struct _get_eid_resp *resp)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(resp, MCTP_ERROR);

	uint8_t ret = MCTP_ERROR;
	mctp_medium_conf *conf = &mctp_inst->medium_conf;
	mctp_ctrl_msg msg = { 0 };

	msg.ext_params.type = mctp_inst->medium_type;

	if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_USB) {
		msg.ext_params.usb_ext_params.dummy = conf->usb_conf.addr;
	} else if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
		msg.ext_params.smbus_ext_params.addr = conf->smbus_conf.addr;
	} else {
		msg.ext_params.i3c_ext_params.addr = conf->i3c_conf.addr;
	}
	msg.ext_params.ep = dest_eid; //destination

	msg.hdr.cmd = MCTP_CTRL_CMD_GET_ENDPOINT_ID;
	msg.hdr.rq = MCTP_REQUEST;
	msg.cmd_data_len = 0;

	ret = mctp_ctrl_read(mctp_inst, &msg, (uint8_t *)resp, sizeof(*resp));
	if (ret) {
		LOG_ERR("Get endpoint id failed.");
	} else {
		LOG_DBG("Get eid: %d", resp->eid);
	}

	return ret;
}

bool get_uuid_ctrl_cmd(mctp *mctp_inst, uint8_t dest_eid, struct _get_uuid_resp *resp)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(resp, MCTP_ERROR);

	uint8_t ret = MCTP_ERROR;
	mctp_medium_conf *conf = &mctp_inst->medium_conf;
	mctp_ctrl_msg msg = { 0 };

	msg.ext_params.type = mctp_inst->medium_type;

	if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_USB) {
		msg.ext_params.usb_ext_params.dummy = conf->usb_conf.addr;
	} else if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
		msg.ext_params.smbus_ext_params.addr = conf->smbus_conf.addr;
	} else {
		msg.ext_params.i3c_ext_params.addr = conf->i3c_conf.addr;
	}
	msg.ext_params.ep = dest_eid; //destination

	msg.hdr.cmd = MCTP_CTRL_CMD_GET_UUID;
	msg.hdr.rq = MCTP_REQUEST;
	msg.cmd_data_len = 0;

	ret = mctp_ctrl_read(mctp_inst, &msg, (uint8_t *)resp, sizeof(*resp));
	if (ret) {
		LOG_ERR("Get uuid failed.");
	} else {
		LOG_HEXDUMP_DBG(resp->uuid, sizeof(resp->uuid), "Get UUID");
	}

	return ret;
}

bool set_eid_ctrl_cmd(mctp *mctp_inst, uint8_t dest_eid, uint8_t operation,
					  uint8_t eid, struct _set_eid_resp *resp)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(resp, MCTP_ERROR);

	uint8_t ret = MCTP_ERROR;
	mctp_medium_conf *conf = &mctp_inst->medium_conf;

	struct _set_eid_req req = { 0 };
	req.op = operation;
	req.eid = eid;

	mctp_ctrl_msg msg = { 0 };
	msg.ext_params.type = mctp_inst->medium_type;

	if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_USB) {
		msg.ext_params.usb_ext_params.dummy = conf->usb_conf.addr;
	} else if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
		msg.ext_params.smbus_ext_params.addr = conf->smbus_conf.addr;
	} else {
		msg.ext_params.i3c_ext_params.addr = conf->i3c_conf.addr;
	}
	msg.ext_params.ep = dest_eid; //destination

	msg.hdr.cmd = MCTP_CTRL_CMD_SET_ENDPOINT_ID;
	msg.hdr.rq = MCTP_REQUEST;
	msg.cmd_data = (uint8_t *)&req;
	msg.cmd_data_len = sizeof(req);

	ret = mctp_ctrl_read(mctp_inst, &msg, (uint8_t *)resp, sizeof(*resp));
	if (ret) {
		LOG_ERR("Set endpoint id failed.");
	} else {
		LOG_DBG("Set eid: %d", resp->eid);
	}

	return ret;
}

bool alloc_eid_ctrl_cmd(mctp *mctp_inst, uint8_t dest_eid, uint8_t operation,
	uint8_t start_eid, uint8_t alloc_pool_size, struct _alocate_ep_id_resp *resp)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(resp, MCTP_ERROR);

	uint8_t ret = MCTP_ERROR;
	mctp_medium_conf *conf = &mctp_inst->medium_conf;

	struct _alocate_ep_id_req req = { 0 };
	req.op_flag = operation;
	req.num_of_eid = alloc_pool_size;
	req.starting_eid = start_eid;

	mctp_ctrl_msg msg = { 0 };
	msg.ext_params.type = mctp_inst->medium_type;

	if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_USB) {
		msg.ext_params.usb_ext_params.dummy = conf->usb_conf.addr;
	} else if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
		msg.ext_params.smbus_ext_params.addr = conf->smbus_conf.addr;
	} else {
		msg.ext_params.i3c_ext_params.addr = conf->i3c_conf.addr;
	}
	msg.ext_params.ep = dest_eid; //destination

	msg.hdr.cmd = MCTP_CTRL_CMD_ALLOCATE_EP_ID;
	msg.hdr.rq = MCTP_REQUEST;
	msg.cmd_data = (uint8_t *)&req;
	msg.cmd_data_len = sizeof(req);

	ret = mctp_ctrl_read(mctp_inst, &msg, (uint8_t *)resp, sizeof(*resp));
	if (ret) {
		LOG_ERR("Allocate endpoint id failed.");
	} else {
		if (resp->status == allocation_accepted) {
			LOG_DBG("Allocate start_eid %d, pool_size %d.", resp->fisrt_eid, resp->eid_pool_size);
		}
	}

	return ret;
}

static void get_mctp_type_support_resp_handler(void *args, uint8_t *buf, uint16_t len)
{
	ARG_UNUSED(args);
	CHECK_NULL_ARG(buf);

	if (buf[0] == MCTP_CTRL_CC_SUCCESS) {
		struct _get_message_type_resp *resp = (struct _get_message_type_resp *)buf;
		//TODO: support get MCTP type support
		LOG_DBG("Get mctp type support count: %d", resp->type_count);
		for (int i = 0; i < resp->type_count; i++) {
			LOG_DBG("Get mctp type support entry[%d]: %d", i, resp->type_number[i]);
		}
	}
}

bool get_mctp_type_support_ctrl_cmd(mctp *mctp_inst, uint8_t dest_eid)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);

	mctp_medium_conf *conf = &mctp_inst->medium_conf;

	mctp_ctrl_msg msg = { 0 };
	msg.ext_params.type = mctp_inst->medium_type;

	if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_USB) {
		msg.ext_params.usb_ext_params.dummy = conf->usb_conf.addr;
	} else if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
		msg.ext_params.smbus_ext_params.addr = conf->smbus_conf.addr;
	} else {
		msg.ext_params.i3c_ext_params.addr = conf->i3c_conf.addr;
	}
	msg.ext_params.ep = dest_eid; //destination

	msg.hdr.cmd = MCTP_CTRL_CMD_GET_MESSAGE_TYPE_SUPPORT;
	msg.hdr.rq = MCTP_REQUEST;
	msg.cmd_data_len = 0;

	msg.recv_resp_cb_fn = get_mctp_type_support_resp_handler;

	uint8_t ret = mctp_ctrl_send_msg(mctp_inst, &msg);
	if (ret) {
		LOG_ERR("Get MCTP type support failed.");
	}

	return ret;
}

bool add_routing_table_entries(mctp *mctp_inst, uint8_t eid, uint8_t endpoint_type)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);
	CHECK_ARG_WITH_RETURN(eid == MCTP_NULL_EID, MCTP_ERROR);

	size_t idx;
	size_t new_size;
	void *tmp = NULL;
	mctp_route_entry *rt_entry;
	struct _get_routing_tbl_entry *rt_info;
	mctp_medium_conf *conf = &mctp_inst->medium_conf;

	// Find a slot
	for (idx = 0; idx < plat_mctp_route_tbl_size; idx++) {
		if (plat_mctp_route_tbl[idx].state == UNUSED) {
			break;
		}
	}
	if (idx == plat_mctp_route_tbl_size) {
		// Allocate more entries
		new_size = max(MCTP_DEFAULT_ROUTE_TBL_SIZE, plat_mctp_route_tbl_size*2);
		tmp = realloc(plat_mctp_route_tbl, new_size * sizeof(*plat_mctp_route_tbl));
		if (!tmp) {
			return MCTP_ERROR;
		}
		plat_mctp_route_tbl = tmp;
		// Zero the new entries
		memset(&plat_mctp_route_tbl[plat_mctp_route_tbl_size], 0x0, sizeof(*plat_mctp_route_tbl) * (new_size - plat_mctp_route_tbl_size));
		plat_mctp_route_tbl_size = new_size;
	}

	// Populate it
	rt_entry = &plat_mctp_route_tbl[idx];
	rt_info = &rt_entry->routing_tbl_entries.routing_info;

	rt_entry->endpoint = eid;
	rt_info->eid_range_size = 1;
	rt_info->starting_eid = eid;
	rt_info->entry_type = endpoint_type; //TODO: support entry type

	if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_USB) {
		rt_entry->bus = conf->usb_conf.bus;
		rt_entry->addr = conf->usb_conf.addr;
		rt_info->phys_transport_binding_id = mctp_over_usb;
		rt_info->phys_media_type_id = usb_2_0_compatible;
		rt_info->phys_address_size = 1; //USB phys addr is 2 bytes
		memcpy(rt_entry->routing_tbl_entries.phys_address, &conf->usb_conf.addr, rt_info->phys_address_size);
	} else if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
		rt_entry->bus = conf->smbus_conf.bus;
		rt_entry->addr = conf->smbus_conf.addr;
		rt_info->phys_transport_binding_id = mctp_over_smbus;
		rt_info->phys_media_type_id = smbus_2_0_or_i2c_100_khz_compatible;
		rt_info->phys_address_size = 1; //SMBus phys addr is 1 byte
		memcpy(rt_entry->routing_tbl_entries.phys_address, &conf->smbus_conf.addr, rt_info->phys_address_size);
	} else {
		rt_entry->bus = conf->i3c_conf.bus;
		rt_entry->addr = conf->i3c_conf.addr;
		rt_info->phys_transport_binding_id = mctp_over_i3c;
		rt_info->phys_media_type_id = i3c_basic_compatible;
		rt_info->phys_address_size = 1; //I3C phys addr is 1 byte
		memcpy(rt_entry->routing_tbl_entries.phys_address, &conf->i3c_conf.addr, rt_info->phys_address_size);
	}

	rt_entry->state = REMOTE;

	return MCTP_SUCCESS;
}

uint8_t register_endpoint(mctp *mctp_inst, uint8_t eid, uint8_t endpoint_type)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);

	for (uint8_t i = 0; i < ARRAY_SIZE(plat_mctp_port); i++) {
		mctp_port *p = plat_mctp_port + i;
		if (p->mctp_inst == mctp_inst) {
			mctp_eid_pool_alloc_info *eid_pool_info = &p->mctp_inst->eid_pool_alloc_info;
			/* Check if it is as a bus owner role and eid_pool_alloc_info.size is not zero */
			if (p->bus_owner && eid_pool_info->size) {
				/* Get MCTP version support */
				if (get_mctp_ver_support_ctrl_cmd(mctp_inst, MCTP_NULL_EID, MCTP_MSG_TYPE_CTRL)) {
					return MCTP_ERROR;
				}

				/* Get endpoint id */
				struct _get_eid_resp get_eid_resp = { 0 };
				if (get_eid_ctrl_cmd(mctp_inst, MCTP_NULL_EID, &get_eid_resp)) {
					return MCTP_ERROR;
				}
				uint8_t assign_eid = get_eid_resp.eid;

				/* Set endpoint id */
				struct _set_eid_resp set_eid_resp = { 0 };
				if (assign_eid != MCTP_NULL_EID && assign_eid >= eid_pool_info->start &&
					assign_eid <= (eid_pool_info->start + eid_pool_info->size) && !eid_pool_info->eid_used[assign_eid]) {
					if (!set_eid_ctrl_cmd(mctp_inst, MCTP_NULL_EID, set_eid, assign_eid, &set_eid_resp)) {
						eid_pool_info->eid_used[assign_eid] = true;
					}
				} else {
					for (assign_eid = eid_pool_info->start; assign_eid < (eid_pool_info->start + eid_pool_info->size); assign_eid++) {
						if (!eid_pool_info->eid_used[assign_eid]) {
							if (!set_eid_ctrl_cmd(mctp_inst, MCTP_NULL_EID, set_eid, assign_eid, &set_eid_resp)) {
								eid_pool_info->eid_used[assign_eid] = true;
								break;
							}
						}
					}
				}

				/* Check endpoint ID allocation status and EID pool size requirement via set_eid_resp */
				if (set_eid_resp.eid_alloc_status == EP_REQ_EID_POOL_ALLOCATION && set_eid_resp.eid_pool_size <= eid_pool_info->size) {
					/* Find unused eid from eid pool and requirement size <= eid pool unused size */
					uint8_t pool_last_eid = eid_pool_info->start + eid_pool_info->size - 1;
					for (uint8_t start_eid = eid_pool_info->start; start_eid <= pool_last_eid; start_eid++) {
						uint8_t pool_remain_size = pool_last_eid - start_eid + 1;
						if (!eid_pool_info->eid_used[start_eid] && set_eid_resp.eid_pool_size <= pool_remain_size) {
							struct _alocate_ep_id_resp alloc_eid_resp = { 0 };
							if (!alloc_eid_ctrl_cmd(mctp_inst, assign_eid, allocate_eids, start_eid, set_eid_resp.eid_pool_size, &alloc_eid_resp)) {
								if (alloc_eid_resp.status == allocation_accepted) {
									uint8_t count = 0;
									while (count < set_eid_resp.eid_pool_size) {
										/* Update eid status */
										eid_pool_info->eid_used[start_eid + count] = true;
										count++;
									}
								}
							}
							break;
						}
					}
				}

				/* Get UUID */
				struct _get_uuid_resp get_uuid_resp = { 0 };
				get_uuid_ctrl_cmd(mctp_inst, assign_eid, &get_uuid_resp);

				/* Get Message Type Support */
				get_mctp_type_support_ctrl_cmd(mctp_inst, assign_eid);

				if (add_routing_table_entries(mctp_inst, assign_eid, get_eid_resp.endpoint_type)) {
					return MCTP_ERROR;
				}

				/* If eid is bridge, */
				if (get_eid_resp.endpoint_type == BUS_OWNER_BRIDGE) {
					/* Routing info update (0x09) */
				}

			} else if (!p->bus_owner) { /* as an endpoint role */
				/* Get Message Type Support */
				if (get_mctp_type_support_ctrl_cmd(mctp_inst, eid)) {
					return MCTP_ERROR;
				}

				/* Check if EID is already registered */
#if SUPPORT_DYNAMIC_MCTP_ROUTE_TBL
				for (uint8_t j = 0; j < plat_mctp_route_tbl_size; j++) {
#else
				for (uint8_t j = 0; j < ARRAY_SIZE(plat_mctp_route_tbl); j++) {
#endif
					mctp_route_entry *r = plat_mctp_route_tbl + j;
					if (r->endpoint == eid) {
						LOG_DBG("Endpoint %d is already registered", eid);
						return MCTP_ERROR;
					}
				}

				/* Get UUID */
				struct _get_uuid_resp get_uuid_resp = { 0 };
				get_uuid_ctrl_cmd(mctp_inst, eid, &get_uuid_resp);

				if (add_routing_table_entries(mctp_inst, eid, endpoint_type)) {
					return MCTP_ERROR;
				}

				/* If eid is bridge, */
				if (endpoint_type == BUS_OWNER_BRIDGE) {
					/* Routing info update (0x09) */
				}

			}
		}
	}

	return MCTP_SUCCESS;
}

bool discovery_notify_ctrl_cmd(mctp *mctp_inst)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);

	uint8_t ret = MCTP_ERROR;
	mctp_medium_conf *conf = &mctp_inst->medium_conf;
	mctp_ctrl_msg msg = { 0 };

	msg.ext_params.type = mctp_inst->medium_type;

	if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_USB) {
		msg.ext_params.usb_ext_params.dummy = conf->usb_conf.addr;
	} else if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
		msg.ext_params.smbus_ext_params.addr = conf->smbus_conf.addr;
	} else {
		msg.ext_params.i3c_ext_params.addr = conf->i3c_conf.addr;
	}
	msg.ext_params.ep = MCTP_NULL_EID; //destination

	/* Send discovery notify ctrl cmd */
	msg.hdr.cmd = MCTP_CTRL_CMD_ENDPOINT_DISCOVERY_NOTIFY;
	msg.hdr.rq = MCTP_REQUEST;
	msg.cmd_data_len = 0;

	struct _mctp_ctrl_resp discovery_notify_resp = { 0 };
	ret = mctp_ctrl_read(mctp_inst, &msg, (uint8_t *)&discovery_notify_resp, sizeof(discovery_notify_resp));
	if (ret) {
		LOG_ERR("Discovery notify failed.");
	}

	return ret;
}

void send_discovery_notify_cmd_handler(struct k_work *work)
{
	//mctp *mctp_inst = (mctp *)k_timer_user_data_get(&send_discovery_notify_cmd_timer);

	/* Send discovery notify cmd */
	for (uint8_t i = 0; i < ARRAY_SIZE(plat_mctp_port); i++) {
		mctp_port *p = plat_mctp_port + i;
		//if (p->mctp_inst == mctp_inst) {
			if (!p->bus_owner && !p->mctp_inst->discovered) {
				discovery_notify_ctrl_cmd(p->mctp_inst);
			}
		//}
	}
}

void send_discovery_notify_cmd(struct k_timer *timer)
{
	if (k_work_busy_get(&send_discovery_notify_cmd_work)) {
		LOG_INF("Work is busy, cancelling and resubmitting.");
		k_work_cancel(&send_discovery_notify_cmd_work);
		k_work_submit(&send_discovery_notify_cmd_work);
	} else {
		LOG_INF("Work is not busy, submitting.");
		k_work_submit(&send_discovery_notify_cmd_work);
	}
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

		if (p->bus_owner) {
			mctp_eid_pool_alloc_info *eid_pool_info = &p->mctp_inst->eid_pool_alloc_info;
			eid_pool_info->size = p->eid_pool_size;
			eid_pool_info->start = p->eid_pool_first_eid;
			eid_pool_info->allocated = true;
		} else {
			//TODO: Use k_work to replace k_timer_start(&send_discovery_notify_cmd_timer, K_MSEC(3000), K_NO_WAIT);
			LOG_INF("Send discovery notify cmd by k_work with mctp_inst and dest_eid");
		}
	}

	plat_mctp_route_tbl = calloc(MCTP_DEFAULT_ROUTE_TBL_SIZE, sizeof(mctp_route_entry));
	if (!plat_mctp_route_tbl) {
		LOG_ERR("Failed to allocate memory for routing table");
		return;
	}

	for (uint8_t j = 0; j < MAX_WORK_ITEMS; j++) {
		k_work_init(&reg_eid_work[j].work, mctp_reg_eid_handler);
	}

	k_timer_start(&send_discovery_notify_cmd_timer, K_MSEC(3000), K_NO_WAIT);
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

uint8_t plat_get_mctp_route_tbl_count()
{
#if SUPPORT_DYNAMIC_MCTP_ROUTE_TBL
	return plat_mctp_route_tbl_size;
#else
	return ARRAY_SIZE(plat_mctp_route_tbl);
#endif
}

mctp_route_entry *plat_get_mctp_route_tbl(uint8_t index)
{
	return plat_mctp_route_tbl + index;
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
