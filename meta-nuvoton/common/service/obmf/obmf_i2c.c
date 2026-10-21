/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * OBMF I2C Channels (v1.0.0 RC1 spec sections 4.6 and 4.7):
 *   - I2C Controller (Producer): the host (OBMF Consumer) sends I2C/SMBus
 *     requests; we drive the physical I2C bus via hal_i2c and respond.
 *   - I2C Target (Consumer): when addressed as an I2C target (e.g. SSIF),
 *     we forward the received write to the host and wait for its response.
 */

#include "obmf_internal.h"
#include "hal_i2c.h"
#include <kernel.h>

LOG_MODULE_DECLARE(obmf);

/* Blocking response wait timeout (Target channel). */
#define OBMF_I2C_RSP_TIMEOUT_MS 5000

/* hal_i2c retry count for controller transactions. */
#define OBMF_I2C_HAL_RETRY 3

/*
 * The OBMF response buffer in obmf.c is OBMF_RSP_BUF_SIZE (256) bytes; cap
 * read lengths so the I2C response always fits.  rx_len/tx_len in I2C_MSG are
 * uint8_t, so the transfer length is also bounded to 255.
 */
#define OBMF_I2C_RSP_BUF_SIZE 256
#define OBMF_I2C_MAX_READ_LEN \
	(OBMF_I2C_RSP_BUF_SIZE - OBMF_HEADER_SIZE - OBMF_I2C_RSP_HDR_SIZE)
#define OBMF_I2C_MAX_XFER_LEN (I2C_BUFF_SIZE - 1)

/* =====================================================================
 * I2C Controller Channel (Producer)
 *
 * The host (OBMF Consumer) sends I2C/SMBus read/write requests; we run the
 * transaction on the physical bus via hal_i2c and return the result.
 * ===================================================================== */

/* Physical I2C buses bound to each I2C Controller channel.
 * Slot 0 → OBMF_ICP_I2C_CONTROLLER_CHANNEL_0 (10)
 * Slot 1 → OBMF_ICP_I2C_CONTROLLER_CHANNEL_1 (11)
 * Slot 2 → OBMF_ICP_I2C_CONTROLLER_CHANNEL_2 (12)
 * Slot 3 → OBMF_ICP_I2C_CONTROLLER_CHANNEL_3 (13)
 * A value of 0xFF means the channel is not configured.
 */

/* Ordered list of channel numbers matching the slots above */
static const uint8_t i2c_ctrl_ch_nums[OBMF_MAX_I2C_CONTROLLER_CHANNELS] = {
	OBMF_ICP_I2C_CONTROLLER_CHANNEL_0,
	OBMF_ICP_I2C_CONTROLLER_CHANNEL_1,
	OBMF_ICP_I2C_CONTROLLER_CHANNEL_2,
	OBMF_ICP_I2C_CONTROLLER_CHANNEL_3,
};

static uint8_t i2c_controller_buses[OBMF_MAX_I2C_CONTROLLER_CHANNELS] = {
	[0 ... (OBMF_MAX_I2C_CONTROLLER_CHANNELS - 1)] = 0xFF
};

/* Return the slot index for a given I2C Controller channel number, or -1. */
static int i2c_ctrl_chan_to_idx(uint8_t channel)
{
	for (int i = 0; i < OBMF_MAX_I2C_CONTROLLER_CHANNELS; i++) {
		if (i2c_ctrl_ch_nums[i] == channel)
			return i;
	}
	return -1;
}

void obmf_i2c_set_controller_bus(uint8_t channel, uint8_t bus)
{
	int idx = i2c_ctrl_chan_to_idx(channel);

	if (idx < 0) {
		LOG_ERR("I2C set_bus: channel %u is not an I2C Controller channel",
			channel);
		return;
	}
	i2c_controller_buses[idx] = bus;
}

/*
 * Build an I2C Controller response into rsp_buf.  The I2C channel is an
 * interface-optimised channel: the response payload (obmf_i2c_response_t +
 * optional read data) immediately follows the 5-byte common header, and the
 * channel status is carried in the common header.  On error, read_len is 0.
 */
