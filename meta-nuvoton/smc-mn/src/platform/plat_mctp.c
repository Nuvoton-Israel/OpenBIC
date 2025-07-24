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

K_TIMER_DEFINE(get_routing_tbl_entries_timer, get_routing_tbl_entries_timer_handler, NULL);
K_WORK_DEFINE(get_routing_tbl_entries_work, get_routing_tbl_entries_work_handler);

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
#if 1
	{ /* I3C controller & MCTP EP */
		.conf.i3c_conf.addr = I3C_MNG_ADDR,
		.conf.i3c_conf.bus = I3C_BUS_CONTROLLER_TO_HUB,
		.medium_type = MCTP_MEDIUM_TYPE_CONTROLLER_I3C,
		.support_bridge = true,
		.bus_owner = false,
		.eid_pool_size = 0,
		.eid_pool_first_eid = 0,
		.required_eid_pool_from_BO = 3,
	},
#else
	{ /* I3C target & MCTP topmost BO */
		.conf.i3c_conf.addr = I3C_MNG_ADDR,
		.conf.i3c_conf.bus = I3C_BUS_CONTROLLER_TO_HUB,
		.medium_type = MCTP_MEDIUM_TYPE_TARGET_I3C,
		.support_bridge = true,
		.bus_owner = true,
		.eid_pool_size = 10,
		.eid_pool_first_eid = 0x11,
		.required_eid_pool_from_BO = 0,
	},
#endif
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
		.eid_pool_size = 0,
		.eid_pool_first_eid = 0,
		.required_eid_pool_from_BO = 3,
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
	uint8_t ret = MCTP_ERROR;

	ret = register_endpoint(reg_eid_work_p->mctp_inst, &reg_eid_work_p->routing_tbl_entry);
	if (ret && (ret != MCTP_ERROR)) {
		LOG_DBG("Register endpoint success, eid: %d", reg_eid_work_p->routing_tbl_entry.routing_info.starting_eid);
	} else {
		LOG_ERR("Register endpoint failed, ret: %d", ret);
	}
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

bool send_routing_info_update(mctp *mctp_inst, uint8_t dest_eid, struct _routing_info_update_req *req, size_t req_data_len)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);
	CHECK_ARG_WITH_RETURN(dest_eid == MCTP_NULL_EID, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(req, MCTP_ERROR);

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

	msg.hdr.cmd = MCTP_CTRL_CMD_ROUTING_INFO_UPDATE;
	msg.hdr.rq = MCTP_REQUEST;
	msg.cmd_data = (uint8_t *)req;
	msg.cmd_data_len = req_data_len;

	struct _mctp_ctrl_resp routing_info_update_resp = { 0 };
	ret = mctp_ctrl_read(mctp_inst, &msg, (uint8_t *)&routing_info_update_resp, sizeof(routing_info_update_resp));
	if (ret) {
		LOG_ERR("Send routing info update failed.");
	}

	return ret;
}

