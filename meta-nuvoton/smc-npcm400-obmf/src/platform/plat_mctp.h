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

#include "mctp.h"

/* i2c 8 bit address */
#define I2C_ADDR_BMC		0x22

/* mctp endpoint */
#define MCTP_EID_BMC_I3C	0x08
#define MCTP_EID_BMC_USB	0x08

/* i3c static 8-bit address */
#define I3C_STATIC_ADDR_BMC	0x20

/* i3c dev bus, BMC (1.8v) <-> SMC (1.8v) */
#define I3C_BUS_TARGET_TO_BMC		5
/* i3c buses used by the OBMF I3C Controller channels (ch18, ch19) */
#define I3C_BUS_CONTROLLER_TO_HUB	4
#define I3C_BUS_CONTROLLER_TO_HUB_1	5

void plat_mctp_init(void);
mctp *find_mctp_by_bus(uint8_t bus);
mctp *find_mctp_by_medium_type(uint8_t type);
uint8_t get_mctp_info(uint8_t dest_endpoint, mctp **mctp_inst, mctp_ext_params *ext_params);

#endif /* _PLAT_MCTP_h */
