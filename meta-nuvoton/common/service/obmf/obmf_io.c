/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * OBMF I/O Port Channel (Channel 9, v0.9.2 spec section 4.31)
 *
 * Producer side: handles port I/O read/write requests sent by the
 * OBMF-ICP Consumer (host/CPU).  The primary use case is capturing
 * POST codes written to I/O port 0x80 (and 0x81-0x83 for extended codes).
 *
 * The MMIO channel continues to carry its own post-code region; this
 * channel provides a dedicated, I/O-port-based alternative.
 */

#include "obmf_internal.h"

LOG_MODULE_DECLARE(obmf);

/* -----------------------------------------------------------------------
 * Local state – POST code register (I/O ports 0x80..0x83)
 * ----------------------------------------------------------------------- */
static uint8_t io_post_code[OBMF_IO_POST_CODE_SIZE];

/* -----------------------------------------------------------------------
 * I/O write callback (registered by platform postcode consumer)
 * ----------------------------------------------------------------------- */
static obmf_io_write_cb_t io_write_cb;
static void *io_write_cb_ctx;

void obmf_register_io_write_cb(obmf_io_write_cb_t cb, void *ctx)
{
	io_write_cb = cb;
	io_write_cb_ctx = ctx;
}

/* -----------------------------------------------------------------------
 * Public accessor: read back the locally cached POST code bytes
 * ----------------------------------------------------------------------- */
void obmf_io_get_post_code(uint8_t *buf, uint8_t len)
{
	uint8_t copy_len = (len < OBMF_IO_POST_CODE_SIZE) ?
			   len : OBMF_IO_POST_CODE_SIZE;
	memcpy(buf, io_post_code, copy_len);
}

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

/*
 * Look up a writable pointer to local state for a given I/O port.
 * Currently only the POST code range (0x80..0x83) is mapped.
 * Returns 0 on success, -1 if the port is not in any known range.
 */
static int io_local_get_buf(uint16_t port, uint8_t **buf, uint16_t *avail)
{
	if (port >= OBMF_IO_POST_CODE_PORT &&
	    port < OBMF_IO_POST_CODE_PORT + OBMF_IO_POST_CODE_SIZE) {
		uint16_t off = port - OBMF_IO_POST_CODE_PORT;
		*buf = &io_post_code[off];
		*avail = OBMF_IO_POST_CODE_SIZE - off;
		return 0;
	}
	return -1;
}

/*
 * Apply a write burst to local state.
 * is_fixed == true  → all bytes written to the same port (Fixed Port I/O).
 * is_fixed == false → each byte written to port+i (Sequential Port I/O).
 */
static void io_local_write(uint16_t port, const uint8_t *data,
			   uint16_t size, bool is_fixed)
{
	for (uint16_t i = 0; i < size; i++) {
		uint16_t target = is_fixed ? port : (uint16_t)(port + i);
		uint8_t *dst;
		uint16_t avail;

		if (io_local_get_buf(target, &dst, &avail) == 0) {
			*dst = data[i];
		}
	}
}

/* -----------------------------------------------------------------------
 * Producer side – handle incoming I/O port requests from Consumer
 * ----------------------------------------------------------------------- */