bool send_routing_tbl_etries_to_bridge(mctp *mctp_inst, uint8_t dest_eid)
{
	CHECK_ARG_WITH_RETURN(dest_eid == MCTP_NULL_EID, MCTP_ERROR);

	mctp_medium_conf *conf = &mctp_inst->medium_conf;
	struct _routing_info_update_req *req = NULL;
	struct routing_info_update_entry *cur_entry = NULL;
	mctp_route_entry *rt_entry = NULL;
	struct _get_routing_tbl_entry *rt_info = NULL;
	uint8_t num_of_entries = 0;
	uint8_t idx;
	uint8_t ret = MCTP_ERROR;
	size_t entry_offset = sizeof(struct _routing_info_update_req);

	/* Get ALL routing table entries */
#if SUPPORT_DYNAMIC_MCTP_ROUTE_TBL
	for (idx = 0; idx < plat_mctp_route_tbl_size; idx++)
#else
	for (idx = 0; idx < ARRAY_SIZE(plat_mctp_route_tbl); idx++)
#endif
	{
		rt_entry = &plat_mctp_route_tbl[idx];
		if (rt_entry->state != UNUSED) {
			num_of_entries++;
		} else {
			break;
		}
	}

	if (num_of_entries != 0) {
		/* Allocate routing info update request */
		req = calloc(1, sizeof(struct _routing_info_update_req) + (num_of_entries + 1) * sizeof(struct _get_routing_tbl_entry_with_address));
		req->count = num_of_entries + 1; //Add own eid entry

		cur_entry = (struct routing_info_update_entry *)((uint8_t *)req + entry_offset);
		entry_offset += sizeof(struct routing_info_update_entry);

		/* Add own eid first */
		cur_entry->type = MCTP_ROUTING_ENTRY_BRIDGE;
		cur_entry->eid_count = 1;
		cur_entry->starting_eid = mctp_inst->endpoint;

		//Own eid phys addr is 1 byte
		if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_USB) {
			memcpy(cur_entry->address, &conf->usb_conf.addr, 1);
		} else if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
			memcpy(cur_entry->address, &conf->smbus_conf.addr, 1);
		} else {
			memcpy(cur_entry->address, &conf->i3c_conf.addr, 1);
		}
		entry_offset += 1; //Own eid phys addr is 1 byte

		for (idx = 0; idx < num_of_entries; idx++) {
			cur_entry = (struct routing_info_update_entry *)((uint8_t *)req + entry_offset);
			entry_offset += sizeof(struct routing_info_update_entry);

			rt_entry = &plat_mctp_route_tbl[idx];
			rt_info = &rt_entry->routing_tbl_entries.routing_info;

			cur_entry->type = rt_info->entry_type;
			cur_entry->eid_count = rt_info->eid_range_size;
			cur_entry->starting_eid = rt_info->starting_eid;
			memcpy(cur_entry->address, rt_entry->routing_tbl_entries.phys_address, rt_info->phys_address_size);
			entry_offset += rt_info->phys_address_size;
		}

		/* Send routing info update (0x09) */
		ret = send_routing_info_update(mctp_inst, dest_eid, req, entry_offset);
		if (ret) {
			LOG_ERR("Send routing info update failed.");
		} else {
			LOG_DBG("Send routing info update success.");
		}
		free(req);

	}

	return ret;
}

bool add_routing_table_entries(mctp *mctp_inst, struct _get_routing_tbl_entry_with_address *routing_tbl_entry)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(routing_tbl_entry, MCTP_ERROR);

	size_t idx;
	size_t new_size;
	void *tmp = NULL;
	mctp_route_entry *rt_entry;
	struct _get_routing_tbl_entry *rt_info;
	mctp_medium_conf *conf = &mctp_inst->medium_conf;

	// Find a slot
#if SUPPORT_DYNAMIC_MCTP_ROUTE_TBL
	for (idx = 0; idx < plat_mctp_route_tbl_size; idx++)
#else
	for (idx = 0; idx < ARRAY_SIZE(plat_mctp_route_tbl); idx++)
#endif
	{
		/* Check if EID is already registered */
		if (plat_mctp_route_tbl[idx].endpoint == routing_tbl_entry->routing_info.starting_eid) {
			LOG_WRN("EID 0x%02x already registered, udate it.", routing_tbl_entry->routing_info.starting_eid);
			break;
		} else if (plat_mctp_route_tbl[idx].state == UNUSED) {
			break;
		}
	}

#if SUPPORT_DYNAMIC_MCTP_ROUTE_TBL
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
#else
	if (idx == ARRAY_SIZE(plat_mctp_route_tbl)) {
		LOG_ERR("No more routing table entries available.");
		return MCTP_ERROR;
	}