static void build_i2c_response(uint8_t channel, uint8_t status,
			       uint8_t command,
			       const uint8_t *read_data, uint16_t read_len,
			       uint8_t *rsp_buf, uint32_t *rsp_len)
{
	obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;

	OBMF_ICP_SET_CHANNEL(rsp_hdr, channel);
	OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr, OBMF_ICP_MSG_TYPE_RESPONSE, status);

	if (status != OBMF_ICP_SUCCESS) {
		read_len = 0;
		read_data = NULL;
	}

	obmf_i2c_response_t *rsp =
		(obmf_i2c_response_t *)(rsp_buf + OBMF_HEADER_SIZE);
	/* Per v1.0.0 RC1 spec section 4.6.2: byte 0[7] is Reserved=0 */
	rsp->command = command & 0x7F;
	rsp->read_length = read_len;
	rsp->reserved = 0;

	if (read_data && read_len > 0) {
		memcpy(rsp_buf + OBMF_HEADER_SIZE + OBMF_I2C_RSP_HDR_SIZE,
		       read_data, read_len);
	}

	uint16_t payload = OBMF_I2C_RSP_HDR_SIZE + read_len;

	OBMF_ICP_SET_SIZE(rsp_hdr, payload);
	*rsp_len = OBMF_HEADER_SIZE + payload;
}

/*
 * handle_i2c_request - called from the obmf.c RX thread when the host sends
 * an I2C Controller request.  Performs the transaction on the physical bus
 * via hal_i2c and fills rsp_buf with the response.
 */