void handle_io_request(obmf_icp_header_t *req_hdr,
		       uint8_t *req_payload, uint32_t req_len,
		       uint8_t *rsp_buf, uint32_t *rsp_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(req_hdr);

	/* Parse the I/O channel 2-byte sub-header */
	obmf_icp_io_sub_header_t *req_sub =
		(obmf_icp_io_sub_header_t *)((uint8_t *)req_hdr + OBMF_HEADER_SIZE);

	/* Tag validation (spec section 5.2, rule 5) */
	uint8_t tag = OBMF_ICP_IO_GET_TAG(req_sub);

	if (tag != channel_tags[channel].producer_tag) {
		LOG_ERR("IO CH%d: tag mismatch (got %u, expected %u) – resetting",
			channel, tag, channel_tags[channel].producer_tag);
		obmf_build_err_response(req_hdr, OBMF_ICP_ERR_OTHER,
					rsp_buf, rsp_len, false);
		channel_tags[channel].producer_tag = 0;
		channel_tags[channel].consumer_tag = 0;
		return;
	}

	uint8_t trans = OBMF_ICP_IO_GET_TRANS(req_sub);

	/* Decode transaction type into direction, fixed/seq, and access width */
	bool is_write;
	bool is_fixed;
	uint8_t width; /* access granularity: 1, 2, or 4 bytes */

	switch (trans) {
	case OBMF_ICP_IO_TRANS_SEQ_READ_8:
		is_write = false; is_fixed = false; width = 1; break;
	case OBMF_ICP_IO_TRANS_SEQ_WRITE_8:
		is_write = true;  is_fixed = false; width = 1; break;
	case OBMF_ICP_IO_TRANS_SEQ_READ_16:
		is_write = false; is_fixed = false; width = 2; break;
	case OBMF_ICP_IO_TRANS_SEQ_WRITE_16:
		is_write = true;  is_fixed = false; width = 2; break;
	case OBMF_ICP_IO_TRANS_SEQ_READ_32:
		is_write = false; is_fixed = false; width = 4; break;
	case OBMF_ICP_IO_TRANS_SEQ_WRITE_32:
		is_write = true;  is_fixed = false; width = 4; break;
	case OBMF_ICP_IO_TRANS_FIXED_READ_8:
		is_write = false; is_fixed = true;  width = 1; break;
	case OBMF_ICP_IO_TRANS_FIXED_WRITE_8:
		is_write = true;  is_fixed = true;  width = 1; break;
	case OBMF_ICP_IO_TRANS_FIXED_READ_16:
		is_write = false; is_fixed = true;  width = 2; break;
	case OBMF_ICP_IO_TRANS_FIXED_WRITE_16:
		is_write = true;  is_fixed = true;  width = 2; break;
	case OBMF_ICP_IO_TRANS_FIXED_READ_32:
		is_write = false; is_fixed = true;  width = 4; break;
	case OBMF_ICP_IO_TRANS_FIXED_WRITE_32:
		is_write = true;  is_fixed = true;  width = 4; break;
	default:
		LOG_ERR("IO CH%d: unsupported transaction type %d", channel, trans);
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
					  rsp_buf, rsp_len, true);
	}

	/*
	 * Payload layout (after IO sub-header):
	 *   Bytes [1:0]  – 16-bit port address (LE)
	 *   Byte  [2]    – size in bytes
	 *   Bytes [3+]   – write data (writes only)
	 */
	if (req_len < 3) {
		LOG_ERR("IO CH%d: payload too short (%u)", channel, req_len);
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
					  rsp_buf, rsp_len, true);
	}

	uint16_t port_addr;
	memcpy(&port_addr, req_payload, sizeof(uint16_t));
	uint8_t size = req_payload[2];

	if (size == 0 || (size % width) != 0) {
		LOG_ERR("IO CH%d: invalid size %u for width %u",
			channel, size, width);
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
					  rsp_buf, rsp_len, true);
	}

	/* Pre-fill common response header fields */
	obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;
	obmf_icp_io_sub_header_t *rsp_sub =
		(obmf_icp_io_sub_header_t *)(rsp_buf + OBMF_HEADER_SIZE);

	OBMF_ICP_SET_CHANNEL(rsp_hdr, channel);
	OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr, OBMF_ICP_MSG_TYPE_RESPONSE,
				   OBMF_ICP_SUCCESS);
	*rsp_sub = *req_sub;

	if (is_write) {
		/* ---- Write path ---- */
		if (req_len < (uint32_t)(3 + size)) {
			LOG_ERR("IO CH%d: write payload too short (%u < %u)",
				channel, req_len, 3 + size);
			OBMF_BUILD_ERR_AND_RETURN(req_hdr,
						  OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
						  rsp_buf, rsp_len, true);
		}

		const uint8_t *write_data = req_payload + 3;

		LOG_DBG("IO Write: port=0x%04x size=%u %s",
			port_addr, size, is_fixed ? "fixed" : "seq");

		io_local_write(port_addr, write_data, size, is_fixed);

		if (io_write_cb) {
			io_write_cb(port_addr, size, write_data,
				    is_fixed, io_write_cb_ctx);
		}

		/* Write response: sub-header only, no additional payload */
		OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_ICP_IO_SUB_HDR_SIZE);
		*rsp_len = OBMF_FULL_IO_HDR_SIZE;

	} else {
		/* ---- Read path ---- */
		uint8_t *rsp_status = rsp_buf + OBMF_FULL_IO_HDR_SIZE;
		uint8_t *rsp_data   = rsp_status + 1;
		uint16_t collected  = 0;

		for (uint8_t i = 0; i < size; i++) {
			uint16_t target = is_fixed ?
				port_addr : (uint16_t)(port_addr + i);
			uint8_t *src;
			uint16_t avail;

			if (io_local_get_buf(target, &src, &avail) != 0) {
				LOG_ERR("IO Read: port=0x%04x out of range",
					target);
				OBMF_BUILD_ERR_AND_RETURN(
					req_hdr,
					OBMF_IO_STATUS_ADDR_OUT_OF_RANGE,
					rsp_buf, rsp_len, true);
			}
			rsp_data[collected++] = *src;
		}

		LOG_DBG("IO Read: port=0x%04x size=%u %s",
			port_addr, size, is_fixed ? "fixed" : "seq");

		/* Read response: status(1) + data(size) */
		*rsp_status = OBMF_ICP_SUCCESS;
		OBMF_ICP_SET_SIZE(rsp_hdr,
				  OBMF_ICP_IO_SUB_HDR_SIZE + 1 + collected);
		*rsp_len = OBMF_FULL_IO_HDR_SIZE + 1 + collected;
	}

	channel_tags[channel].producer_tag ^= 1;
}

