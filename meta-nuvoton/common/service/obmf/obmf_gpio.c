/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * OBMF GPIO Channel
 *   Producer - Host sends Get/Set GPIO requests to us
 *   Consumer - We send Get/Set GPIO requests to host
 */

#include "obmf_internal.h"

LOG_MODULE_DECLARE(obmf);

/* =====================================================================
 * GPIO Channel callbacks - indexed by OBMF channel number.
 * Channel 5 = VW GPIO (OBMF_ICP_VW_GPIO_CHANNEL)
 * Channel 6 = SGPIO1 (OBMF_ICP_SGPIO1_CHANNEL)
 * ===================================================================== */

static obmf_gpio_set_cb_t gpio_set_cb[OBMF_MAX_CHANNELS + 1];
static void              *gpio_set_cb_ctx[OBMF_MAX_CHANNELS + 1];
static obmf_gpio_get_cb_t gpio_get_cb[OBMF_MAX_CHANNELS + 1];
static void              *gpio_get_cb_ctx[OBMF_MAX_CHANNELS + 1];

/* Interrupt configuration set by the Consumer, per channel and GPIO index */
static uint8_t gpio_int_cfg[OBMF_MAX_CHANNELS + 1][OBMF_GPIO_MAX_INT_INDEX];

/* Generic per-channel registration */
void obmf_register_gpio_channel_get_cb(uint8_t channel,
				       obmf_gpio_get_cb_t cb, void *ctx)
{
	if (channel > OBMF_MAX_CHANNELS) {
		return;
	}
	gpio_get_cb[channel] = cb;
	gpio_get_cb_ctx[channel] = ctx;
}

void obmf_register_gpio_channel_set_cb(uint8_t channel,
				       obmf_gpio_set_cb_t cb, void *ctx)
{
	if (channel > OBMF_MAX_CHANNELS) {
		return;
	}
	gpio_set_cb[channel] = cb;
	gpio_set_cb_ctx[channel] = ctx;
}

/* Backward-compat: default to channel 5 (VW GPIO) */
void obmf_register_gpio_set_cb(obmf_gpio_set_cb_t cb, void *ctx)
{
	obmf_register_gpio_channel_set_cb(OBMF_ICP_VW_GPIO_CHANNEL, cb, ctx);
}

void obmf_register_gpio_get_cb(obmf_gpio_get_cb_t cb, void *ctx)
{
	obmf_register_gpio_channel_get_cb(OBMF_ICP_VW_GPIO_CHANNEL, cb, ctx);
}

/*
 * Get / Set Interrupt Config. A Set request is validated as a whole before
 * any entry is applied, so a bad entry leaves the configuration untouched.
 */
static uint8_t gpio_handle_int_config(uint8_t channel, uint8_t command,
				      const obmf_gpio_entry_t *entries,
				      uint16_t count,
				      obmf_gpio_entry_t *rsp_entries)
{
	for (uint16_t i = 0; i < count; i++) {
		uint16_t index = OBMF_GPIO_ENTRY_GET_INDEX(&entries[i]);
		uint8_t cfg = OBMF_GPIO_ENTRY_GET_DATA(&entries[i]);

		if (index >= OBMF_GPIO_MAX_INT_INDEX) {
			return OBMF_GPIO_STATUS_INT_NOT_SUPPORTED;
		}
		if (command == OBMF_GPIO_CMD_SET_INT_CONFIG &&
		    cfg > OBMF_GPIO_INT_BOTH_EDGE) {
			return OBMF_ICP_ERR_CMD_NOT_SUPPORTED;
		}
	}

	for (uint16_t i = 0; i < count; i++) {
		uint16_t index = OBMF_GPIO_ENTRY_GET_INDEX(&entries[i]);

		if (command == OBMF_GPIO_CMD_SET_INT_CONFIG) {
			gpio_int_cfg[channel][index] =
				OBMF_GPIO_ENTRY_GET_DATA(&entries[i]);
		} else {
			OBMF_GPIO_ENTRY_SET(&rsp_entries[i], index,
					    gpio_int_cfg[channel][index]);
		}
	}

	return OBMF_ICP_SUCCESS;
}

