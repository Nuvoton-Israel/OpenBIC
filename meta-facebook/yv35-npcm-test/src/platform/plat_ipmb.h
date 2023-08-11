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

#ifndef PLAT_IPMB_H
#define PLAT_IPMB_H

#include "plat_i2c.h"

/* use i2c1 */
#define IPMB_BMC_BIC_BUS I2C_BUS2

/* bic ipmb address = 0x11 (7-bit) */
#define SELF_I2C_ADDRESS (34 >> 1)

/* bmc ipmb address = 0x10 (7-bit) */
#define BMC_I2C_ADDRESS (32 >> 1)

#define MAX_IPMB_IDX 2

enum {
	BMC_BIC_IPMB_IDX,
	RESERVE_IPMB_IDX,
};

#endif
