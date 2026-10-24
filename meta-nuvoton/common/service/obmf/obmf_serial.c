/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * OBMF Serial Channel
 *   Producer - Host sends serial data to us
 *   Consumer - We send serial data to host
 *
 * Supports multiple independent Serial channels (e.g. console on
 * OBMF_ICP_SERIAL_CHANNEL_0, a second interactive console such as an FPGA
 * UART on OBMF_ICP_SERIAL_CHANNEL_1). Each channel keeps its own state so
 * that one channel's traffic never blocks or corrupts another's (spec
 * §"Each channel is independent").
 */

#include "obmf_internal.h"
#include <sys/util.h>

LOG_MODULE_DECLARE(obmf);

/* Serial channel partial-accept retry (spec §partial-accept retry rule) */
#define OBMF_SERIAL_RSP_TIMEOUT_MS  1000  /* ms to wait for a serial response */

/* Known Serial channels, in the order their state lives in serial_chs[]. */
static const uint8_t serial_ch_nums[] = {
	OBMF_ICP_SERIAL_CHANNEL_0,
	OBMF_ICP_SERIAL_CHANNEL_1,
};

#define OBMF_SERIAL_CH_COUNT (ARRAY_SIZE(serial_ch_nums))

/*
 * Per-channel state. Everything that used to be a module-level singleton
 * (callbacks, response semaphore/result, send buffers) now lives here so
 * that two Serial channels can be in flight independently.
 */
struct serial_ch_state {
	obmf_serial_rsp_cb_t rsp_cb;
	void *rsp_cb_ctx;
	obmf_serial_rx_cb_t rx_cb;
	void *rx_cb_ctx;

	/*
	 * Semaphore and response state for synchronous send/response coupling
	 * in obmf_serial_send. Written by the OBMF RX dispatch thread
	 * (response path), read by this channel's sender thread; a semaphore
	 * provides the required happens-before ordering without extra
	 * locking. Each channel has its own sem, so one channel's sender
	 * never waits on another channel's response.
	 */
	struct k_sem rsp_sem;
	bool     last_was_error;
	uint8_t  last_ack_nack;
	uint16_t last_accepted;

	/*
	 * Per-channel send buffers: avoids per-call malloc and bounds stack
	 * usage. Each channel does its own stop-and-wait, so these are safe
	 * without additional locking as long as a single sender thread drives
	 * obmf_serial_send() for that channel.
	 */
	uint8_t staging[OBMF_SERIAL_MAX_CHARS];
	uint8_t tx_buf[OBMF_HEADER_SIZE + OBMF_SERIAL_REQ_HDR_SIZE +
		       OBMF_SERIAL_MAX_CHARS];
};

static struct serial_ch_state serial_chs[OBMF_SERIAL_CH_COUNT];

/*
 * Must run before any sender thread touches a channel: with several bridges
 * their sender threads race on a lazily initialized rsp_sem.
 */
void obmf_serial_channels_init(void)
{
	for (size_t i = 0; i < OBMF_SERIAL_CH_COUNT; i++)
		k_sem_init(&serial_chs[i].rsp_sem, 0, 1);
}

/* Find the state for a given Serial channel number. */
static struct serial_ch_state *serial_ch_by_num(uint8_t channel)
{
	for (size_t i = 0; i < OBMF_SERIAL_CH_COUNT; i++) {
		if (serial_ch_nums[i] == channel)
			return &serial_chs[i];
	}
	return NULL;
}

/* =====================================================================
 * Serial Channel - callbacks
 * ===================================================================== */

void obmf_register_serial_rsp_cb(uint8_t channel, obmf_serial_rsp_cb_t cb,
				 void *ctx)
{
	struct serial_ch_state *st = serial_ch_by_num(channel);

	if (!st) {
		LOG_ERR("obmf_register_serial_rsp_cb: unknown channel %d",
			channel);
		return;
	}
	st->rsp_cb = cb;
	st->rsp_cb_ctx = ctx;
}

void obmf_register_serial_rx_cb(uint8_t channel, obmf_serial_rx_cb_t cb,
				void *ctx)
{
	struct serial_ch_state *st = serial_ch_by_num(channel);

	if (!st) {
		LOG_ERR("obmf_register_serial_rx_cb: unknown channel %d",
			channel);
		return;
	}
	st->rx_cb = cb;
	st->rx_cb_ctx = ctx;
}