void handle_gpio_request(obmf_icp_header_t *req_hdr,
			 uint8_t *payload, uint32_t payload_len,
			 uint8_t *rsp_buf, uint32_t *rsp_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(req_hdr);

	obmf_gpio_get_cb_t get_cb =
		(channel <= OBMF_MAX_CHANNELS) ? gpio_get_cb[channel] : NULL;
	obmf_gpio_set_cb_t set_cb =
		(channel <= OBMF_MAX_CHANNELS) ? gpio_set_cb[channel] : NULL;
	void *get_ctx =
		(channel <= OBMF_MAX_CHANNELS) ? gpio_get_cb_ctx[channel] : NULL;
	void *set_ctx =
		(channel <= OBMF_MAX_CHANNELS) ? gpio_set_cb_ctx[channel] : NULL;

	if (payload_len < OBMF_GPIO_REQ_HDR_SIZE) {
		LOG_ERR("GPIO ch%d request too short (%d bytes)",
			channel, payload_len);
		*rsp_len = 0;
		return;
	}

	obmf_gpio_request_t *req = (obmf_gpio_request_t *)payload;
	uint8_t command = req->command;
	obmf_gpio_entry_t *entries =
		(obmf_gpio_entry_t *)(payload + OBMF_GPIO_REQ_HDR_SIZE);
	uint32_t entries_bytes = payload_len - OBMF_GPIO_REQ_HDR_SIZE;
	uint16_t num_entries = entries_bytes / sizeof(obmf_gpio_entry_t);

	/* Build response common header */
	obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;
	OBMF_ICP_SET_CHANNEL(rsp_hdr, channel);

	switch (command) {
	case OBMF_GPIO_CMD_GET_VALUES: {
		if (!get_cb) {
			OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr,
				OBMF_ICP_MSG_TYPE_RESPONSE,
				OBMF_ICP_ERR_CMD_NOT_SUPPORTED);
			obmf_gpio_response_t *rsp =
				(obmf_gpio_response_t *)(rsp_buf +
							 OBMF_HEADER_SIZE);
			rsp->command = command;
			OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_GPIO_RSP_HDR_SIZE);
			*rsp_len = OBMF_HEADER_SIZE + OBMF_GPIO_RSP_HDR_SIZE;
			return;
		}

		OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr,
			OBMF_ICP_MSG_TYPE_RESPONSE, OBMF_ICP_SUCCESS);

		obmf_gpio_response_t *rsp =
			(obmf_gpio_response_t *)(rsp_buf + OBMF_HEADER_SIZE);
		rsp->command = command;

		obmf_gpio_entry_t *rsp_entries =
			(obmf_gpio_entry_t *)(rsp_buf + OBMF_HEADER_SIZE +
					      OBMF_GPIO_RSP_HDR_SIZE);

		for (uint16_t i = 0; i < num_entries; i++) {
			uint16_t index = OBMF_GPIO_ENTRY_GET_INDEX(&entries[i]);
			uint8_t val = get_cb(index, get_ctx);
			OBMF_GPIO_ENTRY_SET(&rsp_entries[i], index, val);
		}

		uint32_t rsp_payload_size = OBMF_GPIO_RSP_HDR_SIZE +
					    num_entries * sizeof(obmf_gpio_entry_t);
		OBMF_ICP_SET_SIZE(rsp_hdr, rsp_payload_size);
		*rsp_len = OBMF_HEADER_SIZE + rsp_payload_size;
		break;
	}
	case OBMF_GPIO_CMD_SET_VALUES: {
		if (!set_cb) {
			OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr,
				OBMF_ICP_MSG_TYPE_RESPONSE,
				OBMF_ICP_ERR_CMD_NOT_SUPPORTED);
			obmf_gpio_response_t *rsp =
				(obmf_gpio_response_t *)(rsp_buf +
							 OBMF_HEADER_SIZE);
			rsp->command = command;
			OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_GPIO_RSP_HDR_SIZE);
			*rsp_len = OBMF_HEADER_SIZE + OBMF_GPIO_RSP_HDR_SIZE;
			return;
		}

		for (uint16_t i = 0; i < num_entries; i++) {
			uint16_t index = OBMF_GPIO_ENTRY_GET_INDEX(&entries[i]);
			uint8_t val = OBMF_GPIO_ENTRY_GET_DATA(&entries[i]);
			set_cb(index, val, set_ctx);
		}

		OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr,
			OBMF_ICP_MSG_TYPE_RESPONSE, OBMF_ICP_SUCCESS);

		obmf_gpio_response_t *rsp =
			(obmf_gpio_response_t *)(rsp_buf + OBMF_HEADER_SIZE);
		rsp->command = command;
		OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_GPIO_RSP_HDR_SIZE);
		*rsp_len = OBMF_HEADER_SIZE + OBMF_GPIO_RSP_HDR_SIZE;
		break;
	}
	case OBMF_GPIO_CMD_GET_INT_CONFIG:
	case OBMF_GPIO_CMD_SET_INT_CONFIG: {
		obmf_gpio_response_t *rsp =
			(obmf_gpio_response_t *)(rsp_buf + OBMF_HEADER_SIZE);
		obmf_gpio_entry_t *rsp_entries =
			(obmf_gpio_entry_t *)(rsp_buf + OBMF_HEADER_SIZE +
					      OBMF_GPIO_RSP_HDR_SIZE);
		uint8_t status = gpio_handle_int_config(channel, command,
							entries, num_entries,
							rsp_entries);
		uint32_t rsp_payload_size = OBMF_GPIO_RSP_HDR_SIZE;

		/* Only a successful Get carries the entry array back */
		if (status == OBMF_ICP_SUCCESS &&
		    command == OBMF_GPIO_CMD_GET_INT_CONFIG) {
			rsp_payload_size += num_entries *
					    sizeof(obmf_gpio_entry_t);
		}

		OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr,
			OBMF_ICP_MSG_TYPE_RESPONSE, status);
		rsp->command = command;
		OBMF_ICP_SET_SIZE(rsp_hdr, rsp_payload_size);
		*rsp_len = OBMF_HEADER_SIZE + rsp_payload_size;
		break;
	}
	case OBMF_GPIO_CMD_INT_NOTIFY:
	case OBMF_GPIO_CMD_INT_NOTIFY_DGRAM:
		/* Interrupt notifications are sent by the Producer, not to it */
		LOG_DBG("GPIO ch%d command %d from host (unexpected)",
			channel, command);
		OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr,
			OBMF_ICP_MSG_TYPE_RESPONSE,
			OBMF_ICP_ERR_CMD_NOT_SUPPORTED);
		obmf_gpio_response_t *rsp_n =
			(obmf_gpio_response_t *)(rsp_buf + OBMF_HEADER_SIZE);
		rsp_n->command = command;
		OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_GPIO_RSP_HDR_SIZE);
		*rsp_len = OBMF_HEADER_SIZE + OBMF_GPIO_RSP_HDR_SIZE;
		break;
	default:
		LOG_ERR("GPIO ch%d unsupported command: %d", channel, command);
		OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr,
			OBMF_ICP_MSG_TYPE_RESPONSE,
			OBMF_ICP_ERR_CMD_NOT_SUPPORTED);
		obmf_gpio_response_t *rsp_d =
			(obmf_gpio_response_t *)(rsp_buf + OBMF_HEADER_SIZE);
		rsp_d->command = command;
		OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_GPIO_RSP_HDR_SIZE);
		*rsp_len = OBMF_HEADER_SIZE + OBMF_GPIO_RSP_HDR_SIZE;
		break;
	}
}

