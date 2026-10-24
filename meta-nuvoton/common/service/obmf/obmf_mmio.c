/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * OBMF MMIO Channel (Channel 1)
 *   Producer - Host sends MMIO read/write to our local state
 *   Consumer - We send MMIO read/write to host (legacy API)
 */

#include "obmf_internal.h"

LOG_MODULE_DECLARE(obmf);

/* --- MMIO Producer-side local state --- */
static obmf_vw_structure_t mmio_local_vw;
static obmf_uart_structure_t mmio_local_uart;
static obmf_post_code_structure_t mmio_local_post_code;
static obmf_flash_erase_regs_t mmio_local_flash_erase;

/* MMIO write callback (registered by consumers like UART bridge) */
static obmf_mmio_write_cb_t mmio_write_cb;
static void *mmio_write_cb_ctx;

void obmf_register_mmio_write_cb(obmf_mmio_write_cb_t cb, void *ctx)
{
	mmio_write_cb = cb;
	mmio_write_cb_ctx = ctx;
}

/* --- MMIO Producer: local buffer lookup --- */

static const uint8_t *mmio_local_get_buf(uint64_t addr, uint32_t *avail_size)
{
	if (addr >= OBMF_MMIO_POST_CODE_BASE_OFFSET &&
	    addr < OBMF_MMIO_POST_CODE_BASE_OFFSET + sizeof(mmio_local_post_code)) {
		*avail_size = sizeof(mmio_local_post_code) -
			      (addr - OBMF_MMIO_POST_CODE_BASE_OFFSET);
		return (const uint8_t *)&mmio_local_post_code +
		       (addr - OBMF_MMIO_POST_CODE_BASE_OFFSET);
	}
	if (addr >= OBMF_MMIO_UART_BASE_OFFSET &&
	    addr < OBMF_MMIO_UART_BASE_OFFSET + sizeof(mmio_local_uart)) {
		*avail_size = sizeof(mmio_local_uart) -
			      (addr - OBMF_MMIO_UART_BASE_OFFSET);
		return (const uint8_t *)&mmio_local_uart +
		       (addr - OBMF_MMIO_UART_BASE_OFFSET);
	}
	if (addr >= OBMF_MMIO_VW_BASE_OFFSET &&
	    addr < OBMF_MMIO_VW_BASE_OFFSET + sizeof(mmio_local_vw)) {
		*avail_size = sizeof(mmio_local_vw) -
			      (addr - OBMF_MMIO_VW_BASE_OFFSET);
		return (const uint8_t *)&mmio_local_vw +
		       (addr - OBMF_MMIO_VW_BASE_OFFSET);
	}
	if (addr >= OBMF_MMIO_FLASH_BASE_OFFSET + OBMF_FLASH_ERASE_START_ADDR_OFFSET &&
	    addr < OBMF_MMIO_FLASH_BASE_OFFSET + OBMF_FLASH_ERASE_START_ADDR_OFFSET +
		    sizeof(mmio_local_flash_erase)) {
		uint32_t base = OBMF_MMIO_FLASH_BASE_OFFSET + OBMF_FLASH_ERASE_START_ADDR_OFFSET;
		*avail_size = sizeof(mmio_local_flash_erase) - (addr - base);
		return (const uint8_t *)&mmio_local_flash_erase + (addr - base);
	}
	*avail_size = 0;
	return NULL;
}

/* Copy host-written data into the local region; fails if it doesn't fit */
int obmf_mmio_local_update(uint64_t offset, uint32_t len, const uint8_t *data)
{
	uint32_t avail = 0;
	uint8_t *dst = (uint8_t *)mmio_local_get_buf(offset, &avail);

	if (!dst || len > avail) {
		return -ERANGE;
	}

	memcpy(dst, data, len);
	return 0;
}

/* --- MMIO Producer: Read Request Handler --- */