/* -----------------------------------------------------------------------
 * Consumer side – handle responses to I/O read requests we initiated
 * ----------------------------------------------------------------------- */

void handle_io_response(obmf_icp_header_t *rsp_hdr,
			uint8_t *payload, uint32_t payload_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(rsp_hdr);
	uint8_t status  = OBMF_ICP_GET_STATUS(rsp_hdr);

	channel_tags[channel].is_outstanding = false;

	if (status != OBMF_ICP_SUCCESS) {
		LOG_ERR("IO CH%d response error: status=0x%02x",
			channel, status);
		return;
	}

	/*
	 * For read responses the payload is: status(1) + data(N).
	 * Currently no Consumer-initiated I/O reads are used for POST codes,
	 * but the hook is present for future use.
	 */
	if (payload_len > 1) {
		LOG_DBG("IO CH%d read response: %u data bytes",
			channel, payload_len - 1);
	}
}

/* -----------------------------------------------------------------------
 * Consumer API – userspace-callable port I/O write / read
 *
 * Transaction type is chosen automatically from the access width implied
 * by len:
 *   len is a multiple of 4  →  32-bit (Sequential Port I/O)
 *   len is a multiple of 2  →  16-bit (Sequential Port I/O)
 *   otherwise               →   8-bit (Sequential Port I/O)
 *
 * For a single-port burst (same address repeated) pass is_fixed = true;
 * the Fixed-Port variants are then selected instead.
 *
 * Request packet layout after the 3-byte IO sub-header:
 *   Bytes 0-1  port address (LE uint16)
 *   Byte  2    size in bytes
 *   Bytes 3+   write data (write only)
 * ----------------------------------------------------------------------- */

/**
 * @brief Send a Sequential/Fixed Port I/O write over the OBMF I/O channel.
 *
 * @param port   Starting I/O port address.
 * @param len    Number of bytes to write (determines access width).
 * @param buf    Source data buffer.
 * @param fixed  true = Fixed Port I/O (all bytes to same port address),
 *               false = Sequential Port I/O (port incremented per byte).
 * @return 0 on success, negative errno on failure.
 */