int obmf_gpio_notify(const uint16_t *indices, const uint8_t *values,
		     uint16_t count, uint8_t channel)
{
	uint32_t total_len = OBMF_HEADER_SIZE + OBMF_GPIO_REQ_HDR_SIZE +
			     count * sizeof(obmf_gpio_entry_t);
	uint8_t *tx_buf = (uint8_t *)malloc(total_len);

	if (!tx_buf) {
		LOG_ERR("Failed to allocate GPIO notify buffer (%d bytes)",
			total_len);
		return -1;
	}
	memset(tx_buf, 0, total_len);

	/* Fill GPIO request header */
	obmf_gpio_request_t *req =
		(obmf_gpio_request_t *)(tx_buf + OBMF_HEADER_SIZE);
	req->command = OBMF_GPIO_CMD_INT_NOTIFY;

	/* Fill GPIO entries */
	obmf_gpio_entry_t *entries =
		(obmf_gpio_entry_t *)(tx_buf + OBMF_HEADER_SIZE +
				      OBMF_GPIO_REQ_HDR_SIZE);
	for (uint16_t i = 0; i < count; i++) {
		OBMF_GPIO_ENTRY_SET(&entries[i], indices[i], values[i]);
	}

	int rc;
	int retries = OBMF_REQUEST_MAX_RETRY;

	do {
		rc = obmf_send_request(channel, tx_buf, total_len);
		if (rc == -EBUSY) {
			k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
		}
	} while (rc == -EBUSY && --retries > 0);

	if (rc != 0 && retries == 0) {
		LOG_ERR("GPIO ch:%d write request failed after retries: %d", channel, rc);
		channel_tags[channel].is_outstanding = false;
	} else if (rc != 0 && rc != -ENETDOWN) {
		LOG_ERR("GPIO ch:%d write request failed: %d", channel, rc);
	}

	free(tx_buf);
	return rc;
}