#endif

	// Populate it
	rt_entry = &plat_mctp_route_tbl[idx];
	rt_entry->endpoint = routing_tbl_entry->routing_info.starting_eid;

	rt_info = &rt_entry->routing_tbl_entries.routing_info;
	memcpy(rt_info, &routing_tbl_entry->routing_info, sizeof(*rt_info));

	if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_USB) {
		rt_entry->bus = conf->usb_conf.bus;
		rt_entry->addr = conf->usb_conf.addr;

		if (rt_info->phys_address_size == 0) {
			rt_info->phys_transport_binding_id = mctp_over_usb;
			rt_info->phys_media_type_id = usb_2_0_compatible;
			rt_info->phys_address_size = 1; //USB phys addr is 2 bytes
			memcpy(rt_entry->routing_tbl_entries.phys_address, &conf->usb_conf.addr, rt_info->phys_address_size);
		}
	} else if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
		rt_entry->bus = conf->smbus_conf.bus;
		rt_entry->addr = conf->smbus_conf.addr;

		if (rt_info->phys_address_size == 0) {
			rt_info->phys_transport_binding_id = mctp_over_smbus;
			rt_info->phys_media_type_id = smbus_2_0_or_i2c_100_khz_compatible;
			rt_info->phys_address_size = 1; //SMBus phys addr is 1 byte
			memcpy(rt_entry->routing_tbl_entries.phys_address, &conf->smbus_conf.addr, rt_info->phys_address_size);
		}
	} else {
		rt_entry->bus = conf->i3c_conf.bus;
		rt_entry->addr = conf->i3c_conf.addr;

		if (rt_info->phys_address_size == 0) {
			rt_info->phys_transport_binding_id = mctp_over_i3c;
			rt_info->phys_media_type_id = i3c_basic_compatible;
			rt_info->phys_address_size = 1; //I3C phys addr is 1 byte
			memcpy(rt_entry->routing_tbl_entries.phys_address, &conf->i3c_conf.addr, rt_info->phys_address_size);
		}
	}

	rt_entry->state = REMOTE;

	return MCTP_SUCCESS;
}

uint8_t register_endpoint(mctp *mctp_inst, struct _get_routing_tbl_entry_with_address *routing_tbl_entry)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(routing_tbl_entry, MCTP_ERROR);

	struct _get_routing_tbl_entry *rt_info = &routing_tbl_entry->routing_info;

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
									while (count < alloc_eid_resp.eid_pool_size) {
										/* Update eid status */
										eid_pool_info->eid_used[start_eid + count] = true;
										count++;
									}

									/* Update routing table entry */
									struct _get_routing_tbl_entry_with_address alloc_routing_tbl_entry = {0};
									struct _get_routing_tbl_entry *alloc_rt_info = &alloc_routing_tbl_entry.routing_info;

									/* Use eid_pool's start EID and size */
									alloc_rt_info->starting_eid = eid_pool_info->start;
									alloc_rt_info->eid_range_size = eid_pool_info->size;
									alloc_rt_info->entry_type = MCTP_ROUTING_ENTRY_ENDPOINT;

									if (add_routing_table_entries(mctp_inst, &alloc_routing_tbl_entry)) {
										LOG_ERR("Failed to add routing table entries for allocated EID.");
										return MCTP_ERROR;
									}

									/* Change assign_eid's entry_type to bridge */
									rt_info->entry_type = MCTP_ROUTING_ENTRY_BRIDGE;
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

				rt_info->starting_eid = assign_eid;
				if (add_routing_table_entries(mctp_inst, routing_tbl_entry)) {
					LOG_ERR("Failed to add routing table entries for assigned EID 0x%x.", assign_eid);
					return MCTP_ERROR;
				}

				/* If eid is bridge, send routing table entries to bridge */
				if (get_eid_resp.endpoint_type == BUS_OWNER_BRIDGE) {
					/* Routing info update (0x09) */
					send_routing_tbl_etries_to_bridge(mctp_inst, assign_eid);
				}

			} else if (!p->bus_owner) {	/* Entry's EID is an endpoint or a bridge */
				/* Get Message Type Support */
				if (get_mctp_type_support_ctrl_cmd(mctp_inst, routing_tbl_entry->routing_info.starting_eid)) {
					return MCTP_ERROR;
				}

				/* Get UUID */
				struct _get_uuid_resp get_uuid_resp = { 0 };
				get_uuid_ctrl_cmd(mctp_inst, routing_tbl_entry->routing_info.starting_eid, &get_uuid_resp);

				if (add_routing_table_entries(mctp_inst, routing_tbl_entry)) {
					LOG_ERR("Failed to add routing table entries for endpoint 0x%02x", routing_tbl_entry->routing_info.starting_eid);
					return MCTP_ERROR;
				}
			} else { //p->bus_owner's eid pool is not ready yet
				return MCTP_ERROR;
			}

			return routing_tbl_entry->routing_info.starting_eid;
		}
	}

	return MCTP_ERROR;
}

