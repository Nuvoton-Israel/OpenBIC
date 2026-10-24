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
 * Check every entry of a Get/Set request against the channel's GPIO
 * Configuration Array: the index must be one the channel exposes, and Set
 * Values may only drive an output.  Nothing is applied unless all pass.
 */
static uint8_t gpio_check_entries(uint8_t channel, uint8_t command,
				  const obmf_gpio_entry_t *entries,
				  uint16_t count)
{
	const obmf_gpio_config_data_t *cfg = obmf_gpio_channel_config(channel);
	uint16_t n;

	if (!cfg) {
		return OBMF_ICP_SUCCESS;
	}
	n = MIN(cfg->gpio_count, OBMF_GPIO_MAX_ENTRIES);

	for (uint16_t i = 0; i < count; i++) {
		uint16_t index = OBMF_GPIO_ENTRY_GET_INDEX(&entries[i]);
		const obmf_gpio_config_entry_t *e = NULL;

		for (uint16_t j = 0; j < n; j++) {
			if ((cfg->entries[j].gpio_index & 0x0FFF) == index) {
				e = &cfg->entries[j];
				break;
			}
		}
		if (!e) {
			return OBMF_GPIO_STATUS_INDEX_NOT_SUPPORTED;
		}
		if (command == OBMF_GPIO_CMD_SET_VALUES &&
		    e->gpio_direction == OBMF_GPIO_DIR_INPUT) {
			return OBMF_GPIO_STATUS_INVALID_OPERATION;
		}
	}

	return OBMF_ICP_SUCCESS;
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

	if (payload_len < OBMF_GPIO_REQ_HDR_SIZE ||
	    (payload_len - OBMF_GPIO_REQ_HDR_SIZE) % sizeof(obmf_gpio_entry_t)) {
		LOG_ERR("GPIO ch%d malformed request (%d bytes)",
			channel, payload_len);
		obmf_build_err_response(req_hdr, OBMF_ICP_STATUS_INVALID_CMD,
					rsp_buf, rsp_len, false);
		return;
	}

	obmf_gpio_request_t *req = (obmf_gpio_request_t *)payload;
	uint8_t command = req->command;
	obmf_gpio_entry_t *entries =
		(obmf_gpio_entry_t *)(payload + OBMF_GPIO_REQ_HDR_SIZE);
	uint32_t entries_bytes = payload_len - OBMF_GPIO_REQ_HDR_SIZE;
	uint16_t num_entries = entries_bytes / sizeof(obmf_gpio_entry_t);

	/* Get responses echo every entry; they must fit the response buffer */
	if ((command == OBMF_GPIO_CMD_GET_VALUES ||
	     command == OBMF_GPIO_CMD_GET_INT_CONFIG) &&
	    OBMF_HEADER_SIZE + OBMF_GPIO_RSP_HDR_SIZE + entries_bytes >
	    OBMF_RSP_BUF_SIZE) {
		LOG_ERR("GPIO ch%d: %u entries do not fit a response",
			channel, num_entries);
		obmf_build_err_response(req_hdr,
					OBMF_ICP_STATUS_SIZE_NOT_SUPPORTED,
					rsp_buf, rsp_len, false);
		return;
	}

	/* Build response common header */
	obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;
	OBMF_ICP_SET_CHANNEL(rsp_hdr, channel);

	if (command <= OBMF_GPIO_CMD_SET_INT_CONFIG) {
		uint8_t status = gpio_check_entries(channel, command, entries,
						    num_entries);

		if (status != OBMF_ICP_SUCCESS) {
			obmf_gpio_response_t *rsp_e =
				(obmf_gpio_response_t *)(rsp_buf +
							 OBMF_HEADER_SIZE);

			LOG_WRN("GPIO ch%d command %d rejected: 0x%02x",
				channel, command, status);
			OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr,
				OBMF_ICP_MSG_TYPE_RESPONSE, status);
			rsp_e->command = command;
			OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_GPIO_RSP_HDR_SIZE);
			*rsp_len = OBMF_HEADER_SIZE + OBMF_GPIO_RSP_HDR_SIZE;
			return;
		}
	}

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
		uint8_t status = OBMF_ICP_SUCCESS;

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

		/* Apply nothing unless every entry carries a valid level */
		for (uint16_t i = 0; i < num_entries; i++) {
			if (OBMF_GPIO_ENTRY_GET_DATA(&entries[i]) >
			    OBMF_GPIO_VALUE_HIGH) {
				status = OBMF_ICP_ERR_CMD_NOT_SUPPORTED;
				break;
			}
		}

		if (status == OBMF_ICP_SUCCESS) {
			for (uint16_t i = 0; i < num_entries; i++) {
				set_cb(OBMF_GPIO_ENTRY_GET_INDEX(&entries[i]),
				       OBMF_GPIO_ENTRY_GET_DATA(&entries[i]),
				       set_ctx);
			}
		}

		OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr,
			OBMF_ICP_MSG_TYPE_RESPONSE, status);

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
	} else if (rc != 0 && rc != -ENETDOWN) {
		LOG_ERR("GPIO ch:%d write request failed: %d", channel, rc);
	}

	free(tx_buf);
	return rc;
}
