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

#ifndef _PLAT_MCTP_h
#define _PLAT_MCTP_h

#include <kernel.h>
#include "storage_handler.h"
#include "pldm.h"

#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))

#define MCTP_MSG_TYPE_SHIFT	0
#define MCTP_MSG_TYPE_MASK	0x7F
#define MCTP_IC_SHIFT		7
#define MCTP_IC_MASK		0x80

/* i2c 8 bit address */
#define I2C_ADDR_BIC		0x20
#define I2C_ADDR_BMC		0x22
/* use i2c0 */
#define I2C_BUS_PLDM I2C_BUS1
#define I2C_BUS_TARGET_TO_BMC	I2C_BUS1

/* mctp endpoint */
#define MCTP_EID_CXL_I2C	0x2E
#define MCTP_EID_BMC_I2C	0x15
#define MCTP_EID_BMC_I3C	0x08
#define MCTP_EID_BMC_SERIAL	0x08

#define MCTP_EID_BIC_I3C_WF	0x12
#define MCTP_EID_BIC_I3C_FF	0x13
#define MCTP_EID_MNG_I3C	0x1D

/* i3c static 8-bit address */
#define I3C_STATIC_ADDR_BIC_SD	0x20
#define I3C_STATIC_ADDR_BIC_WF	0x0A
#define I3C_STATIC_ADDR_BIC_FF	0x0B
#define I3C_STATIC_ADDR_BMC	0x20
#define I3C_STATIC_ADDR_HUB	0x70
//#define I3C_MNG_ADDR		0x9
#define I3C_MNG_ADDR		0xA

/* i3c dynamic 8-bit address */
#define I3C_DYNAMIC_ADDR_BIC	0xA

/* i3c dev bus */
/* Normal use bus, BMC (1.8v) <-> BIC (1.8v) */
#define I3C_BUS_TARGET_TO_BMC		5

/* Test use bus, BIC (3.3v) <-> BIC (3.3v) */
#define I3C_BUS_CONTROLLER_TO_BIC	4
#define I3C_BUS_TARGET_TO_BIC		4

/* Test use bus, BIC (1.8v) <-> HUB <-> (1.8v) BIC */
//#define I3C_BUS_CONTROLLER_TO_HUB	5
#define I3C_BUS_CONTROLLER_TO_HUB	4
#define I3C_BUS_TARGET_TO_HUB		I3C_BUS_TARGET_TO_BMC

#define MAX_WORK_ITEMS  4
enum {
    WORK_VIA_DISCOVERY_NOTIFY = 0,
    WORK_VIA_ALLOC_EID,
    WORK_VIA_ROUTING_INFO_UPDATE,
    WORK_VIA_GET_ROUTING_TBL_ENTRIES,
};

typedef struct _mctp_reg_eid_work {
    struct k_work work;
    mctp *mctp_inst;
    struct _get_routing_tbl_entry_with_address routing_tbl_entry;
} mctp_reg_eid_work;

extern mctp_reg_eid_work reg_eid_work[MAX_WORK_ITEMS];
extern struct k_sem reg_eid_work_sem;

extern struct k_timer get_routing_tbl_entries_timer;

/* init the mctp moduel for platform */
void send_cmd_to_dev(struct k_timer *timer);
void send_cmd_to_dev_handler(struct k_work *work);
void send_discovery_notify_cmd(struct k_timer *timer);
void send_discovery_notify_cmd_handler(struct k_work *work);
void get_routing_tbl_entries_timer_handler(struct k_timer *timer);
void get_routing_tbl_entries_work_handler(struct k_work *work);
bool add_routing_table_entries(mctp *mctp_inst, struct _get_routing_tbl_entry_with_address *routing_tbl_entry);
uint8_t register_endpoint(mctp *mctp_inst, struct _get_routing_tbl_entry_with_address *routing_tbl_entry);
void plat_mctp_init(void);
mctp *find_mctp_by_bus(uint8_t bus);
mctp *get_mctp_init();
uint8_t get_mctp_info(uint8_t dest_endpoint, mctp **mctp_inst, mctp_ext_params *ext_params);
mctp_port *find_port_by_mctp_inst(mctp *mctp_inst);
mctp *find_mctp_by_medium_type(uint8_t type);

#endif /* _PLAT_MCTP_h */