/* =====================================================================
 * Serial Channel (Producer) - Host sends serial data to us
 * ===================================================================== */

void handle_serial_request(obmf_icp_header_t *req_hdr,
			   uint8_t *payload, uint32_t payload_len,
			   uint8_t *rsp_buf, uint32_t *rsp_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(req_hdr);
	struct serial_ch_state *st = serial_ch_by_num(channel);

	if (!st) {
		LOG_ERR("handle_serial_request: unknown channel %d", channel);
		return;
	}

	if (payload_len >= OBMF_SERIAL_REQ_HDR_SIZE) {
		obmf_serial_request_t *serial_req =
			(obmf_serial_request_t *)payload;
		uint16_t char_count = serial_req->char_count;
		const uint8_t *char_data =
			payload + OBMF_SERIAL_REQ_HDR_SIZE;

		uint16_t accepted = char_count;

		if (char_count > 0 &&
		    payload_len >= OBMF_SERIAL_REQ_HDR_SIZE + char_count &&
		    st->rx_cb) {
			accepted = st->rx_cb(char_data, char_count,
					     st->rx_cb_ctx);
		}

		/* Build response (ACK or partial NACK per spec §partial-accept) */
		obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;
		OBMF_ICP_SET_CHANNEL(rsp_hdr, channel);
		OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr,
			OBMF_ICP_MSG_TYPE_RESPONSE,
			OBMF_ICP_SUCCESS);

		obmf_serial_response_t *serial_rsp =
			(obmf_serial_response_t *)(rsp_buf + OBMF_HEADER_SIZE);
		serial_rsp->ack_nack = (accepted < char_count) ? 1 : 0;
		serial_rsp->accepted_count = accepted;
		OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_SERIAL_RSP_SIZE);
		*rsp_len = OBMF_HEADER_SIZE + OBMF_SERIAL_RSP_SIZE;
	}
}

/* =====================================================================
 * Serial Channel (Consumer) - We send serial data to host
 * ===================================================================== */

/*
 * Signal an error response back to obmf_serial_send. Sets the shared
 * state variables, clears is_outstanding, and gives the semaphore.
 * Must only be called from handle_serial_response (OBMF RX thread).
 */
static void serial_signal_error(struct serial_ch_state *st, uint8_t channel)
{
	st->last_was_error = true;
	st->last_ack_nack  = 1;
	st->last_accepted  = 0;
	channel_tags[channel].is_outstanding = false;
	k_sem_give(&st->rsp_sem);
}

void handle_serial_response(obmf_icp_header_t *rsp_hdr,
			    uint8_t *payload, uint32_t payload_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(rsp_hdr);
	uint8_t status = OBMF_ICP_GET_STATUS(rsp_hdr);
	struct serial_ch_state *st = serial_ch_by_num(channel);

	if (!st) {
		LOG_ERR("handle_serial_response: unknown channel %d", channel);
		channel_tags[channel].is_outstanding = false;
		return;
	}

	if (status != OBMF_ICP_SUCCESS) {
		LOG_ERR("Serial response error: status=0x%x", status);
		serial_signal_error(st, channel);
		return;
	}

	if (payload_len < OBMF_SERIAL_RSP_SIZE) {
		LOG_ERR("Serial response too short (%d bytes)", payload_len);
		serial_signal_error(st, channel);
		return;
	}

	obmf_serial_response_t *rsp = (obmf_serial_response_t *)payload;

	st->last_was_error = false;
	st->last_ack_nack  = rsp->ack_nack & 0x01;
	st->last_accepted  = rsp->accepted_count;

	if (st->rsp_cb) {
		st->rsp_cb(rsp->ack_nack, rsp->accepted_count, st->rsp_cb_ctx);
	}

	/*
	 * Clear is_outstanding BEFORE signalling so that the retry inside
	 * obmf_serial_send can call obmf_send_request() immediately on
	 * wakeup without hitting -EBUSY.
	 */
	channel_tags[channel].is_outstanding = false;
	k_sem_give(&st->rsp_sem);
}

