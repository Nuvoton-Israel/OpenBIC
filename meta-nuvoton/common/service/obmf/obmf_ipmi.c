/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * OBMF IPMI Channel (Consumer) - We send requests, receive responses
 */

#include "obmf_internal.h"

LOG_MODULE_DECLARE(obmf);

static obmf_ipmi_rsp_cb_t ipmi_rsp_cb;
static void *ipmi_rsp_cb_ctx;

void obmf_register_ipmi_rsp_cb(obmf_ipmi_rsp_cb_t cb, void *ctx)
{
	ipmi_rsp_cb = cb;
	ipmi_rsp_cb_ctx = ctx;
}

void handle_ipmi_response(obmf_icp_header_t *rsp_hdr,
			  uint8_t *payload, uint32_t payload_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(rsp_hdr);
	uint8_t status = OBMF_ICP_GET_STATUS(rsp_hdr);

	if (status != OBMF_ICP_SUCCESS) {
		LOG_ERR("IPMI response error: status=0x%x", status);
		channel_tags[channel].is_outstanding = false;
		return;
	}

	if (payload_len < OBMF_IPMI_RSP_HDR_SIZE + 1) {
		LOG_ERR("IPMI response too short (%d bytes)", payload_len);
		channel_tags[channel].is_outstanding = false;
		return;
	}

	obmf_ipmi_response_t *rsp = (obmf_ipmi_response_t *)payload;
	uint8_t *rsp_data = payload + OBMF_IPMI_RSP_HDR_SIZE;
	uint32_t rsp_data_len = payload_len - OBMF_IPMI_RSP_HDR_SIZE;

	/* rsp_data layout: cmd_byte + completion_code + data */
	uint8_t cmd = rsp_data[0];
	uint8_t cc = (rsp_data_len > 1) ? rsp_data[1] : 0;
	const uint8_t *resp_payload = (rsp_data_len > 2) ? &rsp_data[2] : NULL;
	uint16_t resp_payload_len = (rsp_data_len > 2) ? (rsp_data_len - 2) : 0;

	if (ipmi_rsp_cb) {
		ipmi_rsp_cb(rsp->netfn_lun, cmd, cc,
			    resp_payload, resp_payload_len,
			    ipmi_rsp_cb_ctx);
	}

	channel_tags[channel].is_outstanding = false;
}

int obmf_ipmi_send(uint8_t netfn_lun, uint8_t cmd,
		   const uint8_t *data, uint16_t data_len)
{
	/* Header + IPMI req hdr + cmd_byte + payload */
	uint32_t total_len = OBMF_HEADER_SIZE + OBMF_IPMI_REQ_HDR_SIZE +
			     1 + data_len;
	uint8_t *tx_buf = (uint8_t *)malloc(total_len);

	if (!tx_buf) {
		LOG_ERR("Failed to allocate IPMI send buffer (%d bytes)",
			total_len);
		return -1;
	}
	memset(tx_buf, 0, total_len);

	/* Fill IPMI request header */
	obmf_ipmi_request_t *req =
		(obmf_ipmi_request_t *)(tx_buf + OBMF_HEADER_SIZE);
	req->command = OBMF_IPMI_CMD_SEND_MESSAGE;
	req->netfn_lun = netfn_lun;

	/* IPMI command byte + data */
	uint8_t *ipmi_payload = tx_buf + OBMF_HEADER_SIZE +
				OBMF_IPMI_REQ_HDR_SIZE;
	ipmi_payload[0] = cmd;
	if (data && data_len > 0) {
		memcpy(&ipmi_payload[1], data, data_len);
	}

	int rc;
	int retries = OBMF_REQUEST_MAX_RETRY;

	do {
		rc = obmf_send_request(OBMF_ICP_IPMI_CHANNEL_NUM, tx_buf,
							   total_len);
		if (rc == -EBUSY) {
			k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
		}
	} while (rc == -EBUSY && --retries > 0);

	if (rc != 0 && retries == 0) {
		LOG_ERR("IPMI write request failed after retries: %d", rc);
		channel_tags[OBMF_ICP_IPMI_CHANNEL_NUM].is_outstanding = false;
	} else if (rc != 0 && rc != -ENETDOWN) {
		LOG_ERR("IPMI write request failed: %d", rc);
	}

	free(tx_buf);
	return rc;
}
