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

#ifndef _MCTP_OBMF_I2C_H
#define _MCTP_OBMF_I2C_H

#include "mctp.h"

/*
 * OBMF I2C MCTP medium
 *
 * This medium is used for MCTP bridging via the OBMF I2C Target channel.
 * It is TX-only: outgoing MCTP packets are sent to the OBMF host (BMC)
 * via obmf_i2c_target_write() using SMBus framing.
 *
 * Incoming MCTP packets (BMC → SMC direction) arrive on the physical SMBus
 * via the paired OBMF I2C Controller channel driving I2C writes to SMC's
 * I2C target address.  Those are received by the separate MCTP_MEDIUM_TYPE_SMBUS
 * instance and do NOT need to be read by this medium.
 *
 * medium_conf.smbus_conf:
 *   .bus  = unused (OBMF I2C Target channel number, informational only)
 *   .addr = 8-bit destination address of the OBMF host (BMC) on the SMBus
 */

uint8_t mctp_obmf_i2c_init(mctp *mctp_inst, mctp_medium_conf medium_conf);
uint8_t mctp_obmf_i2c_deinit(mctp *mctp_inst);

#endif /* _MCTP_OBMF_I2C_H */