int obmf_serial_send(uint8_t channel, const uint8_t *data, uint16_t len,
		     obmf_serial_refill_cb_t refill, void *ctx)
{
	struct serial_ch_state *st = serial_ch_by_num(channel);

	if (!st) {
		LOG_ERR("obmf_serial_send: unknown channel %d", channel);
		return -EINVAL;
	}

	if (!data || len == 0)
		return 0;

	if (len > OBMF_SERIAL_MAX_CHARS)
		return -EMSGSIZE;

	/* Load initial payload into staging buffer */
	uint16_t buf_len = len;

	memcpy(st->staging, data, buf_len);

	/* Opportunistically append queued data into the first packet */
	if (refill) {
		uint16_t space = OBMF_SERIAL_MAX_CHARS - buf_len;

		if (space > 0)
			buf_len += refill(st->staging + buf_len, space, ctx);
	}

	int rc = 0;

	for (int attempt = 0; attempt <= OBMF_REQUEST_MAX_RETRY; attempt++) {
		uint32_t total_len =
			OBMF_HEADER_SIZE + OBMF_SERIAL_REQ_HDR_SIZE + buf_len;

		memset(st->tx_buf, 0, total_len);

		obmf_serial_request_t *req =
			(obmf_serial_request_t *)(st->tx_buf +
						  OBMF_HEADER_SIZE);
		req->operation_event = 0;
		req->char_count = buf_len;
		memcpy(st->tx_buf + OBMF_HEADER_SIZE + OBMF_SERIAL_REQ_HDR_SIZE,
		       st->staging, buf_len);

		/*
		 * Discard any leftover signal from a previous timed-out response
		 * before issuing the new send.
		 */
		k_sem_reset(&st->rsp_sem);

		int retries = OBMF_REQUEST_MAX_RETRY;

		do {
			rc = obmf_send_request(channel, st->tx_buf, total_len);
			if (rc == -EBUSY)
				k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
		} while (rc == -EBUSY && --retries > 0);

		if (rc != 0) {
			/* -ENETDOWN means the channel is already down: stay quiet */
			if (rc != -ENETDOWN) {
				LOG_ERR("Serial send request failed: %d"
					" (attempt %d, channel %d)",
					rc, attempt, channel);
			}
			break;
		}

		/* Block until handle_serial_response signals */
		int wait_rc = k_sem_take(&st->rsp_sem,
					 K_MSEC(OBMF_SERIAL_RSP_TIMEOUT_MS));

		if (wait_rc != 0) {
			LOG_WRN("Serial response timeout (attempt %d,"
				" channel %d)", attempt, channel);
			channel_tags[channel].is_outstanding = false;
			rc = -ETIMEDOUT;
			break;
		}

		if (st->last_was_error) {
			LOG_ERR("Serial response transport error (attempt %d,"
				" channel %d)", attempt, channel);
			rc = -EIO;
			break;
		}

		if (st->last_ack_nack == 0) {
			/* Full ACK – all bytes delivered */
			rc = 0;
			break;
		}

		/*
		 * Partial or full NACK. Advance past accepted bytes and retry
		 * from the first unaccepted byte (spec §partial-accept retry).
		 * Appending more queued data here avoids a wasted round-trip.
		 */
		uint16_t accepted = st->last_accepted;

		if (accepted >= buf_len) {
			/* Unexpected: NACK with accepted >= sent; treat as ACK */
			rc = 0;
			break;
		}

		uint16_t tail = buf_len - accepted;

		memmove(st->staging, st->staging + accepted, tail);
		buf_len = tail;

		if (refill) {
			uint16_t space = OBMF_SERIAL_MAX_CHARS - buf_len;

			if (space > 0)
				buf_len += refill(st->staging + buf_len,
						  space, ctx);
		}

		LOG_WRN("Serial partial NACK: channel=%d accepted=%u"
			" retry %d/%d next=%u", channel, accepted,
			attempt + 1, OBMF_REQUEST_MAX_RETRY + 1, buf_len);
		rc = -EAGAIN;
	}

	if (rc == -EAGAIN) {
		LOG_ERR("Serial send: channel %d gave up after %d attempts",
			channel, OBMF_REQUEST_MAX_RETRY + 1);
		rc = -EIO;
	}

	return rc;
}