void handle_mmio_read_req(obmf_icp_header_t *req_hdr,
			  uint8_t *req_payload, uint32_t req_len,
			  uint8_t *rsp_buf, uint32_t *rsp_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(req_hdr);
	obmf_icp_mmio_sub_header_t *req_sub =
		(obmf_icp_mmio_sub_header_t *)((uint8_t *)req_hdr + OBMF_HEADER_SIZE);

	uint8_t trans_type = OBMF_ICP_MMIO_GET_TRANS(req_sub);
	bool is_long = (trans_type == OBMF_ICP_TRANS_LONG_READ);
	uint32_t addr_size = is_long ? 8 : 4;
	uint32_t size_field_len = 2;
	uint32_t min_req_size = addr_size + size_field_len;

	if (req_len < min_req_size) {
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
					  rsp_buf, rsp_len, true);
	}

	/* v0.9: Short uses 32-bit addr, Long uses 64-bit addr */
	uint64_t addr = 0;
	if (is_long) {
		memcpy(&addr, req_payload, sizeof(uint64_t));
	} else {
		uint32_t addr32 = 0;
		memcpy(&addr32, req_payload, sizeof(uint32_t));
		addr = addr32;
	}

	uint16_t read_size;

	memcpy(&read_size, &req_payload[addr_size], sizeof(uint16_t));

	if (read_size == 0) {
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
					  rsp_buf, rsp_len, true);
	}

	LOG_DBG("MMIO Read Request: addr=0x%llx, size=%d", addr, read_size);

	/* Look up local buffer for this address */
	uint32_t avail = 0;
	const uint8_t *src = mmio_local_get_buf(addr, &avail);
	if (!src || read_size > avail) {
		LOG_ERR("MMIO read out of range: addr=0x%llx, size=%d",
			addr, read_size);
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_ERR_ADDR_OUT_OF_RANGE,
					  rsp_buf, rsp_len, true);
	}

	/* Build response */
	obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;
	obmf_icp_mmio_sub_header_t *rsp_sub =
		(obmf_icp_mmio_sub_header_t *)(rsp_buf + OBMF_HEADER_SIZE);
	uint8_t *rsp_payload = rsp_buf + OBMF_FULL_MMIO_HDR_SIZE;

	OBMF_ICP_SET_CHANNEL(rsp_hdr, channel);
	/* v1.0.0 RC1: Status in common header */
	OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr, OBMF_ICP_MSG_TYPE_RESPONSE,
				   OBMF_ICP_SUCCESS);
	obmf_mmio_rsp_sub_header(rsp_sub, req_sub);

	/* v1.0.0 RC1 Read Response: data only, no status byte in payload */
	memcpy(rsp_payload, src, read_size);

	OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_MMIO_SUB_HDR_SIZE + read_size);
	*rsp_len = OBMF_FULL_MMIO_HDR_SIZE + read_size;
}

/* --- MMIO Producer: Write Request Handler --- */

void handle_mmio_write_req(obmf_icp_header_t *req_hdr,
			   uint8_t *req_payload, uint32_t req_len,
			   uint8_t *rsp_buf, uint32_t *rsp_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(req_hdr);
	obmf_icp_mmio_sub_header_t *req_sub =
		(obmf_icp_mmio_sub_header_t *)((uint8_t *)req_hdr + OBMF_HEADER_SIZE);

	uint8_t trans_type = OBMF_ICP_MMIO_GET_TRANS(req_sub);
	bool is_long = (trans_type == OBMF_ICP_TRANS_LONG_WRITE);
	uint32_t addr_size = is_long ? 8 : 4;
	uint32_t min_write_size = addr_size + 1;

	if (req_len < min_write_size) {
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
					  rsp_buf, rsp_len, true);
	}

	/* v0.9: Short uses 32-bit addr, Long uses 64-bit addr */
	uint64_t addr = 0;
	if (is_long) {
		memcpy(&addr, req_payload, sizeof(uint64_t));
	} else {
		uint32_t addr32 = 0;
		memcpy(&addr32, req_payload, sizeof(uint32_t));
		addr = addr32;
	}

	/*
	 * Short/Long Write carry no length field: the data runs from after
	 * the address to the end of the message (Tables 10 and 14).
	 */
	uint16_t write_size = req_len - addr_size;
	uint8_t *write_data = req_payload + addr_size;

	LOG_DBG("MMIO Write Request: addr=0x%llx, size=%d", addr, write_size);

	/* Update local MMIO state */
	if (obmf_mmio_local_update(addr, write_size, write_data)) {
		LOG_ERR("MMIO write out of range: addr=0x%llx, size=%d",
			addr, write_size);
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_ERR_ADDR_OUT_OF_RANGE,
					  rsp_buf, rsp_len, true);
	}

	/* Notify registered callback */
	if (mmio_write_cb) {
		mmio_write_cb(addr, write_size, write_data, mmio_write_cb_ctx);
	}

	/* Build success response */
	obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;
	obmf_icp_mmio_sub_header_t *rsp_sub =
		(obmf_icp_mmio_sub_header_t *)(rsp_buf + OBMF_HEADER_SIZE);

	OBMF_ICP_SET_CHANNEL(rsp_hdr, channel);
	/* v1.0.0 RC1: Status in common header, write response has no payload */
	OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr, OBMF_ICP_MSG_TYPE_RESPONSE,
				   OBMF_ICP_SUCCESS);
	obmf_mmio_rsp_sub_header(rsp_sub, req_sub);

	OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_MMIO_SUB_HDR_SIZE);
	*rsp_len = OBMF_FULL_MMIO_HDR_SIZE;
}