static void get_routing_tbl_entries_resp_handler(void *args, uint8_t *read_buf, uint16_t read_len)
{
	CHECK_NULL_ARG(args);
	CHECK_NULL_ARG(read_buf);

	uint8_t status = 0;
	mctp_ctrl_resp_arg *resp_arg = (mctp_ctrl_resp_arg *)args;
	/* Store the pointer-to-pointer in read_buf to handle resp of variable length */
	struct _get_routing_tbl_entry_resp **resp_ptr = (struct _get_routing_tbl_entry_resp **)resp_arg->read_buf;

	if (read_len > resp_arg->read_len) {
		LOG_WRN("Response length(%d) is greater than buffer length(%d)", read_len,
			resp_arg->read_len);

		/* Reallocate resp */
		if (*resp_ptr != NULL) {
			void *tmp = realloc(*resp_ptr, read_len);

			if (!tmp) {
				return;
			}
			*resp_ptr = tmp;

			/* Zero the new entries */
			memset((uint8_t *)(*resp_ptr) + resp_arg->read_len, 0x0, read_len - resp_arg->read_len);

			/* Update read_buf length */
			resp_arg->read_len = read_len;
		}
		resp_arg->return_len = resp_arg->read_len;
	} else {
		resp_arg->return_len = read_len;
	}

	/* Return first data is completion code */
	if (read_buf[0] != MCTP_CTRL_CC_SUCCESS) {
		LOG_ERR("Return code status(0x%x)", read_buf[0]);
		status = MCTP_CTRL_READ_STATUS_CC_ERROR;
	} else {
		memcpy(*resp_ptr, read_buf, resp_arg->return_len);
		status = MCTP_CTRL_READ_STATUS_SUCCESS;
	}

	k_msgq_put(resp_arg->msgq, &status, K_NO_WAIT);
}

bool get_routing_tbl_entries_ctrl_cmd(mctp *mctp_inst, uint8_t dest_eid)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);
	CHECK_ARG_WITH_RETURN(dest_eid == MCTP_NULL_EID, MCTP_ERROR);

	mctp_medium_conf *conf = &mctp_inst->medium_conf;
	struct _get_routing_tbl_entry_req req = { 0 };
	struct _get_routing_tbl_entry_resp *resp = NULL;
	struct _get_routing_tbl_entry_with_address *resp_rt_entry = NULL;
	uint8_t entry_handle = 0x00; //0x00 to access first entries in table
	uint8_t ret = MCTP_ERROR;
	size_t entry_offset = sizeof(struct _get_routing_tbl_entry_resp);

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

	uint8_t status = 0;
	uint8_t status_msgq_buf[1];
	struct k_msgq *status_msgq = (struct k_msgq *)malloc(sizeof(struct k_msgq));
	if (status_msgq == NULL) {
		LOG_ERR("Fail to allocate status_msgq");
		return ret;
	}
	k_msgq_init(status_msgq, status_msgq_buf, sizeof(uint8_t), 1);

	mctp_ctrl_resp_arg *resp_arg = (mctp_ctrl_resp_arg *)malloc(sizeof(mctp_ctrl_resp_arg));
	if (resp_arg == NULL) {
		SAFE_FREE(status_msgq);
		LOG_ERR("Fail to allocate resp_arg");
		return ret;
	}

	resp = calloc(1, sizeof(struct _get_routing_tbl_entry_resp));
	if (!resp) {
		LOG_ERR("Failed to allocate memory for routing table");
		return MCTP_ERROR;
	}

	resp->completion_code = MCTP_CTRL_CC_ERROR;
	while (entry_handle != 0xFF) //0xFF = No more entries
	{
		req.entry_handle = entry_handle;

		/* Send the get routing table entries ctrl cmd */
		msg.hdr.cmd = MCTP_CTRL_CMD_GET_ROUTING_TABLE_ENTRIES;
		msg.hdr.rq = MCTP_REQUEST;
		msg.cmd_data = (uint8_t *)&req;
		msg.cmd_data_len = sizeof(req);

		resp_arg->msgq = status_msgq;
		resp_arg->read_buf = (uint8_t *)&resp; /* Store the pointer-to-pointer in read_buf */
		resp_arg->read_len = sizeof(*resp);
		resp_arg->return_len = 0;

		msg.recv_resp_cb_fn = get_routing_tbl_entries_resp_handler;
		msg.recv_resp_cb_args = (void *)resp_arg;

		if (mctp_ctrl_send_msg(mctp_inst, &msg) == MCTP_ERROR) {
			LOG_ERR("Fail to send ctrl msg");
			goto exit;
		}

		status = MCTP_CTRL_READ_STATUS_CC_ERROR;
		if (k_msgq_get(status_msgq, &status, K_FOREVER)) {
			LOG_ERR("Fail to get status from msgq");
			goto exit;
		}

		if (status == MCTP_CTRL_READ_STATUS_SUCCESS) {
			ret = MCTP_SUCCESS;
		} else {
			LOG_ERR("MCTP ctrl status: 0x%x", status);
			goto exit;
		}

		/* Add routing table entries */
		if (resp->completion_code == MCTP_CTRL_CC_SUCCESS) {
			for (uint8_t i = 0; i < resp->num_of_entries; i++) {
				resp_rt_entry = (struct _get_routing_tbl_entry_with_address *)((uint8_t *)resp + entry_offset);
				entry_offset += sizeof(struct _get_routing_tbl_entry) + resp_rt_entry->routing_info.phys_address_size;

				/* Check if starting_eid is own eid */
				if(resp_rt_entry->routing_info.starting_eid == plat_eid) {
					continue;
				}

				/* Check if starting_eid is downstream eid */
				bool downstream_eids_flag = false;
				for (uint8_t j = 0; j < ARRAY_SIZE(plat_mctp_port); j++) {
					mctp_port *p = plat_mctp_port + j;
					mctp_eid_pool_alloc_info *eid_pool_info = &p->mctp_inst->eid_pool_alloc_info;
					if (eid_pool_info->allocated &&
						resp_rt_entry->routing_info.starting_eid >= eid_pool_info->start &&
						resp_rt_entry->routing_info.starting_eid < (eid_pool_info->start + eid_pool_info->size)) {
						downstream_eids_flag = true;
						break;
					}
				}
				if (downstream_eids_flag) {
					LOG_DBG("Downstream EID 0x%02x, skip it.", resp_rt_entry->routing_info.starting_eid);
					continue;
				}

				/* Register endpoint */
				ret = register_endpoint(mctp_inst, resp_rt_entry);
				if (ret == MCTP_ERROR) {
					LOG_ERR("Failed to register endpoint for routing table entry.");
				}
			}

			entry_handle = resp->next_entry_handle;
		} else {
			LOG_ERR("Get routing tbl entries response is NULL.");
			ret = MCTP_ERROR;
			goto exit;
		}
	}

