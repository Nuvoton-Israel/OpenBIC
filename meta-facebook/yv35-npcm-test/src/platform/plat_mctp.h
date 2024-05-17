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

#include "storage_handler.h"
#define MCTP_MSG_TYPE_SHIFT	0
#define MCTP_MSG_TYPE_MASK	0x7F
#define MCTP_IC_SHIFT		7
#define MCTP_IC_MASK		0x80

/* i2c 8 bit address */
#define I2C_ADDR_BIC		0x20
#define I2C_ADDR_CXL0		0x74
#define I2C_ADDR_CXL1		0xC2
#define I2C_ADDR_BMC		0x22
/* use i2c0 */
#define I2C_BUS_PLDM I2C_BUS1
/* i2c dev bus, use i2c2 */
#define I2C_BUS_CXL I2C_BUS3

/* mctp endpoint */
#define MCTP_EID_CXL_I2C	0x2E
#define MCTP_EID_BMC_I2C	0x15
#define MCTP_EID_BIC_I2C	0x0A
#define MCTP_EID_BMC_I3C	0x08
#define MCTP_EID_BIC_I3C	0x0C

/* i3c static 8-bit address */
#define I3C_STATIC_ADDR_BIC	0xA
#define I3C_STATIC_ADDR_BMC	0x20

/* i3c dynamic 8-bit address */
#define I3C_DYNAMIC_ADDR_BIC	0xA

/* i3c dev bus */
#define I3C_BUS_MASTER		4
#define I3C_BUS_BMC		5

/* init the mctp moduel for platform */
void send_cmd_to_dev(struct k_timer *timer);
void send_cmd_to_dev_handler(struct k_work *work);
void plat_mctp_init(void);
mctp *find_mctp_by_smbus(uint8_t bus);
mctp *get_mctp_init();
uint8_t get_mctp_info(uint8_t dest_endpoint, mctp **mctp_inst, mctp_ext_params *ext_params);

#endif /* _PLAT_MCTP_h */