/* =====================================================================
 * MMIO Channel (Consumer) - Legacy API for UART/POST/VW MMIO writes
 * ===================================================================== */

int obmf_mmio_read(uint32_t offset, uint32_t len, uint8_t *buf)
{
	uint8_t tx_buf[OBMF_FULL_MMIO_HDR_SIZE + 8 + 2]; /* Long read req */
	memset(tx_buf, 0, sizeof(tx_buf));
	obmf_icp_mmio_sub_header_t *sub_hdr =
		(obmf_icp_mmio_sub_header_t *)(tx_buf + OBMF_HEADER_SIZE);
	OBMF_ICP_MMIO_SET_TRANS(sub_hdr, OBMF_ICP_TRANS_LONG_READ);

	uint8_t *addr = (uint8_t *)(tx_buf + OBMF_FULL_MMIO_HDR_SIZE);
	uint8_t *read_len_ptr = (uint8_t *)(tx_buf + OBMF_FULL_MMIO_HDR_SIZE +
					     sizeof(uint64_t));

	uint64_t addr_val = MMIO_SPACE_OFFSET + offset;
	uint16_t read_len_val = len;
	memcpy(addr, &addr_val, sizeof(uint64_t));
	memcpy(read_len_ptr, &read_len_val, sizeof(uint16_t));

	int rc;
	int retries = OBMF_REQUEST_MAX_RETRY;

	do {
		rc = obmf_send_request(OBMF_ICP_MMIO_CHANNEL, tx_buf,
				       sizeof(tx_buf));
		if (rc == -EBUSY) {
			k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
		}
	} while (rc == -EBUSY && --retries > 0);

	return rc;
}

int obmf_mmio_write(uint32_t offset, uint32_t len, uint8_t *buf)
{
	/* Long Write: 64-bit address followed by the data, no length field */
	uint32_t total_len = OBMF_FULL_MMIO_HDR_SIZE + 8 + len;
	uint8_t *tx_buf = (uint8_t *)malloc(total_len);

	if (!tx_buf) {
		LOG_ERR("Failed to allocate MMIO write buffer (%d bytes)",
			total_len);
		return -1;
	}
	memset(tx_buf, 0, total_len);

	obmf_icp_mmio_sub_header_t *sub_hdr =
		(obmf_icp_mmio_sub_header_t *)(tx_buf + OBMF_HEADER_SIZE);
	OBMF_ICP_MMIO_SET_TRANS(sub_hdr, OBMF_ICP_TRANS_LONG_WRITE);

	uint8_t *addr = (uint8_t *)(tx_buf + OBMF_FULL_MMIO_HDR_SIZE);
	uint8_t *data = (uint8_t *)(tx_buf + OBMF_FULL_MMIO_HDR_SIZE +
				    sizeof(uint64_t));

	uint64_t addr_val = MMIO_SPACE_OFFSET + offset;
	memcpy(addr, &addr_val, sizeof(uint64_t));
	memcpy(data, buf, len);

	int rc;
	int retries = OBMF_REQUEST_MAX_RETRY;

	do {
		rc = obmf_send_request(OBMF_ICP_MMIO_CHANNEL, tx_buf, total_len);
		if (rc == -EBUSY) {
			k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
		}
	} while (rc == -EBUSY && --retries > 0);

	if (rc != 0 && retries == 0) {
		LOG_ERR("MMIO write request failed after retries: %d", rc);
		channel_tags[OBMF_ICP_MMIO_CHANNEL].is_outstanding = false;
	} else if (rc != 0 && rc != -ENETDOWN) {
		LOG_ERR("MMIO write request failed: %d", rc);
	}

	free(tx_buf);
	return rc;
}

/* --- MMIO Consumer: Response Handler --- */

void handle_mmio_response(obmf_icp_header_t *rsp_hdr,
			  uint8_t *payload, uint32_t payload_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(rsp_hdr);
	uint8_t status = OBMF_ICP_GET_STATUS(rsp_hdr);

	channel_tags[channel].is_outstanding = false;

	if (status != OBMF_ICP_SUCCESS) {
		LOG_ERR("MMIO response error: status=0x%x", status);
	}
}