void handle_i2c_request(obmf_icp_header_t *req_hdr,
			uint8_t *payload, uint32_t payload_len,
			uint8_t *rsp_buf, uint32_t *rsp_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(req_hdr);

	int bus_idx = i2c_ctrl_chan_to_idx(channel);

	if (bus_idx < 0 || i2c_controller_buses[bus_idx] == 0xFF) {
		LOG_ERR("I2C CH%d: channel not configured", channel);
		build_i2c_response(channel, OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
				   0, NULL, 0, rsp_buf, rsp_len);
		return;
	}

	uint8_t i2c_bus = i2c_controller_buses[bus_idx];

	if (payload_len < OBMF_I2C_REQ_HDR_SIZE) {
		LOG_ERR("I2C CH%d: request too short (%u)", channel,
			payload_len);
		build_i2c_response(channel, OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
				   0, NULL, 0, rsp_buf, rsp_len);
		return;
	}

	obmf_i2c_request_t *req = (obmf_i2c_request_t *)payload;
	uint8_t command = OBMF_I2C_CMD_GET_COMMAND(req->command);
	uint8_t address = req->address;
	uint16_t read_len = req->read_length;
	const uint8_t *write_data = payload + OBMF_I2C_REQ_HDR_SIZE;
	uint16_t write_len = payload_len - OBMF_I2C_REQ_HDR_SIZE;

	/* Cap lengths so the response fits and to the I2C_MSG buffer limits. */
	if (read_len > OBMF_I2C_MAX_READ_LEN) {
		read_len = OBMF_I2C_MAX_READ_LEN;
	}
	if (write_len > OBMF_I2C_MAX_XFER_LEN) {
		write_len = OBMF_I2C_MAX_XFER_LEN;
	}

	I2C_MSG msg = { 0 };

	msg.bus = i2c_bus;
	msg.target_addr = address & 0x7F; /* OBMF address is 7-bit */

	int ret;

	switch (command) {
	case OBMF_I2C_CMD_READ:
		if (read_len == 0) {
			build_i2c_response(channel,
					   OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
					   req->command, NULL, 0,
					   rsp_buf, rsp_len);
			return;
		}
		msg.tx_len = 0;
		msg.rx_len = read_len;
		ret = i2c_master_read(&msg, OBMF_I2C_HAL_RETRY);
		break;
	case OBMF_I2C_CMD_WRITE:
		msg.tx_len = write_len;
		if (write_len > 0) {
			memcpy(msg.data, write_data, write_len);
		}
		ret = i2c_master_write(&msg, OBMF_I2C_HAL_RETRY);
		read_len = 0;
		break;
	case OBMF_I2C_CMD_SMBUS_WRITE_READ:
		/* Combined write-then-read (repeated start) via hal_i2c. */
		if (read_len == 0) {
			build_i2c_response(channel,
					   OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
					   req->command, NULL, 0,
					   rsp_buf, rsp_len);
			return;
		}
		msg.tx_len = write_len;
		msg.rx_len = read_len;
		if (write_len > 0) {
			memcpy(msg.data, write_data, write_len);
		}
		ret = i2c_master_read(&msg, OBMF_I2C_HAL_RETRY);
		break;
	default:
		/* SMBus Block Read / Host Notify are not yet implemented. */
		LOG_WRN("I2C CH%d: unsupported command %u", channel, command);
		build_i2c_response(channel, OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
				   req->command, NULL, 0,
				   rsp_buf, rsp_len);
		return;
	}

	if (ret != 0) {
		if (ret == -ENXIO) {
			LOG_DBG("I2C CH%d: cmd %u to addr 0x%02x failed (ret %d)",
				channel, command, address, ret);
			build_i2c_response(channel, OBMF_I2C_STATUS_NACK,
					req->command, NULL, 0,
					rsp_buf, rsp_len);
		} else if (ret == -ETIMEDOUT) {
			LOG_ERR("I2C CH%d: cmd %u to addr 0x%02x failed (ret %d)",
				channel, command, address, ret);
			build_i2c_response(channel, OBMF_I2C_STATUS_TIMEOUT,
					req->command, NULL, 0,
					rsp_buf, rsp_len);
		} else if (ret == -EBUSY) {
			LOG_ERR("I2C CH%d: cmd %u to addr 0x%02x failed (ret %d)",
				channel, command, address, ret);
			build_i2c_response(channel, OBMF_I2C_STATUS_BUS_BUSY,
					req->command, NULL, 0,
					rsp_buf, rsp_len);
		} else {
			LOG_ERR("I2C CH%d: cmd %u to addr 0x%02x failed (ret %d)",
				channel, command, address, ret);
			build_i2c_response(channel, OBMF_ICP_ERR_OTHER,
					req->command, NULL, 0,
					rsp_buf, rsp_len);
		}

		return;
	}

	/* For reads, hal_i2c placed the data back into msg.data. */
	build_i2c_response(channel, OBMF_ICP_SUCCESS, req->command,
			   (read_len > 0) ? msg.data : NULL, read_len,
			   rsp_buf, rsp_len);
}


/* =====================================================================
 * I2C Target Channel (Consumer)
 *
 * When this device is addressed as an I2C target, the platform calls
 * obmf_i2c_target_write() to pass the received write payload through to the
 * host (OBMF Producer) and wait for its response.
 * ===================================================================== */

struct obmf_i2c_target_rsp_entry {
	uint8_t status;
	uint8_t command;
};

/* Per-index msgq and callback arrays (indexed by OBMF_I2C_TGT_CHAN_IDX) */
static struct k_msgq i2c_target_msgq[OBMF_MAX_I2C_TARGET_CHANNELS];
static uint8_t i2c_target_msgq_buf[OBMF_MAX_I2C_TARGET_CHANNELS]
				   [sizeof(struct obmf_i2c_target_rsp_entry)];
static obmf_i2c_target_rsp_cb_t i2c_target_rsp_cb[OBMF_MAX_I2C_TARGET_CHANNELS];
static void *i2c_target_rsp_cb_ctx[OBMF_MAX_I2C_TARGET_CHANNELS];

void obmf_i2c_target_channels_init(void)
{
	for (int i = 0; i < OBMF_MAX_I2C_TARGET_CHANNELS; i++) {
		k_msgq_init(&i2c_target_msgq[i],
			    (char *)i2c_target_msgq_buf[i],
			    sizeof(struct obmf_i2c_target_rsp_entry), 1);
	}
}