int obmf_io_write(uint16_t port, uint16_t len, const uint8_t *buf, bool fixed)
{
	if (!buf || len == 0) {
		return -EINVAL;
	}

	/* Select transaction type based on access width implied by len */
	uint8_t trans;

	if ((len % 4) == 0) {
		trans = fixed ? OBMF_ICP_IO_TRANS_FIXED_WRITE_32
			      : OBMF_ICP_IO_TRANS_SEQ_WRITE_32;
	} else if ((len % 2) == 0) {
		trans = fixed ? OBMF_ICP_IO_TRANS_FIXED_WRITE_16
			      : OBMF_ICP_IO_TRANS_SEQ_WRITE_16;
	} else {
		trans = fixed ? OBMF_ICP_IO_TRANS_FIXED_WRITE_8
			      : OBMF_ICP_IO_TRANS_SEQ_WRITE_8;
	}

	/*
	 * Packet: OBMF common header (5)
	 *       + IO sub-header      (3)
	 *       + port addr (2) + size (1) + data (len)
	 */
	uint32_t total_len = OBMF_FULL_IO_HDR_SIZE + 2 + 1 + len;
	uint8_t *tx_buf = (uint8_t *)malloc(total_len);

	if (!tx_buf) {
		LOG_ERR("IO write: failed to alloc %u bytes", total_len);
		return -ENOMEM;
	}
	memset(tx_buf, 0, total_len);

	/* Fill IO sub-header */
	obmf_icp_io_sub_header_t *sub_hdr =
		(obmf_icp_io_sub_header_t *)(tx_buf + OBMF_HEADER_SIZE);
	OBMF_ICP_IO_SET_TRANS(sub_hdr, trans);
	/* Tag is set by obmf_send_request */

	/* Fill payload */
	uint8_t *payload = tx_buf + OBMF_FULL_IO_HDR_SIZE;
	memcpy(payload, &port, sizeof(uint16_t));   /* port addr LE */
	payload[2] = (uint8_t)len;                  /* size        */
	memcpy(payload + 3, buf, len);              /* data        */

	int rc;
	int retries = OBMF_REQUEST_MAX_RETRY;

	do {
		rc = obmf_send_request(OBMF_ICP_IO_CHANNEL, tx_buf, total_len);
		if (rc == -EBUSY) {
			k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
		}
	} while (rc == -EBUSY && --retries > 0);

	if (rc != 0 && retries == 0) {
		LOG_ERR("IO write request failed after retries: %d", rc);
		channel_tags[OBMF_ICP_IO_CHANNEL].is_outstanding = false;
	} else if (rc != 0 && rc != -ENETDOWN) {
		LOG_ERR("IO write request failed: %d", rc);
	}

	free(tx_buf);
	return rc;
}

/**
 * @brief Send a Sequential/Fixed Port I/O read over the OBMF I/O channel.
 *
 * @param port   Starting I/O port address.
 * @param len    Number of bytes to read (determines access width).
 * @param fixed  true = Fixed Port I/O, false = Sequential Port I/O.
 * @return 0 on success, negative errno on failure.
 *
 * Note: The read data is returned asynchronously via the response handler
 * (handle_io_response).  Register a response callback if needed.
 */
int obmf_io_read(uint16_t port, uint16_t len, bool fixed)
{
	if (len == 0) {
		return -EINVAL;
	}

	uint8_t trans;

	if ((len % 4) == 0) {
		trans = fixed ? OBMF_ICP_IO_TRANS_FIXED_READ_32
			      : OBMF_ICP_IO_TRANS_SEQ_READ_32;
	} else if ((len % 2) == 0) {
		trans = fixed ? OBMF_ICP_IO_TRANS_FIXED_READ_16
			      : OBMF_ICP_IO_TRANS_SEQ_READ_16;
	} else {
		trans = fixed ? OBMF_ICP_IO_TRANS_FIXED_READ_8
			      : OBMF_ICP_IO_TRANS_SEQ_READ_8;
	}

	/* Packet: common header (5) + IO sub-header (3) + port (2) + size (1) */
	uint32_t total_len = OBMF_FULL_IO_HDR_SIZE + 2 + 1;
	uint8_t tx_buf[OBMF_FULL_IO_HDR_SIZE + 2 + 1];
	memset(tx_buf, 0, sizeof(tx_buf));

	obmf_icp_io_sub_header_t *sub_hdr =
		(obmf_icp_io_sub_header_t *)(tx_buf + OBMF_HEADER_SIZE);
	OBMF_ICP_IO_SET_TRANS(sub_hdr, trans);

	uint8_t *payload = tx_buf + OBMF_FULL_IO_HDR_SIZE;
	memcpy(payload, &port, sizeof(uint16_t));
	payload[2] = (uint8_t)len;

	int rc;
	int retries = OBMF_REQUEST_MAX_RETRY;

	do {
		rc = obmf_send_request(OBMF_ICP_IO_CHANNEL, tx_buf, total_len);
		if (rc == -EBUSY) {
			k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
		}
	} while (rc == -EBUSY && --retries > 0);

	return rc;
}