exit:
	SAFE_FREE(resp_arg);
	SAFE_FREE(status_msgq);
	SAFE_FREE(resp);
	return ret;
}

void get_routing_tbl_entries_work_handler(struct k_work *work)
{
	mctp *mctp_inst = (mctp *)k_timer_user_data_get(&get_routing_tbl_entries_timer);

	if (mctp_inst->discovered) {
		/* Get routing tbl entries ctrl cmd */
		get_routing_tbl_entries_ctrl_cmd(mctp_inst, mctp_inst->bus_owner_eid);
	}
}

void get_routing_tbl_entries_timer_handler(struct k_timer *timer)
{
	if (k_work_busy_get(&get_routing_tbl_entries_work)) {
		LOG_DBG("%s: Work is busy, cancelling and resubmitting.", __func__);
		k_work_cancel(&get_routing_tbl_entries_work);
		k_work_submit(&get_routing_tbl_entries_work);
	} else {
		LOG_DBG("%s: Work is not busy, submitting.", __func__);
		k_work_submit(&get_routing_tbl_entries_work);
	}
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
		LOG_DBG("%s: Work is busy, cancelling and resubmitting.", __func__);
		k_work_cancel(&send_discovery_notify_cmd_work);
		k_work_submit(&send_discovery_notify_cmd_work);
	} else {
		LOG_DBG("%s: Work is not busy, submitting.", __func__);
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
			LOG_DBG("Send discovery notify cmd by k_work with mctp_inst and dest_eid");
		}
	}

#if SUPPORT_DYNAMIC_MCTP_ROUTE_TBL
	plat_mctp_route_tbl = calloc(MCTP_DEFAULT_ROUTE_TBL_SIZE, sizeof(mctp_route_entry));
	if (!plat_mctp_route_tbl) {
		LOG_ERR("Failed to allocate memory for routing table");
		return;
	}
#endif

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
	for (uint8_t i = 0; i < ARRAY_SIZE(plat_mctp_port); i++) {
		mctp_port *p = plat_mctp_port + i;
		if (p->mctp_inst) {
			p->mctp_inst->endpoint = plat_eid;
		}
	}

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