void obmf_register_i2c_target_rsp_cb(uint8_t idx,
				      obmf_i2c_target_rsp_cb_t cb, void *ctx)
{
	if (idx >= OBMF_MAX_I2C_TARGET_CHANNELS)
		return;
	i2c_target_rsp_cb[idx] = cb;
	i2c_target_rsp_cb_ctx[idx] = ctx;
}

void handle_i2c_target_response(obmf_icp_header_t *rsp_hdr,
				uint8_t *payload, uint32_t payload_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(rsp_hdr);
	uint8_t status_hdr = OBMF_ICP_GET_STATUS(rsp_hdr);
	struct obmf_i2c_target_rsp_entry entry = {
		.status = status_hdr,
		.command = 0,
	};

	if (status_hdr != OBMF_ICP_SUCCESS) {
		LOG_ERR("I2C target response error: status=0x%x", status_hdr);
	} else if (payload_len >= OBMF_I2C_TARGET_RSP_SIZE) {
		obmf_i2c_target_response_t *rsp =
			(obmf_i2c_target_response_t *)payload;

		entry.command = OBMF_I2C_CMD_GET_COMMAND(rsp->command);
	}

	channel_tags[channel].is_outstanding = false;

	uint8_t idx = OBMF_I2C_TGT_CHAN_IDX(channel);

	if (i2c_target_rsp_cb[idx])
		i2c_target_rsp_cb[idx](status_hdr, entry.command,
				       i2c_target_rsp_cb_ctx[idx]);
	if (k_msgq_put(&i2c_target_msgq[idx], &entry, K_NO_WAIT) != 0)
		LOG_ERR("I2C target[%d]: failed to enqueue response", idx);
}

int obmf_i2c_target_write(uint8_t channel, uint8_t address,
			  const uint8_t *write_data, uint16_t write_len)
{
	if (!write_data || write_len == 0) {
		LOG_ERR("I2C target write: invalid args");
		return -1;
	}

	uint32_t total_len =
		OBMF_HEADER_SIZE + OBMF_I2C_TARGET_REQ_HDR_SIZE + write_len;
	uint8_t *tx_buf = (uint8_t *)malloc(total_len);

	if (!tx_buf) {
		LOG_ERR("I2C target: failed to alloc tx buffer (%d bytes)",
			total_len);
		return -1;
	}
	memset(tx_buf, 0, total_len);

	obmf_i2c_target_request_t *req =
		(obmf_i2c_target_request_t *)(tx_buf + OBMF_HEADER_SIZE);
	req->command = OBMF_I2C_TARGET_CMD_WRITE;
	req->address = address;
	req->reserved = 0;
	memcpy(tx_buf + OBMF_HEADER_SIZE + OBMF_I2C_TARGET_REQ_HDR_SIZE,
	       write_data, write_len);

	int rc;
	int retries = OBMF_REQUEST_MAX_RETRY;

	do {
		rc = obmf_send_request(channel, tx_buf, total_len);
		if (rc == -EBUSY) {
			k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
		}
	} while (rc == -EBUSY && --retries > 0);

	if (rc != 0) {
		/* -ENETDOWN: channel already down, the link-down path reported it */
		if (rc != -ENETDOWN) {
			LOG_ERR("I2C target send request failed: %d (attempt %d)",
				rc, OBMF_REQUEST_MAX_RETRY - retries);
		}
		free(tx_buf);
		return rc;
	}

	uint8_t idx = OBMF_I2C_TGT_CHAN_IDX(channel);
	struct obmf_i2c_target_rsp_entry entry;

	rc = k_msgq_get(&i2c_target_msgq[idx], &entry,
			K_MSEC(OBMF_I2C_RSP_TIMEOUT_MS));
	if (rc != 0) {
		LOG_HEXDUMP_ERR(tx_buf, total_len, "I2C target timeout:");
		channel_tags[channel].is_outstanding = false;
		free(tx_buf);
		return -1;
	}

	free(tx_buf);
	return (entry.status == OBMF_ICP_SUCCESS) ? 0 : -1;
}