int obmf_gpio_report_level(uint8_t channel, uint16_t index, bool high)
{
	const uint8_t data = 0; /* reserved for Interrupt Notify */
	bool notify;

	if (channel > OBMF_MAX_CHANNELS || index >= OBMF_GPIO_MAX_INT_INDEX) {
		return -EINVAL;
	}

	switch (gpio_int_cfg[channel][index]) {
	case OBMF_GPIO_INT_LEVEL_HIGH:
	case OBMF_GPIO_INT_RISING_EDGE:
		notify = high;
		break;
	case OBMF_GPIO_INT_LEVEL_LOW:
	case OBMF_GPIO_INT_FALLING_EDGE:
		notify = !high;
		break;
	case OBMF_GPIO_INT_BOTH_EDGE:
		notify = true;
		break;
	default:
		notify = false;
		break;
	}

	if (!notify) {
		return 0;
	}

	return obmf_gpio_notify(&index, &data, 1, channel);
}

/* =====================================================================
 * GPIO Channel (Consumer) - We send Get/Set GPIO requests to host
 * ===================================================================== */

static obmf_gpio_rsp_cb_t gpio_rsp_cb;
static void *gpio_rsp_cb_ctx;

void obmf_register_gpio_rsp_cb(obmf_gpio_rsp_cb_t cb, void *ctx)
{
	gpio_rsp_cb = cb;
	gpio_rsp_cb_ctx = ctx;
}

void handle_gpio_response(obmf_icp_header_t *rsp_hdr,
			  uint8_t *payload, uint32_t payload_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(rsp_hdr);
	uint8_t status = OBMF_ICP_GET_STATUS(rsp_hdr);

	channel_tags[channel].is_outstanding = false;

	if (payload_len < OBMF_GPIO_REQ_HDR_SIZE) {
		LOG_ERR("GPIO response too short (%d bytes)", payload_len);
		return;
	}

	obmf_gpio_request_t *rsp = (obmf_gpio_request_t *)payload;
	uint8_t command = rsp->command;

	uint16_t entry_bytes = payload_len - OBMF_GPIO_REQ_HDR_SIZE;
	uint16_t count = entry_bytes / sizeof(obmf_gpio_entry_t);

	uint16_t indices[count];
	uint8_t values[count];
	obmf_gpio_entry_t *entries =
		(obmf_gpio_entry_t *)(payload + OBMF_GPIO_REQ_HDR_SIZE);

	for (uint16_t i = 0; i < count; i++) {
		indices[i] = OBMF_GPIO_ENTRY_GET_INDEX(&entries[i]);
		values[i] = OBMF_GPIO_ENTRY_GET_DATA(&entries[i]);
	}

	if (gpio_rsp_cb) {
		gpio_rsp_cb(command, indices, values, count, gpio_rsp_cb_ctx);
	} else {
		LOG_DBG("GPIO response (cmd=%d, status=0x%02x) dropped - "
			"no callback", command, status);
	}
}

