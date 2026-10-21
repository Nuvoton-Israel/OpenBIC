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
 * MCTP medium over OBMF I2C Target channel
 *
 * TX path: MCTP bridge packet → SMBus framing → obmf_i2c_target_write()
 *          → OBMF I2C Target channel → OBMF host (BMC)
 *
 * RX path: handled externally by the paired MCTP_MEDIUM_TYPE_SMBUS instance
 *          (BMC uses OBMF I2C Controller to write to SMC's I2C target address
 *          on the physical bus; SMC's I2C target hardware receives it and
 *          mctp_smbus_read() picks it up).  This medium's read_data is a no-op
 *          stub so the TX queue / mctp_start() framework still works.
 */

#include "mctp_obmf_i2c.h"
#include "obmf.h"
#include "plat_i2c.h"

#include <logging/log.h>
#include <stdint.h>
#include <string.h>
#include <sys/crc.h>
#include <zephyr.h>
#include "libutil.h"

LOG_MODULE_DECLARE(mctp, LOG_LEVEL_DBG);

#define MCTP_SMBUS_CMD_CODE 0x0F
#define MCTP_SMBUS_PEC_POLY 0x07

typedef struct __attribute__((packed)) {
	uint8_t cmd_code;   /* Always 0x0F */
	uint8_t byte_count; /* 1 (src_addr) + N (mctp data) */
	uint8_t src_addr;   /* SMC's own 8-bit I2C address + 1 (read flag) */
} smbus_hdr_t;

/* -----------------------------------------------------------------------
 * PEC calculation: prepend destination write-address byte before the frame.
 * ----------------------------------------------------------------------- */
static uint8_t calc_smbus_pec(uint8_t dest_addr_8bit, const uint8_t *frame, uint32_t frame_len)
{
	/* dest_addr_8bit is the 8-bit I2C address with R/W=0 */
	uint8_t pec = crc8(&dest_addr_8bit, 1, MCTP_SMBUS_PEC_POLY, 0x00, false);
	pec = crc8(frame, frame_len, MCTP_SMBUS_PEC_POLY, pec, false);
	return pec;
}

/* -----------------------------------------------------------------------
 * TX: send buf (MCTP transport frame) to the OBMF host via I2C Target channel
 *
 * buf already contains the 4-byte MCTP transport header + message payload.
 * We wrap it in SMBus framing (cmd_code, byte_count, src_addr, [PEC]) and
 * forward via obmf_i2c_target_write().
 * ----------------------------------------------------------------------- */
static uint16_t mctp_obmf_i2c_write(void *mctp_p, uint8_t *buf, uint32_t len,
				    mctp_ext_params ext_params)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_p, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(buf, MCTP_ERROR);
	CHECK_ARG_WITH_RETURN(!len, MCTP_ERROR);

	if (ext_params.type != MCTP_MEDIUM_TYPE_OBMF_I2C)
		return MCTP_ERROR;

	mctp *mctp_inst = (mctp *)mctp_p;

	/* SMBus frame: cmd_code(1) + byte_count(1) + src_addr(1) + mctp_data(N) + pec(1) */
	uint32_t frame_len = sizeof(smbus_hdr_t) + len + 1 /* PEC */;
	uint8_t frame[frame_len];
	memset(frame, 0, frame_len);

	smbus_hdr_t *hdr = (smbus_hdr_t *)frame;
	hdr->cmd_code   = MCTP_SMBUS_CMD_CODE;
	hdr->byte_count = 1 /* src_addr */ + (uint8_t)len;
	/* src_addr: SMC's own 8-bit address with bit 0 set (SMBus convention) */
	hdr->src_addr   = mctp_inst->medium_conf.smbus_conf.addr | 0x01;

	memcpy(frame + sizeof(smbus_hdr_t), buf, len);

	/* Destination 8-bit address (write: R/W=0) */
	uint8_t dst_addr_8bit = ext_params.smbus_ext_params.addr & 0xFE;

	frame[frame_len - 1] = calc_smbus_pec(dst_addr_8bit, frame, frame_len - 1);

	LOG_HEXDUMP_DBG(frame, frame_len, "mctp_obmf_i2c_write");

	/*
	 * Map physical SMBus bus → OBMF I2C Target channel number.
	 * Add a new case here when a new I2C Target channel is introduced.
	 */
	uint8_t phys_bus = mctp_inst->medium_conf.smbus_conf.bus;
	uint8_t obmf_ch;

	switch (phys_bus) {
	case I2C_BUS1:
		obmf_ch = OBMF_I2C_TGT_CHAN_NUM(0);
		break;
	case I2C_BUS2:
		obmf_ch = OBMF_I2C_TGT_CHAN_NUM(1);
		break;
	default:
		LOG_ERR("mctp_obmf_i2c: no target channel for bus %d", phys_bus);
		return MCTP_ERROR;
	}

	int rc = obmf_i2c_target_write(obmf_ch, dst_addr_8bit >> 1,
				       frame, (uint16_t)frame_len);

	return (rc == 0) ? MCTP_SUCCESS : MCTP_ERROR;
}

/* -----------------------------------------------------------------------
 * RX stub: incoming traffic arrives via the separate SMBUS mctp instance
 * (physical I2C target hardware).  This function only yields the CPU so
 * the TX task thread created by mctp_start() is not busy-spinning.
 * ----------------------------------------------------------------------- */
static uint16_t mctp_obmf_i2c_read(void *mctp_p, uint8_t *buf, uint32_t len,
				   mctp_ext_params *ext_params)
{
	ARG_UNUSED(mctp_p);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	ARG_UNUSED(ext_params);

	k_sleep(K_MSEC(MCTP_POLL_TIME_MS));
	return 0;
}

/* -----------------------------------------------------------------------
 * Init / deinit
 * ----------------------------------------------------------------------- */
uint8_t mctp_obmf_i2c_init(mctp *mctp_inst, mctp_medium_conf medium_conf)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);

	mctp_inst->medium_conf = medium_conf;
	mctp_inst->read_data   = mctp_obmf_i2c_read;
	mctp_inst->write_data  = mctp_obmf_i2c_write;

	return MCTP_SUCCESS;
}

uint8_t mctp_obmf_i2c_deinit(mctp *mctp_inst)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);

	mctp_inst->read_data  = NULL;
	mctp_inst->write_data = NULL;
	memset(&mctp_inst->medium_conf, 0, sizeof(mctp_inst->medium_conf));
	return MCTP_SUCCESS;
}
