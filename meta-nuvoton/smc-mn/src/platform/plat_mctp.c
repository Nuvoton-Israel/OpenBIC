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
		.eid_pool_size = 2,
		.eid_pool_first_eid = 0x08,
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

#if 0
// TDOD: allocate memory for mctp route table and reallocate it when more entries are needed
static mctp_route_entry* plat_mctp_route_tbl = NULL;
//plat_mctp_route_tbl[] needs to add struct _get_routing_tbl_entry_with_address!!!
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

	for (i = 0; i < ARRAY_SIZE(plat_mctp_route_tbl); i++) {
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

void mctp_reg_eid_handler(struct k_work *work)
{
	mctp_reg_eid_work *reg_eid_work_p = CONTAINER_OF(work, mctp_reg_eid_work, work);

	register_endpoint(reg_eid_work_p->mctp_inst, reg_eid_work_p->eid);
}

bool get_mctp_ver_support_ctrl_cmd(mctp *mctp_inst, uint8_t dest_eid, uint8_t msg_type_no,
								   struct _get_mctp_ver_support_resp *resp)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(resp, MCTP_ERROR);

	uint8_t ret = MCTP_ERROR;
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

	ret = mctp_ctrl_read(mctp_inst, &msg, (uint8_t *)resp, sizeof(*resp));
	if (ret) {
		LOG_ERR("Get mctp version support failed.");
	} else {
		for (int i = 1; i <= (resp->ver_num_entry_count); i++) {
			//TODO: check the version support
			size_t ver_no_entry_offset = sizeof(struct _mctp_ver_fields)*(i - 1);
			struct _mctp_ver_fields *entry = (struct _mctp_ver_fields *)(resp +
											  sizeof(struct _get_mctp_ver_support_resp) + ver_no_entry_offset);
			LOG_DBG("Get mctp version support entry[%d]={%d, %d, %d, %d}",
					 i, entry->major, entry->minor, entry->update, entry->alpha);
		}
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
		LOG_HEXDUMP_DBG(resp->uuid, sizeof(resp->uuid), "Get uuid");
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

bool get_mctp_type_support_ctrl_cmd(mctp *mctp_inst, uint8_t dest_eid, struct _get_message_type_resp *resp)
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

	msg.hdr.cmd = MCTP_CTRL_CMD_GET_MESSAGE_TYPE_SUPPORT;
	msg.hdr.rq = MCTP_REQUEST;
	msg.cmd_data_len = 0;

	ret = mctp_ctrl_read(mctp_inst, &msg, (uint8_t *)resp, sizeof(*resp));
	if (ret) {
		LOG_ERR("Get mctp type support failed.");
	} else {
		//TODO: according to type_count to print type_number of _get_message_type_resp
		LOG_DBG("Get mctp type support count: %d", resp->type_count);
		for (int i = 0; i < resp->type_count; i++) {
			LOG_DBG("Get mctp type support entry[%d]: %d", i, resp->type_number[i]);
		}
	}

	return ret;
}

uint8_t register_endpoint(mctp *mctp_inst, uint8_t eid)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);

	for (uint8_t i = 0; i < ARRAY_SIZE(plat_mctp_port); i++) {
		mctp_port *p = plat_mctp_port + i;
		if (p->mctp_inst == mctp_inst) {
			mctp_eid_pool_alloc_info *eid_pool_info = &p->mctp_inst->eid_pool_alloc_info;
			/* Check if it is as a bus owner role and eid_pool_alloc_info.size is not zero */
			if (p->bus_owner && eid_pool_info->size) {
				/* Get MCTP version support */
				struct _get_mctp_ver_support_resp get_mctp_ctl_ver_resp = { 0 };
				get_mctp_ver_support_ctrl_cmd(mctp_inst, MCTP_NULL_EID, MCTP_MSG_TYPE_CTRL, &get_mctp_ctl_ver_resp);

				/* Get endpoint id */
				struct _get_eid_resp get_eid_resp = { 0 };
				if (get_eid_ctrl_cmd(mctp_inst, MCTP_NULL_EID, &get_eid_resp)) {
					return MCTP_ERROR;
				}
				uint8_t alloc_eid = get_eid_resp.eid;

				/* Set endpoint id */
				struct _set_eid_resp set_eid_resp = { 0 };
				if (alloc_eid != MCTP_NULL_EID && alloc_eid >= eid_pool_info->start &&
					alloc_eid <= (eid_pool_info->start + eid_pool_info->size) && !eid_pool_info->eid_used[alloc_eid]) {
					if (!set_eid_ctrl_cmd(mctp_inst, MCTP_NULL_EID, set_eid, alloc_eid, &set_eid_resp)) {
						eid_pool_info->eid_used[alloc_eid] = true;
					}
				} else {
					for (alloc_eid = eid_pool_info->start; alloc_eid < (eid_pool_info->start + eid_pool_info->size); alloc_eid++) {
						if (!eid_pool_info->eid_used[alloc_eid]) {
							if (!set_eid_ctrl_cmd(mctp_inst, MCTP_NULL_EID, set_eid, alloc_eid, &set_eid_resp)) {
								eid_pool_info->eid_used[alloc_eid] = true;
								break;
							}
						}
					}
				}

				//TODO: Trigger allocate eid ctrl cmd if endpoint requires EID pool allocation
				//struct _alloc_eid_resp alloc_eid_resp = { 0 };
				//alloc_eid_ctrl_cmd(mctp_inst, alloc_eid, alloc_eid_resp);

				/* Get UUID */
				struct _get_uuid_resp get_uuid_resp = { 0 };
				get_uuid_ctrl_cmd(mctp_inst, alloc_eid, &get_uuid_resp);

				/* Get Message Type Support */
				struct _get_message_type_resp get_mctp_type_resp = { 0 };
				get_mctp_type_support_ctrl_cmd(mctp_inst, alloc_eid, &get_mctp_type_resp);

				//add_routing_table_entry(mctp_inst, alloc_eid);

				/* If eid is bridge, */
					/* Routing info update (0x09) */

				uint8_t bus = 0;
				uint8_t addr = 0;
				if (p->medium_type == MCTP_MEDIUM_TYPE_USB) {
					bus = p->conf.usb_conf.bus;
					addr = p->conf.usb_conf.addr;
				} else if (p->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
					bus = p->conf.smbus_conf.bus;
					addr = p->conf.smbus_conf.addr;
				} else {
					bus = p->conf.i3c_conf.bus;
					addr = p->conf.i3c_conf.addr;
				}

				for (uint8_t j = 0; j < ARRAY_SIZE(plat_mctp_route_tbl); j++) {
					mctp_route_entry *r = plat_mctp_route_tbl + j;
					// Check bus & addr are match and then update the endpoint id
					if (r->bus == bus && r->addr == addr) {
						r->endpoint = alloc_eid;
						break;
					}
				}
			} else if (!p->bus_owner) { /* as an endpoint role */
				/* Get Message Type Support */
				struct _get_message_type_resp get_mctp_type_resp = { 0 };
				if (get_mctp_type_support_ctrl_cmd(mctp_inst, eid, &get_mctp_type_resp)) {
					return MCTP_ERROR;
				}

				/* Check if EID is already registered */
				for (uint8_t j = 0; j < ARRAY_SIZE(plat_mctp_route_tbl); j++) {
					mctp_route_entry *r = plat_mctp_route_tbl + j;
					if (r->endpoint == eid) {
						LOG_DBG("Endpoint %d is already registered", eid);
						return MCTP_ERROR;
					}
				}

				/* Get UUID */
				struct _get_uuid_resp get_uuid_resp = { 0 };
				get_uuid_ctrl_cmd(mctp_inst, eid, &get_uuid_resp);

				//add_routing_table_entry(mctp_inst, alloc_eid);

				/* If eid is bridge, */
					/* Routing info update (0x09) */

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
	return ARRAY_SIZE(plat_mctp_route_tbl);
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