int obmf_gpio_get_values(const uint16_t *indices, uint16_t count, uint8_t channel)
{
	uint32_t total_len = OBMF_HEADER_SIZE + OBMF_GPIO_REQ_HDR_SIZE +
			     count * sizeof(obmf_gpio_entry_t);
	uint8_t *tx_buf = (uint8_t *)malloc(total_len);

	if (!tx_buf) {
		LOG_ERR("Failed to allocate GPIO get_values buffer");
		return -1;
	}
	memset(tx_buf, 0, total_len);

	/* Fill GPIO request header */
	obmf_gpio_request_t *req =
		(obmf_gpio_request_t *)(tx_buf + OBMF_HEADER_SIZE);
	req->command = OBMF_GPIO_CMD_GET_VALUES;

	/* Fill GPIO entries (data field unused for GET) */
	obmf_gpio_entry_t *entries =
		(obmf_gpio_entry_t *)(tx_buf + OBMF_HEADER_SIZE +
				      OBMF_GPIO_REQ_HDR_SIZE);
	for (uint16_t i = 0; i < count; i++) {
		OBMF_GPIO_ENTRY_SET(&entries[i], indices[i], 0);
	}

	int rc;
	int retries = OBMF_REQUEST_MAX_RETRY;

	do {
		rc = obmf_send_request(channel, tx_buf, total_len);
		if (rc == -EBUSY) {
			k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
		}
	} while (rc == -EBUSY && --retries > 0);

	if (rc != 0 && retries == 0) {
		LOG_ERR("GPIO ch:%d write request failed after retries: %d", channel, rc);
		channel_tags[channel].is_outstanding = false;
	} else if (rc != 0 && rc != -ENETDOWN) {
		LOG_ERR("GPIO ch:%d write request failed: %d", channel, rc);
	}

	free(tx_buf);
	return rc;
}

int obmf_gpio_set_values(const uint16_t *indices, const uint8_t *values,
			 uint16_t count, uint8_t channel)
{
	uint32_t total_len = OBMF_HEADER_SIZE + OBMF_GPIO_REQ_HDR_SIZE +
			     count * sizeof(obmf_gpio_entry_t);
	uint8_t *tx_buf = (uint8_t *)malloc(total_len);

	if (!tx_buf) {
		LOG_ERR("Failed to allocate GPIO set_values buffer");
		return -1;
	}
	memset(tx_buf, 0, total_len);

	/* Fill GPIO request header */
	obmf_gpio_request_t *req =
		(obmf_gpio_request_t *)(tx_buf + OBMF_HEADER_SIZE);
	req->command = OBMF_GPIO_CMD_SET_VALUES;

	LOG_DBG("gpio_set_values: channel=%d, count=%d", channel, count);

	/* Fill GPIO entries */
	obmf_gpio_entry_t *entries =
		(obmf_gpio_entry_t *)(tx_buf + OBMF_HEADER_SIZE +
				      OBMF_GPIO_REQ_HDR_SIZE);
	for (uint16_t i = 0; i < count; i++) {
		LOG_DBG("gpio_set_values: index=%d, value=%d", indices[i], values[i]);
		OBMF_GPIO_ENTRY_SET(&entries[i], indices[i], values[i]);
	}

	int rc;
	int retries = OBMF_REQUEST_MAX_RETRY;

	do {
		rc = obmf_send_request(channel, tx_buf, total_len);
		if (rc == -EBUSY) {
			k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
		}
	} while (rc == -EBUSY && --retries > 0);

	if (rc != 0 && retries == 0) {
		LOG_ERR("GPIO ch:%d write request failed after retries: %d", channel, rc);
		channel_tags[channel].is_outstanding = false;
	} else if (rc != 0 && rc != -ENETDOWN) {
		LOG_ERR("GPIO ch:%d write request failed: %d", channel, rc);
	}

	free(tx_buf);
	return rc;
}
