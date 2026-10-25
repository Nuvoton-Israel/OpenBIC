/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "obmf.h"
#include "obmf_internal.h"
#include <logging/log.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <class/usb_obmf.h>
#include <device.h>
#include "hal_vw_gpio.h"
#include <sys/ring_buffer.h>
#include <sys/atomic.h>

LOG_MODULE_REGISTER(obmf);

/* Largest Channel 0 (discovery structure) read/write, in bytes */
#define OBMF_CH0_MAX_TRANS_SIZE 64
#define OBMF_PRIMARY_ENDPOINT 0
#define OBMF_MIN_MSG_SIZE OBMF_HEADER_SIZE
/* Short transactions use a 32-bit address, Long a 64-bit one.  Reads
 * carry a 2-byte Read Length; writes carry no length field, the data runs
 * to the end of the message (OBMF-ICP 1.0.0 Tables 8, 10, 12, 14).
 */
#define OBMF_SHORT_ADDR_SIZE 4
#define OBMF_LONG_ADDR_SIZE 8
#define OBMF_SIZE_FIELD_LEN 2
#define OBMF_SHORT_READ_REQ_SIZE (OBMF_SHORT_ADDR_SIZE + OBMF_SIZE_FIELD_LEN)
#define OBMF_LONG_READ_REQ_SIZE (OBMF_LONG_ADDR_SIZE + OBMF_SIZE_FIELD_LEN)

/* Error handling macro */
#define OBMF_RETURN_ERR_IF(cond, fmt, ...) \
	do { \
		if (cond) { \
			LOG_ERR(fmt, ##__VA_ARGS__); \
			return -1; \
		} \
	} while (0)

// Forward declarations
int obmf_transport_send(uint8_t dest_endpoint, uint8_t *msg, uint32_t len);
static void obmf_reset_channel_state(void);
static void obmf_link_down_cb(const struct device *dev);
static void obmf_link_up_cb(const struct device *dev);

/*
 * Set from the USB status callback (ISR context) when the host connection is
 * lost (USB bus reset / disconnect). The RX thread consumes this flag at a
 * safe point and performs the actual channel-state reset.
 */
static atomic_t obmf_reset_pending = ATOMIC_INIT(0);

/* Thread stack for OBMF RX task */
#define OBMF_RX_THREAD_STACK_SIZE 4096
#define OBMF_RX_THREAD_PRIORITY 7

K_THREAD_STACK_DEFINE(obmf_rx_thread_stack, OBMF_RX_THREAD_STACK_SIZE);
static struct k_thread obmf_rx_thread_data;
static k_tid_t obmf_rx_thread_id;

/* Thread stack for OBMF TX/response handling */
#define OBMF_TX_THREAD_STACK_SIZE 2048
#define OBMF_TX_THREAD_PRIORITY 7

K_THREAD_STACK_DEFINE(obmf_tx_thread_stack, OBMF_TX_THREAD_STACK_SIZE);
static struct k_thread obmf_tx_thread_data;
static k_tid_t obmf_tx_thread_id;

/* Ring buffer for BULK OUT data (similar to MCTP) */
#define OBMF_RINGBUF_SIZE 4096
RING_BUF_DECLARE(obmf_ringbuf, OBMF_RINGBUF_SIZE);
static struct k_sem obmf_rx_sem;

/* Message queue for passing transport data to TX thread */
struct obmf_transport_msg {
	uint8_t *buf;
	uint32_t len;
};

/**
 * @brief Get the OBMF-ICP Channel Type for a given channel number.
 */
static inline uint8_t obmf_icp_get_channel_type(uint8_t channel)
{
	switch (channel) {
	case OBMF_ICP_CONTROL_CHANNEL:
		return OBMF_ICP_CHANNEL_TYPE_CONFIG;
	case OBMF_ICP_MMIO_CHANNEL:
		return OBMF_ICP_CHANNEL_TYPE_MMIO;
	case OBMF_ICP_SERIAL_CHANNEL_0:
	case OBMF_ICP_SERIAL_CHANNEL_1:
		return OBMF_ICP_CHANNEL_TYPE_SERIAL;
	case OBMF_ICP_IPMI_CHANNEL_NUM:
		return OBMF_ICP_CHANNEL_TYPE_IPMI;
	case OBMF_ICP_SPI_CHANNEL:
		return OBMF_ICP_CHANNEL_TYPE_SPI_CONTROLLER;
	case OBMF_ICP_MEM_DEV_CHANNEL:
		return OBMF_ICP_CHANNEL_TYPE_MEM_DEVICE;
	case OBMF_ICP_VW_GPIO_CHANNEL:
	case OBMF_ICP_SGPIO1_CHANNEL:
	case OBMF_ICP_SMC_GPIO_CHANNEL:
	case OBMF_ICP_SGPIO0_CHANNEL:
		return OBMF_ICP_CHANNEL_TYPE_GPIO;
	case OBMF_ICP_IO_CHANNEL:
		return OBMF_ICP_CHANNEL_TYPE_IO;
	case OBMF_ICP_I2C_CONTROLLER_CHANNEL_0:
	case OBMF_ICP_I2C_CONTROLLER_CHANNEL_1:
	case OBMF_ICP_I2C_CONTROLLER_CHANNEL_2:
	case OBMF_ICP_I2C_CONTROLLER_CHANNEL_3:
		return OBMF_ICP_CHANNEL_TYPE_I2C_CONTROLLER;
	case OBMF_ICP_I2C_TARGET_CHANNEL_0:
	case OBMF_ICP_I2C_TARGET_CHANNEL_1:
		return OBMF_ICP_CHANNEL_TYPE_I2C_TARGET;
	case OBMF_ICP_I3C_CONTROLLER_CHANNEL:
	case OBMF_ICP_I3C_CONTROLLER_CHANNEL_1:
		return OBMF_ICP_CHANNEL_TYPE_I3C_CONTROLLER;
	default:
		return OBMF_ICP_CHANNEL_TYPE_MMIO;
	}
}

K_MSGQ_DEFINE(obmf_tx_msgq, sizeof(struct obmf_transport_msg), OBMF_MAX_CHANNELS, 4);

/* OBMF TX/response handling thread */
static void obmf_tx_thread(void *arg1, void *arg2, void *arg3)
{
	struct obmf_transport_msg msg;
	
	while (1) {
		if (k_msgq_get(&obmf_tx_msgq, &msg, K_FOREVER) != 0) {
			continue;
		}

		if (!msg.buf || msg.len < OBMF_MIN_MSG_SIZE) {
			if (!msg.buf) {
				LOG_ERR("TX: null buffer");
			} else {
				LOG_ERR("TX: message too small: %d bytes", msg.len);
				free(msg.buf);
			}
			continue;
		}

		obmf_icp_header_t orig_hdr;
		memcpy(&orig_hdr, msg.buf, OBMF_HEADER_SIZE);

		uint8_t *payload = msg.buf + OBMF_HEADER_SIZE;
		uint32_t payload_len = msg.len - OBMF_HEADER_SIZE;

		/*
		 * Fragment the message if it exceeds MPS
		 * Similar to MCTP's packet splitting logic
		 */
		uint32_t max_payload_per_fragment =
			CONFIG_OBMF_BULK_EP_MPS - OBMF_HEADER_SIZE;
		uint32_t num_fragments =
			(payload_len + max_payload_per_fragment - 1)
			/ max_payload_per_fragment;

		/* A header-only message (e.g. an error response) is one packet */
		if (num_fragments == 0) {
			num_fragments = 1;
		}

		LOG_DBG("TX: total payload=%d, fragments=%d, max_per_frag=%d",
			payload_len, num_fragments, max_payload_per_fragment);

		bool tx_failed = false;

		for (uint32_t i = 0; i < num_fragments; i++) {
			uint8_t fragment_buf[CONFIG_OBMF_BULK_EP_MPS] = { 0 };
			obmf_icp_header_t *frag_hdr =
				(obmf_icp_header_t *)fragment_buf;

			/* Copy original header to this fragment */
			memcpy(frag_hdr, &orig_hdr, OBMF_HEADER_SIZE);

			/* Calculate payload size for this fragment */
			uint32_t offset = i * max_payload_per_fragment;
			uint32_t remaining = payload_len - offset;
			uint32_t frag_payload_len =
				(remaining < max_payload_per_fragment) ?
				remaining : max_payload_per_fragment;

			/*
			 * The first fragment keeps the original header size,
			 * which carries the *total* reassembled payload size.
			 * Subsequent fragments must advertise only their own
			 * payload size, otherwise the receiver mis-parses the
			 * packet length.
			 */
			if (i > 0) {
				OBMF_ICP_SET_SIZE(frag_hdr, frag_payload_len);
			}

			/* Copy payload for this fragment */
			memcpy(fragment_buf + OBMF_HEADER_SIZE,
			       payload + offset,
			       frag_payload_len);

			uint32_t total_frag_len =
				OBMF_HEADER_SIZE + frag_payload_len;

			LOG_DBG("TX fragment[%d/%d]: offset=%d,"
				" payload=%d, total=%d",
				i, num_fragments, offset,
				frag_payload_len, total_frag_len);

			/* Send this fragment */
			int ret = obmf_transport_send(OBMF_PRIMARY_ENDPOINT,
						      fragment_buf,
						      total_frag_len);
			if (ret != 0) {
				LOG_ERR("TX: failed to send fragment [%d/%d]: offset=%d, payload=%d",
					i, num_fragments, offset, frag_payload_len);
				tx_failed = true;
				break;
			}

			/*
			 * A fragment that is an exact multiple of MPS is
			 * followed by a zero-length packet, which
			 * usb_transfer_sync() adds itself, so the host still
			 * sees the end of each transfer.
			 */
		}

		if (tx_failed) {
			uint8_t ch = OBMF_ICP_GET_CHANNEL(&orig_hdr);

			/*
			 * This is the only place that knows the message never
			 * reached the host, so it is also the only place that can
			 * release a request whose response will never come.
			 */
			if (ch <= OBMF_MAX_CHANNELS &&
			    OBMF_ICP_GET_RQRESP(&orig_hdr) ==
			    OBMF_ICP_MSG_TYPE_REQUEST) {
				channel_tags[ch].is_outstanding = false;
			}
		} else {
			LOG_DBG("TX: completed sending %d fragments",
				num_fragments);
		}

		/* Free the dynamically allocated message buffer */
		free(msg.buf);
	}
}

/*
 * Fragment reassembly buffers for USB bulk transfers (per-channel).
 * Similar to MCTP's temp_msg_buf design, we use dynamic allocation
 * to avoid memory waste and prevent cross-channel contamination.
 */
#define OBMF_FRAG_BUF_SIZE 1024

typedef struct {
	uint8_t *buf;
	uint32_t offset;
	int64_t last_ms;
	uint8_t rqresp;
	bool discarding;
	uint32_t discard_seen;
	uint32_t discard_expected;
} obmf_frag_msg_buf;

/* A partial message older than this is abandoned */
#define OBMF_FRAG_TIMEOUT_MS 1000

static obmf_frag_msg_buf obmf_frag_bufs[OBMF_MAX_CHANNELS + 1];

/* Reassembly progress of a message addressed to a channel that does not exist */
static struct {
	bool active;
	uint8_t channel;
	uint8_t rqresp;
	int64_t last_ms;
	uint32_t seen;
	uint32_t expected;
} obmf_unk;

static void obmf_out_cb(const struct device *dev, uint32_t len, uint8_t *data);

/* Hand a response to the TX thread; the caller keeps @buf */
static void obmf_queue_response(const uint8_t *buf, uint32_t len)
{
	struct obmf_transport_msg msg;

	msg.buf = (uint8_t *)malloc(len);
	if (!msg.buf) {
		LOG_ERR("Failed to allocate buffer for OBMF response (%d bytes)",
			len);
		return;
	}
	memcpy(msg.buf, buf, len);
	msg.len = len;

	if (k_msgq_put(&obmf_tx_msgq, &msg, K_NO_WAIT) != 0) {
		LOG_WRN("Failed to queue OBMF response, queue full");
		free(msg.buf);
	}
}

/*
 * USB bus-reset / disconnect notification.
 *
 * Runs in USB interrupt context, so it must stay ISR-safe: just flag the
 * event and let the RX thread perform the real cleanup (freeing fragment
 * buffers and clearing outstanding requests) at a safe point before
 * processing the next request.
 */
static void obmf_bus_reset_cb(const struct device *dev)
{
	ARG_UNUSED(dev);
	atomic_set(&obmf_reset_pending, 1);
}

/* Let the USB layer NAK the host instead of dropping what does not fit */
static uint32_t obmf_rx_space_cb(const struct device *dev)
{
	ARG_UNUSED(dev);
	return ring_buf_space_get(&obmf_ringbuf);
}

static struct obmf_ops obmf_usb_ops = {
	.read = obmf_out_cb,
	.rx_space = obmf_rx_space_cb,
	.bus_reset = obmf_bus_reset_cb,
	.link_down = obmf_link_down_cb,
	.link_up = obmf_link_up_cb,
};

/*
 * Read helper: returns number of bytes available in ring buffer
 * Similar to mctp_ringbuf_read behavior
 */
static uint16_t obmf_ringbuf_read(uint8_t *buf, uint32_t len)
{
	uint16_t ret = 0;
	uint32_t rx_len = 0;
	uint8_t hdr_buf[OBMF_HEADER_SIZE];
	obmf_icp_header_t *hdr;
	uint32_t msg_len, payload_len, got;

	rx_len = ring_buf_capacity_get(&obmf_ringbuf)
		 - ring_buf_space_get(&obmf_ringbuf);
	if (rx_len == 0)
		return 0;

	/* A valid OBMF packet must contain at least the common header */
	if (rx_len < OBMF_HEADER_SIZE) {
		LOG_ERR("%s: invalid packet in ring buffer (%u bytes)",
			__func__, rx_len);
		/* Drain the garbage so the reader does not get stuck */
		ring_buf_get(&obmf_ringbuf, hdr_buf, rx_len);
		obmf_usb_rx_resume(NULL);
		return 0;
	}

	/*
	 * Read the OBMF common header first to learn how long this packet
	 * is (similar to mctp_ringbuf_read using mctp_usb_hdr->len). This
	 * lets us re-frame a single packet out of the ring buffer even
	 * when several packets have been coalesced by the ISR.
	 */
	ring_buf_get(&obmf_ringbuf, hdr_buf, OBMF_HEADER_SIZE);
	hdr = (obmf_icp_header_t *)hdr_buf;

	/* Total length of this logical message (single-fragment case) */
	msg_len = OBMF_HEADER_SIZE + hdr->size;

	/*
	 * Cap to a single USB packet (MPS). Multi-fragment messages carry
	 * the *total* payload size in every fragment header, so one USB
	 * packet never exceeds MPS. Capping here rebuilds the USB packet
	 * boundary that the ring buffer loses when packets are coalesced.
	 */
	if (msg_len > CONFIG_OBMF_BULK_EP_MPS)
		msg_len = CONFIG_OBMF_BULK_EP_MPS;

	/* Never consume more than what is actually buffered */
	if (msg_len > rx_len)
		msg_len = rx_len;

	payload_len = msg_len - OBMF_HEADER_SIZE;

	/* Caller buffer must be able to hold at least the header */
	if (len < OBMF_HEADER_SIZE) {
		LOG_ERR("%s: caller buffer too small (%u bytes)", __func__,
			len);
		return 0;
	}
	if (payload_len > (len - OBMF_HEADER_SIZE))
		payload_len = len - OBMF_HEADER_SIZE;

	/* Assemble header + payload into the caller buffer */
	memcpy(buf, hdr_buf, OBMF_HEADER_SIZE);
	got = ring_buf_get(&obmf_ringbuf, buf + OBMF_HEADER_SIZE, payload_len);

	ret = (uint16_t)(OBMF_HEADER_SIZE + got);
	obmf_usb_rx_resume(NULL);
	return ret;
}

/*
 * Blocking read: try to read existing data first, otherwise wait on semaphore
 * Similar to mctp_usb_read - ensures we consume pre-existing ring buffer data
 * immediately without relying solely on semaphore
 */
static uint16_t obmf_usb_read(uint8_t *buf, uint32_t len)
{
	uint16_t rt_size = 0;

	/* First try to read from ring buffer without blocking */
	rt_size = obmf_ringbuf_read(buf, len);

	/* If no data available, wait for semaphore and retry */
	while (rt_size == 0) {
		k_sem_take(&obmf_rx_sem, K_FOREVER);
		rt_size = obmf_ringbuf_read(buf, len);
	}

	return rt_size;
}

/*
 * OBMF BULK OUT callback (ISR context)
 * Quick put to ring buffer and signal RX task, similar to mctp_usb_rx
 */
static void obmf_out_cb(const struct device *dev, uint32_t len, uint8_t *data)
{
	int wrote;

	/* Put data to ring buffer (even if len == 0 for ZLP handling) */
	wrote = ring_buf_put(&obmf_ringbuf, data, len);
	if (wrote < len) {
		LOG_WRN("OBMF ring buffer full, drop %d bytes", len - wrote);
	}

	/* Signal RX task that data is available */
	k_sem_give(&obmf_rx_sem);
}

/*
 * OBMF RX task - process BULK OUT data from ring buffer
 * Similar to mctp_rx_task
 */
static void obmf_rx_task(void *arg1, void *arg2, void *arg3)
{
	LOG_DBG("OBMF RX task started");

	while (1) {
		uint8_t fragment_buf[CONFIG_OBMF_BULK_EP_MPS] = { 0 };
		uint16_t fragment_len;
		bool is_complete = false;
		uint8_t channel;

		/*
		 * Read fragment using blocking read that first checks ring
		 * buffer, then waits on semaphore if needed. This ensures we
		 * process any pre-existing data immediately on startup or
		 * when scheduled.
		 */
		fragment_len =
			obmf_usb_read(fragment_buf, sizeof(fragment_buf));

		/*
		 * Handle a pending USB bus reset / disconnect (host reboot).
		 * Do it here, in thread context, before parsing the freshly
		 * read fragment: this frees any stale partial reassembly
		 * buffers and outstanding requests so the new fragment is
		 * processed as the first message after reconnect.
		 */
		if (atomic_cas(&obmf_reset_pending, 1, 0)) {
			obmf_reset_channel_state();
		}

		if (fragment_len == 0) {
			/*
			 * Should not happen with our read implementation,
			 * but handle gracefully
			 */
			LOG_WRN("OBMF RX task: read returned 0");
			k_sleep(K_USEC(1));
			continue;
		}

		/* Parse OBMF common header to get channel */
		if (fragment_len < OBMF_HEADER_SIZE) {
			LOG_ERR("OBMF fragment too small: %u bytes",
				fragment_len);
			k_yield();
			continue;
		}

		obmf_icp_header_t *hdr = (obmf_icp_header_t *)fragment_buf;
		channel = OBMF_ICP_GET_CHANNEL(hdr);

		if (channel > OBMF_MAX_CHANNELS) {
			LOG_ERR("Invalid OBMF channel: %u", channel);
			LOG_HEXDUMP_ERR(fragment_buf, fragment_len,
						 "Invalid fragment data:");
			/*
			 * Track the message by its Size field so that it is
			 * answered once, on its last fragment, even when that
			 * fragment fills a whole packet.
			 */
			int64_t unk_now = k_uptime_get();
			bool unk_cont = obmf_unk.active &&
					obmf_unk.channel == channel &&
					obmf_unk.rqresp == OBMF_ICP_GET_RQRESP(hdr) &&
					unk_now - obmf_unk.last_ms <=
						OBMF_FRAG_TIMEOUT_MS;

			if (!unk_cont) {
				obmf_unk.channel = channel;
				obmf_unk.rqresp = OBMF_ICP_GET_RQRESP(hdr);
				obmf_unk.expected = OBMF_ICP_GET_SIZE(hdr);
				obmf_unk.seen = 0;
			}
			obmf_unk.last_ms = unk_now;
			obmf_unk.seen += fragment_len - OBMF_HEADER_SIZE;
			obmf_unk.active = true;

			if (obmf_unk.seen >= obmf_unk.expected ||
			    fragment_len < CONFIG_OBMF_BULK_EP_MPS) {
				obmf_unk.active = false;
				if (OBMF_ICP_GET_RQRESP(hdr) ==
				    OBMF_ICP_MSG_TYPE_REQUEST) {
					uint8_t rsp_buf[OBMF_HEADER_SIZE];
					uint32_t rsp_len = 0;

					obmf_build_err_response(hdr,
						OBMF_ICP_ERR_UNKNOWN_CHANNEL,
						rsp_buf, &rsp_len, false);
					obmf_queue_response(rsp_buf, rsp_len);
				}
			}
			k_yield();
			continue;
		}

		obmf_frag_msg_buf *frag_buf = &obmf_frag_bufs[channel];

		/*
		 * Drop a partial message that went stale, or whose next
		 * fragment carries a different RqResp bit, and restart with
		 * this fragment as a new message.
		 */
		int64_t now_ms = k_uptime_get();
		bool mid_msg = frag_buf->buf || frag_buf->discarding;

		if (mid_msg &&
		    (now_ms - frag_buf->last_ms > OBMF_FRAG_TIMEOUT_MS ||
		     frag_buf->rqresp != OBMF_ICP_GET_RQRESP(hdr))) {
			LOG_WRN("OBMF channel %u: dropping partial message",
				channel);
			free(frag_buf->buf);
			frag_buf->buf = NULL;
			frag_buf->offset = 0;
			frag_buf->discarding = false;
		}
		frag_buf->last_ms = now_ms;

		/* Skip the rest of a message that overflowed the buffer */
		if (frag_buf->discarding) {
			frag_buf->discard_seen += fragment_len - OBMF_HEADER_SIZE;
			if (frag_buf->discard_seen >= frag_buf->discard_expected ||
			    fragment_len < CONFIG_OBMF_BULK_EP_MPS) {
				frag_buf->discarding = false;
				if (OBMF_ICP_GET_RQRESP(hdr) ==
				    OBMF_ICP_MSG_TYPE_REQUEST) {
					uint8_t rsp_buf[OBMF_HEADER_SIZE];
					uint32_t rsp_len = 0;

					obmf_build_err_response(hdr,
						OBMF_ICP_STATUS_SIZE_NOT_SUPPORTED,
						rsp_buf, &rsp_len, false);
					obmf_queue_response(rsp_buf, rsp_len);
				}
			}
			k_yield();
			continue;
		}

		/* First fragment (SOM): allocate buffer */
		if (!frag_buf->buf) {
			frag_buf->buf = (uint8_t *)malloc(OBMF_FRAG_BUF_SIZE);
			if (!frag_buf->buf) {
				LOG_ERR("Failed to allocate fragment buffer"
					" for channel %u", channel);
				k_yield();
				continue;
			}
			memset(frag_buf->buf, 0, OBMF_FRAG_BUF_SIZE);
			frag_buf->offset = 0;
			frag_buf->rqresp = OBMF_ICP_GET_RQRESP(hdr);
			LOG_DBG("OBMF SOM: allocated fragment buffer for"
				" channel %u", channel);
		}

		/* Check buffer overflow */
		if (frag_buf->offset + fragment_len > OBMF_FRAG_BUF_SIZE) {
			obmf_icp_header_t *fh = (obmf_icp_header_t *)frag_buf->buf;

			LOG_ERR("OBMF fragment buffer overflow on channel"
				" %u: current %u + new %u > %u",
				channel, frag_buf->offset,
				fragment_len, OBMF_FRAG_BUF_SIZE);
			frag_buf->discard_expected = OBMF_ICP_GET_SIZE(fh);
			frag_buf->discard_seen = frag_buf->offset -
				OBMF_HEADER_SIZE +
				fragment_len - OBMF_HEADER_SIZE;
			frag_buf->discarding = true;
			free(frag_buf->buf);
			frag_buf->buf = NULL;
			frag_buf->offset = 0;
			if (frag_buf->discard_seen >= frag_buf->discard_expected ||
			    fragment_len < CONFIG_OBMF_BULK_EP_MPS) {
				frag_buf->discarding = false;
				if (OBMF_ICP_GET_RQRESP(hdr) ==
				    OBMF_ICP_MSG_TYPE_REQUEST) {
					uint8_t rsp_buf[OBMF_HEADER_SIZE];
					uint32_t rsp_len = 0;

					obmf_build_err_response(hdr,
						OBMF_ICP_STATUS_SIZE_NOT_SUPPORTED,
						rsp_buf, &rsp_len, false);
					obmf_queue_response(rsp_buf, rsp_len);
				}
			}
			k_yield();
			continue;
		}

		/* Append current fragment */
		if (frag_buf->offset == 0) {
			/* First fragment: copy entire data including header */
			memcpy(frag_buf->buf + frag_buf->offset,
			       fragment_buf, fragment_len);
			frag_buf->offset += fragment_len;
		} else {
			/* Sub fragments: skip header, copy only payload */
			uint32_t payload_len = fragment_len - OBMF_HEADER_SIZE;
			memcpy(frag_buf->buf + frag_buf->offset,
			       fragment_buf + OBMF_HEADER_SIZE,
			       payload_len);
			frag_buf->offset += payload_len;
		}

		/*
		 * Detect end of message (EOM).  The first fragment's Size
		 * field carries the total payload, so the message is complete
		 * once that many bytes arrived; this also covers a message
		 * that exactly fills the last packet and so has no short
		 * packet.  A short packet always ends it as well.
		 */
		obmf_icp_header_t *first_hdr = (obmf_icp_header_t *)frag_buf->buf;
		uint32_t expected_len =
			OBMF_HEADER_SIZE + OBMF_ICP_GET_SIZE(first_hdr);

		if (frag_buf->offset >= expected_len ||
		    fragment_len < CONFIG_OBMF_BULK_EP_MPS) {
			is_complete = true;
			LOG_DBG("OBMF EOM detected on channel %u:"
				" fragment len %u, total %u of %u bytes",
				channel, fragment_len, frag_buf->offset,
				expected_len);
		} else {
			LOG_DBG("OBMF fragment on channel %u: len %u,"
				" accumulated %u of %u bytes (waiting for more)",
				channel, fragment_len, frag_buf->offset,
				expected_len);
		}

		/* Process complete message */
		if (is_complete) {
			uint8_t rsp_buf[OBMF_RSP_BUF_SIZE];
			uint32_t rsp_len = 0;
			obmf_get_response(frag_buf->buf, frag_buf->offset,
					  rsp_buf, &rsp_len);

			if (rsp_len > 0) {
				/* Send response to TX thread for handling */
				obmf_queue_response(rsp_buf, rsp_len);
			}

			/* Free fragment buffer (EOM cleanup) */
			free(frag_buf->buf);
			frag_buf->buf = NULL;
			frag_buf->offset = 0;
		}
		k_yield();
	}
}

// --- OBMF Transport Layer ---
int obmf_transport_send(uint8_t dest_endpoint, uint8_t *msg, uint32_t len)
{
	const struct device *dev =
		device_get_binding(CONFIG_USB_OBMF_DEVICE_NAME "_0");
	uint32_t bytes_written = 0;

	if (!dev) {
		LOG_ERR("Failed to get OBMF USB device");
		return -1;
	}

	if (obmf_usb_ep_write(dev, msg, len, &bytes_written) != 0) {
		LOG_ERR("obmf_usb_ep_write failed");
		return -1;
	}

	if (bytes_written != len) {
		LOG_ERR("Incomplete write: %d vs %d", bytes_written, len);
		return -1;
	}

	return 0;
}

// --- Channel 0 (Producer) Data ---
static obmf_discovery_structure_t discovery_struct;

OBMF_Channel_Tags channel_tags[OBMF_MAX_CHANNELS + 1] = {0};

/* Channels that were host-enabled when the USB link went down */
static uint32_t obmf_suspended_mask;

/*
 * Reset all per-channel state after a USB bus reset / disconnect (e.g. a host
 * reboot). Frees any in-progress fragment reassembly buffers and forgets
 * requests that will never be answered.
 *
 * Called from the RX thread context only (never from the ISR callback).
 */
static void obmf_reset_channel_state(void)
{
	for (int i = 0; i < OBMF_MAX_CHANNELS + 1; i++) {
		if (obmf_frag_bufs[i].buf) {
			free(obmf_frag_bufs[i].buf);
			obmf_frag_bufs[i].buf = NULL;
		}
		obmf_frag_bufs[i].offset = 0;
		obmf_frag_bufs[i].discarding = false;

		channel_tags[i].is_outstanding = false;
	}

	obmf_unk.active = false;

	/* The host re-runs discovery after a reset, so drop any pending restore */
	obmf_suspended_mask = 0;

	LOG_WRN("OBMF channel state reset (host connection lost)");
}

/*
 * Locate a channel's configuration space header inside the discovery
 * structure. Returns NULL for Channel 0 and unknown channel numbers.
 */
static obmf_channel_config_header_t *obmf_chan_cfg_hdr(uint8_t channel)
{
	switch (channel) {
	case OBMF_ICP_MMIO_CHANNEL:
		return &discovery_struct.mmio_channel_config.header;
	case OBMF_ICP_SERIAL_CHANNEL_0:
	case OBMF_ICP_SERIAL_CHANNEL_1:
		return &discovery_struct.serial_channel_configs[
			OBMF_SERIAL_CHAN_IDX(channel)].header;
	case OBMF_ICP_IPMI_CHANNEL_NUM:
		return &discovery_struct.ipmi_channel_config.header;
	case OBMF_ICP_SPI_CHANNEL:
		return &discovery_struct.spi_channel_config.header;
	case OBMF_ICP_MEM_DEV_CHANNEL:
		return &discovery_struct.mem_dev_channel_config.header;
	case OBMF_ICP_VW_GPIO_CHANNEL:
		return &discovery_struct.gpio_channel_config.header;
	case OBMF_ICP_SGPIO1_CHANNEL:
		return &discovery_struct.sgpio1_channel_config.header;
	case OBMF_ICP_SMC_GPIO_CHANNEL:
		return &discovery_struct.smc_gpio_channel_config.header;
	case OBMF_ICP_SGPIO0_CHANNEL:
		return &discovery_struct.sgpio0_channel_config.header;
	case OBMF_ICP_IO_CHANNEL:
		return &discovery_struct.io_channel_config.header;
	case OBMF_ICP_I2C_CONTROLLER_CHANNEL_0:
	case OBMF_ICP_I2C_CONTROLLER_CHANNEL_1:
	case OBMF_ICP_I2C_CONTROLLER_CHANNEL_2:
	case OBMF_ICP_I2C_CONTROLLER_CHANNEL_3:
		return &discovery_struct.i2c_channel_configs[
			OBMF_I2C_CTRL_CHAN_IDX(channel)].header;
	case OBMF_ICP_I2C_TARGET_CHANNEL_0:
	case OBMF_ICP_I2C_TARGET_CHANNEL_1:
		return &discovery_struct.i2c_target_channel_configs[
			OBMF_I2C_TGT_CHAN_IDX(channel)].header;
	case OBMF_ICP_I3C_CONTROLLER_CHANNEL:
		return &discovery_struct.i3c_channel_config.header;
	case OBMF_ICP_I3C_CONTROLLER_CHANNEL_1:
		return &discovery_struct.i3c1_channel_config.header;
	default:
		return NULL;
	}
}

const obmf_gpio_config_data_t *obmf_gpio_channel_config(uint8_t channel)
{
	obmf_channel_config_header_t *hdr = obmf_chan_cfg_hdr(channel);

	if (!hdr || hdr->channel_type != OBMF_ICP_CHANNEL_TYPE_GPIO) {
		return NULL;
	}
	return (const obmf_gpio_config_data_t *)
	       ((uint8_t *)hdr + CHAN_CFG_CONFIG_DATA_OFFSET);
}

int obmf_i3c_ibi_target_set(uint8_t channel, uint8_t dynamic_addr,
			     uint16_t max_ibi_payload_len)
{
	obmf_channel_config_header_t *hdr = obmf_chan_cfg_hdr(channel);
	obmf_i3c_config_data_t *cfg;
	uint8_t n;

	if (!hdr || hdr->channel_type != OBMF_ICP_CHANNEL_TYPE_I3C_CONTROLLER) {
		return -EINVAL;
	}
	cfg = (obmf_i3c_config_data_t *)((uint8_t *)hdr +
					 CHAN_CFG_CONFIG_DATA_OFFSET);
	n = MIN(cfg->ibi_target_count, OBMF_I3C_MAX_IBI_TARGETS);

	for (uint8_t i = 0; i < n; i++) {
		if (cfg->ibi_targets[i].dynamic_addr == dynamic_addr) {
			cfg->ibi_targets[i].max_ibi_payload_len =
				max_ibi_payload_len;
			return 0;
		}
	}
	if (n == OBMF_I3C_MAX_IBI_TARGETS) {
		return -ENOMEM;
	}

	cfg->ibi_targets[n] = (obmf_i3c_ibi_target_cfg_t) {
		.dynamic_addr = dynamic_addr,
		.max_ibi_payload_len = max_ibi_payload_len,
	};
	cfg->ibi_target_count = n + 1;
	hdr->config_size = OBMF_I3C_CONFIG_SIZE(n + 1);
	return 0;
}

bool obmf_channel_is_enabled(uint8_t channel)
{
	obmf_channel_config_header_t *hdr = obmf_chan_cfg_hdr(channel);

	return hdr && (hdr->channel_control & CHAN_CONTROL_ENABLE);
}

void obmf_channel_specific_status_set(uint8_t channel, uint8_t bits, bool set)
{
	obmf_channel_config_header_t *hdr = obmf_chan_cfg_hdr(channel);

	if (!hdr) {
		return;
	}
	if (set) {
		hdr->channel_specific_status |= bits;
	} else {
		hdr->channel_specific_status &= ~bits;
	}
}

/*
 * Outstanding requests are deliberately left untouched: a plain USB suspend
 * may be followed by a resume without a bus reset.
 */
static void obmf_channels_link_down(void)
{
	obmf_suspended_mask = 0;

	for (uint8_t ch = 1; ch <= OBMF_MAX_CHANNELS; ch++) {
		obmf_channel_config_header_t *hdr = obmf_chan_cfg_hdr(ch);

		if (!hdr || !(hdr->channel_control & CHAN_CONTROL_ENABLE)) {
			continue;
		}

		obmf_suspended_mask |= BIT(ch);
		hdr->channel_control &= ~CHAN_CONTROL_ENABLE;
		hdr->channel_status = CHAN_HEALTH_NOT_READY;
		channel_tags[ch].is_outstanding = false;
	}

	/*
	 * Drop anything already queued for TX: it would otherwise still be
	 * sent fragment-by-fragment, each one blocking on the USB write
	 * timeout with no host present to read it.
	 */
	struct obmf_transport_msg queued_msg;

	while (k_msgq_get(&obmf_tx_msgq, &queued_msg, K_NO_WAIT) == 0) {
		free(queued_msg.buf);
	}

	if (obmf_suspended_mask) {
		LOG_WRN("OBMF link down: channels 0x%x disabled",
			obmf_suspended_mask);
	}
}

/*
 * Re-enable exactly the channels taken down by obmf_channels_link_down().
 * The host never learns about the suspend, so it will not re-issue the
 * CHANNEL_CONTROL writes itself.
 */
static void obmf_channels_link_up(void)
{
	if (!obmf_suspended_mask) {
		return;
	}

	for (uint8_t ch = 1; ch <= OBMF_MAX_CHANNELS; ch++) {
		obmf_channel_config_header_t *hdr;

		if (!(obmf_suspended_mask & BIT(ch))) {
			continue;
		}

		hdr = obmf_chan_cfg_hdr(ch);
		if (hdr) {
			hdr->channel_control |= CHAN_CONTROL_ENABLE;
			hdr->channel_status = CHAN_HEALTH_READY |
					      CHAN_STATUS_CONFIG_VALID;
		}
	}

	LOG_WRN("OBMF link up: channels 0x%x restored", obmf_suspended_mask);
	obmf_suspended_mask = 0;
}

static void obmf_link_down_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	obmf_channels_link_down();
}

static void obmf_link_up_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	obmf_channels_link_up();
}

static K_WORK_DEFINE(obmf_link_down_work, obmf_link_down_work_handler);
static K_WORK_DEFINE(obmf_link_up_work, obmf_link_up_work_handler);

/*
 * The RX thread blocks forever inside obmf_usb_read() when no data arrives,
 * so link state changes must run on the system workqueue instead. Sharing one
 * workqueue also serialises link down against link up.
 */
static void obmf_link_down_cb(const struct device *dev)
{
	ARG_UNUSED(dev);
	k_work_submit(&obmf_link_down_work);
}

static void obmf_link_up_cb(const struct device *dev)
{
	ARG_UNUSED(dev);
	k_work_submit(&obmf_link_up_work);
}

/* GPIO config accessors for platform init */
obmf_gpio_config_entry_t *obmf_gpio_get_config_entries(void)
{
	return discovery_struct.gpio_channel_config.config.entries;
}

uint16_t obmf_gpio_get_config_count(void)
{
	return discovery_struct.gpio_channel_config.config.gpio_count;
}

/* SGPIO1 config accessors for platform init */
obmf_gpio_config_entry_t *obmf_sgpio1_get_config_entries(void)
{
	return discovery_struct.sgpio1_channel_config.config.entries;
}

uint16_t obmf_sgpio1_get_config_count(void)
{
	return discovery_struct.sgpio1_channel_config.config.gpio_count;
}

void obmf_sgpio1_set_config_count(uint16_t count)
{
	if (count > OBMF_GPIO_MAX_ENTRIES) {
		count = OBMF_GPIO_MAX_ENTRIES;
	}
	discovery_struct.sgpio1_channel_config.config.gpio_count = count;
}

/* SGPIO legacy accessors (equivalent to SGPIO1) */
obmf_gpio_config_entry_t *obmf_sgpio_get_config_entries(void)
{
	return obmf_sgpio1_get_config_entries();
}

uint16_t obmf_sgpio_get_config_count(void)
{
	return obmf_sgpio1_get_config_count();
}

/* SGPIO0 config accessors for platform init */
obmf_gpio_config_entry_t *obmf_sgpio0_get_config_entries(void)
{
	return discovery_struct.sgpio0_channel_config.config.entries;
}

uint16_t obmf_sgpio0_get_config_count(void)
{
	return discovery_struct.sgpio0_channel_config.config.gpio_count;
}

void obmf_sgpio0_set_config_count(uint16_t count)
{
	if (count > OBMF_GPIO_MAX_ENTRIES) {
		count = OBMF_GPIO_MAX_ENTRIES;
	}
	discovery_struct.sgpio0_channel_config.config.gpio_count = count;
}

/* Platform GPIO config accessors for platform init */
obmf_gpio_config_entry_t *obmf_smc_gpio_get_config_entries(void)
{
	return discovery_struct.smc_gpio_channel_config.config.entries;
}

uint16_t obmf_smc_gpio_get_config_count(void)
{
	return discovery_struct.smc_gpio_channel_config.config.gpio_count;
}

void obmf_smc_gpio_set_config_count(uint16_t count)
{
	if (count > OBMF_GPIO_MAX_ENTRIES) {
		count = OBMF_GPIO_MAX_ENTRIES;
	}
	discovery_struct.smc_gpio_channel_config.config.gpio_count = count;
}

int obmf_i2c_target_set_address(uint8_t channel, uint8_t address)
{
	if (channel < OBMF_ICP_I2C_TARGET_CHANNEL_0 ||
	    channel >= OBMF_I2C_TGT_CHAN_NUM(OBMF_MAX_I2C_TARGET_CHANNELS)) {
		return -EINVAL;
	}
	if (!obmf_channel_is_present(channel)) {
		return -ENODEV;
	}

	obmf_i2c_target_channel_config_t *tgt =
		&discovery_struct.i2c_target_channel_configs[OBMF_I2C_TGT_CHAN_IDX(channel)];

	tgt->config.target_address_list[0] = address;
	tgt->config.target_address_count = 1;
	tgt->header.config_size =
		offsetof(obmf_i2c_target_config_data_t, target_address_list) +
		tgt->config.target_address_count;

	return 0;
}

// --- Forward Declarations ---
static void handle_ch0_read_req(obmf_icp_header_t *req_hdr,
				uint8_t *req_payload, uint32_t req_len,
				uint8_t *rsp_buf, uint32_t *rsp_len);
static void handle_ch0_write_req(obmf_icp_header_t *req_hdr,
				 uint8_t *req_payload, uint32_t req_len,
				 uint8_t *rsp_buf, uint32_t *rsp_len);

// --- Public API --- 

void obmf_build_err_response(obmf_icp_header_t *req_hdr,
			     uint8_t error_code,
			     uint8_t *rsp_buf, uint32_t *rsp_len,
			     bool alt)
{
	obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;
	uint8_t channel = OBMF_ICP_GET_CHANNEL(req_hdr);

	ARG_UNUSED(alt);

	/* Build common header for response */
	OBMF_ICP_SET_CHANNEL(rsp_hdr, channel);
	/* v1.0.0 RC1: Status code goes in common header byte 1[6:0] */
	OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr, OBMF_ICP_MSG_TYPE_RESPONSE,
				   error_code);

	if (channel <= OBMF_ICP_MMIO_CHANNEL) {
		/* Channel 0 / MMIO: echo the MMIO sub-header, no payload */
		obmf_icp_mmio_sub_header_t *req_sub =
			(obmf_icp_mmio_sub_header_t *)((uint8_t *)req_hdr + OBMF_HEADER_SIZE);
		obmf_icp_mmio_sub_header_t *rsp_sub =
			(obmf_icp_mmio_sub_header_t *)(rsp_buf + OBMF_HEADER_SIZE);

		obmf_mmio_rsp_sub_header(rsp_sub, req_sub);
		OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_MMIO_SUB_HDR_SIZE);
		*rsp_len = OBMF_FULL_MMIO_HDR_SIZE;
	} else if (channel == OBMF_ICP_IO_CHANNEL) {
		/* I/O channel: echo the transaction, reserved bits zero */
		obmf_icp_io_sub_header_t *req_sub =
			(obmf_icp_io_sub_header_t *)((uint8_t *)req_hdr + OBMF_HEADER_SIZE);
		obmf_icp_io_sub_header_t *rsp_sub =
			(obmf_icp_io_sub_header_t *)(rsp_buf + OBMF_HEADER_SIZE);

		rsp_sub->trans_rsvd = OBMF_ICP_IO_GET_TRANS(req_sub);
		OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_ICP_IO_SUB_HDR_SIZE);
		*rsp_len = OBMF_FULL_IO_HDR_SIZE;
	} else {
		/* All other channels have no sub-header - error responses carry no payload */
		OBMF_ICP_SET_SIZE(rsp_hdr, 0);
		*rsp_len = OBMF_HEADER_SIZE;
	}
}

void obmf_stop(void)
{
	if (obmf_rx_thread_id) {
		k_thread_abort(obmf_rx_thread_id);
		obmf_rx_thread_id = NULL;
	}
	if (obmf_tx_thread_id) {
		k_thread_abort(obmf_tx_thread_id);
		obmf_tx_thread_id = NULL;
	}
}

/* Channels configured by the platform; bit N = channel N */
static uint32_t obmf_present_mask = BIT(OBMF_ICP_CONTROL_CHANNEL);

bool obmf_channel_is_present(uint8_t channel)
{
	return channel <= OBMF_MAX_CHANNELS &&
	       (obmf_present_mask & BIT(channel));
}

/* The direction of each channel is fixed by what the service implements */
static uint8_t obmf_channel_device_role(uint8_t channel)
{
	switch (channel) {
	case OBMF_ICP_IPMI_CHANNEL_NUM:
	case OBMF_ICP_MEM_DEV_CHANNEL:
	case OBMF_ICP_I2C_TARGET_CHANNEL_0:
	case OBMF_ICP_I2C_TARGET_CHANNEL_1:
		return OBMF_CH_DEVICE_ROLE_CONSUMER; /* SMC requests, BMC responds */
	default:
		return OBMF_CH_DEVICE_ROLE_PRODUCER;
	}
}

/*
 * Fill one channel's configuration space from a platform table entry.
 * Returns the CONFIG_SIZE to advertise, or a negative errno.
 */
static int obmf_channel_config_fill(const struct obmf_plat_channel *pc,
				    uint8_t *data)
{
	uint8_t ch = pc->channel;

	switch (obmf_icp_get_channel_type(ch)) {
	case OBMF_ICP_CHANNEL_TYPE_MMIO:
		memcpy(data, &pc->cfg.mmio, sizeof(pc->cfg.mmio));
		return sizeof(pc->cfg.mmio);
	case OBMF_ICP_CHANNEL_TYPE_SERIAL:
		memcpy(data, &pc->cfg.serial, sizeof(pc->cfg.serial));
		return sizeof(pc->cfg.serial);
	case OBMF_ICP_CHANNEL_TYPE_IPMI:
		return 0;
	case OBMF_ICP_CHANNEL_TYPE_SPI_CONTROLLER:
		memcpy(data, &pc->cfg.spi, sizeof(pc->cfg.spi));
		return sizeof(pc->cfg.spi);
	case OBMF_ICP_CHANNEL_TYPE_MEM_DEVICE:
		memcpy(data, &pc->cfg.mem_dev, sizeof(pc->cfg.mem_dev));
		return sizeof(pc->cfg.mem_dev);
	case OBMF_ICP_CHANNEL_TYPE_GPIO: {
		obmf_gpio_config_data_t *gpio = (obmf_gpio_config_data_t *)data;

		if (pc->cfg.gpio.count > OBMF_GPIO_MAX_ENTRIES) {
			LOG_ERR("Channel %u: %u GPIOs, max %u", ch,
				pc->cfg.gpio.count, OBMF_GPIO_MAX_ENTRIES);
			return -EINVAL;
		}
		gpio->gpio_count = pc->cfg.gpio.count;
		return sizeof(*gpio);
	}
	case OBMF_ICP_CHANNEL_TYPE_IO: {
		obmf_io_config_data_t *io = (obmf_io_config_data_t *)data;

		*io = pc->cfg.io;
		if (io->hdr.io_range_count == 0) {
			for (int i = 0; i < OBMF_IO_MAX_RANGES; i++) {
				if (io->ranges[i].range_flags &
				    OBMF_IO_RANGE_ENABLE) {
					io->hdr.io_range_count = i + 1;
				}
			}
		}
		if (io->hdr.io_range_count > OBMF_IO_MAX_RANGES) {
			LOG_ERR("Channel %u: %u I/O ranges, max %u", ch,
				io->hdr.io_range_count, OBMF_IO_MAX_RANGES);
			return -EINVAL;
		}
		return sizeof(*io);
	}
	case OBMF_ICP_CHANNEL_TYPE_I2C_CONTROLLER:
		memcpy(data, &pc->cfg.i2c, sizeof(pc->cfg.i2c));
		obmf_i2c_set_controller_bus(ch, pc->bus);
		return sizeof(pc->cfg.i2c);
	case OBMF_ICP_CHANNEL_TYPE_I2C_TARGET: {
		obmf_i2c_target_config_data_t *tgt =
			(obmf_i2c_target_config_data_t *)data;

		/* Addresses are added later with obmf_i2c_target_set_address() */
		tgt->target_speed = pc->cfg.i2c_target.speed;
		tgt->target_buffer_size = OBMF_I2C_TARGET_BUFFER_SIZE;
		tgt->target_address_count = 0;
		return offsetof(obmf_i2c_target_config_data_t,
				target_address_list);
	}
	case OBMF_ICP_CHANNEL_TYPE_I3C_CONTROLLER: {
		obmf_i3c_config_data_t *i3c = (obmf_i3c_config_data_t *)data;

		/* IBI targets are added by obmf_i3c_ibi_target_set() */
		*i3c = pc->cfg.i3c;
		i3c->ibi_target_count = 0;
		obmf_i3c_set_controller_bus(ch, pc->bus);
		return OBMF_I3C_CONFIG_SIZE(0);
	}
	default:
		return -ENOTSUP;
	}
}

/* Add one platform channel to the Channel 0 discovery structure */
static int obmf_channel_add(const struct obmf_plat_channel *pc)
{
	uint8_t ch = pc->channel;
	obmf_channel_config_header_t *hdr = obmf_chan_cfg_hdr(ch);
	int size;

	if (!hdr) {
		LOG_ERR("Channel %u is not supported by the OBMF service", ch);
		return -EINVAL;
	}
	if (obmf_channel_is_present(ch)) {
		LOG_ERR("Channel %u is defined twice", ch);
		return -EEXIST;
	}

	size = obmf_channel_config_fill(pc, (uint8_t *)hdr +
					CHAN_CFG_CONFIG_DATA_OFFSET);
	if (size < 0) {
		return size;
	}

	hdr->channel_type = obmf_icp_get_channel_type(ch);
	hdr->channel_number = ch;
	if (pc->name) {
		if (strlen(pc->name) > OBMF_CHANNEL_NAME_MAX) {
			LOG_WRN("Channel %u: name \"%s\" truncated to %u chars",
				ch, pc->name, OBMF_CHANNEL_NAME_MAX);
		}
		strncpy(hdr->channel_name, pc->name, OBMF_CHANNEL_NAME_MAX);
	} else {
		snprintf(hdr->channel_name, sizeof(hdr->channel_name),
			 "CH%u", ch);
	}
	hdr->device_role = obmf_channel_device_role(ch);
	/* Disabled until the host enables it; the default config is valid */
	hdr->channel_status = CHAN_HEALTH_DISABLED | CHAN_STATUS_CONFIG_VALID;
	hdr->channel_control = 0;
	/*
	 * Bounded by the shared internal response buffer for every channel.
	 * A payload excludes the common header, which also lives in that
	 * buffer.
	 */
	hdr->max_request_payload_size = OBMF_RSP_BUF_SIZE;
	hdr->max_response_payload_size = OBMF_RSP_BUF_SIZE - OBMF_HEADER_SIZE;
	hdr->config_size = size;

	/* Channel offset table is indexed by (channel_number - 1) */
	discovery_struct.channel_offsets[ch - 1] =
		(uint8_t *)hdr - (uint8_t *)&discovery_struct;
	obmf_present_mask |= BIT(ch);

	return 0;
}

/* Build the Channel 0 discovery structure from the platform description */
static void obmf_discovery_init(const struct obmf_plat_config *cfg)
{
	memset(&discovery_struct, 0, sizeof(discovery_struct));
	obmf_present_mask = BIT(OBMF_ICP_CONTROL_CHANNEL);

	discovery_struct.base.version_minor_bcd = 0x00; /* v1.0 minor */
	discovery_struct.base.version_major_bcd = 0x01; /* v1.0 major */
	discovery_struct.base.vendor_id = cfg->vendor_id;
	discovery_struct.base.device_id = cfg->device_id;
	strncpy(discovery_struct.base.device_name,
		cfg->device_name ? cfg->device_name : "OpenBIC-OBMF",
		OBMF_DEVICE_NAME_SIZE - 1);
	/*
	 * Size of the channel offset table. Channels the platform did not
	 * define keep offset 0, i.e. not implemented.
	 */
	discovery_struct.base.num_of_channels = OBMF_MAX_CHANNELS;
	discovery_struct.base.config_status = 0;
	discovery_struct.base.vendor_config_offset = 0; /* Not supported */

	for (int i = 0; i < cfg->num_channels; i++) {
		const struct obmf_plat_channel *pc = &cfg->channels[i];
		int ret = obmf_channel_add(pc);

		if (ret) {
			LOG_ERR("Channel %u (%s) skipped: %d", pc->channel,
				pc->name ? pc->name : "-", ret);
		}
	}

	LOG_INF("OBMF channels: 0x%x", obmf_present_mask);
}

int obmf_service_init(const struct obmf_plat_config *cfg)
{
	if (!cfg || (cfg->num_channels && !cfg->channels)) {
		LOG_ERR("No OBMF platform configuration");
		return -EINVAL;
	}

	/* Everything the RX/TX threads use is set up before they start */
	obmf_discovery_init(cfg);

	for (int i = 0; i < OBMF_MAX_CHANNELS + 1; i++) {
		channel_tags[i].is_outstanding = false;
	}

	/* Initialize I2C Target channel message queues */
	obmf_i2c_target_channels_init();

	/* Initialize Serial channel response semaphores */
	obmf_serial_channels_init();

	/* Initialise SPI producer role (ch17: shd_dev binding) */
	obmf_spi_producer_init();

	/* Initialise Memory Device flash page cache (ch4) */
	obmf_mem_dev_init();

	k_sem_init(&obmf_rx_sem, 0, 1);

	/* Create dedicated RX thread for OBMF BULK OUT processing */
	obmf_rx_thread_id = k_thread_create(&obmf_rx_thread_data,
					obmf_rx_thread_stack,
					K_THREAD_STACK_SIZEOF(obmf_rx_thread_stack),
					obmf_rx_task, NULL, NULL, NULL,
					K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	if (!obmf_rx_thread_id) {
		LOG_ERR("Failed to create OBMF RX thread");
		obmf_stop();
		return -ENOMEM;
	}
	k_thread_name_set(obmf_rx_thread_id, "obmf_rx");

	/* Create dedicated TX thread for OBMF transport handling */
	obmf_tx_thread_id = k_thread_create(&obmf_tx_thread_data,
					obmf_tx_thread_stack,
					K_THREAD_STACK_SIZEOF(obmf_tx_thread_stack),
					obmf_tx_thread, NULL, NULL, NULL,
					K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	if (!obmf_tx_thread_id) {
		LOG_ERR("Failed to create OBMF TX thread");
		obmf_stop();
		return -ENOMEM;
	}
	k_thread_name_set(obmf_tx_thread_id, "obmf_tx");

	/* Register for USB traffic last, once there is someone to handle it */
	const struct device *dev =
		device_get_binding(CONFIG_USB_OBMF_DEVICE_NAME "_0");
	if (!dev) {
		LOG_ERR("Failed to get OBMF USB device for"
			" callback registration");
		return -ENODEV;
	}
	usb_obmf_register_device(dev, &obmf_usb_ops);

	LOG_DBG("OBMF Secondary Service Initialized");
	return 0;
}

int obmf_get_response(uint8_t *msg, uint32_t len,
		      uint8_t *rsp_buf, uint32_t *rsp_len)
{
	obmf_icp_header_t *hdr = (obmf_icp_header_t *)msg;
	uint8_t channel = OBMF_ICP_GET_CHANNEL(hdr);
	uint8_t channel_type = obmf_icp_get_channel_type(channel);
	uint8_t msg_type = OBMF_ICP_GET_RQRESP(hdr);

	/* Initialize response length (no response by default) */
	*rsp_len = 0;

	if (len < OBMF_MIN_MSG_SIZE) {
		LOG_ERR("Invalid OBMF message length: %d", len);
		/* Only send error response for requests, not responses */
		if (msg_type == OBMF_ICP_MSG_TYPE_REQUEST) {
			obmf_build_err_response(hdr, OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
						rsp_buf, rsp_len, true);
		}
		return -1;
	}

	/* Check if channel is valid and configured by the platform */
	if (!obmf_channel_is_present(channel)) {
		LOG_ERR("Invalid or unconfigured channel %d", channel);
		/* Only send error response for requests, not responses */
		if (msg_type == OBMF_ICP_MSG_TYPE_REQUEST) {
			obmf_build_err_response(hdr, OBMF_ICP_ERR_UNKNOWN_CHANNEL,
						rsp_buf, rsp_len, true);
		}
		return -1;
	}

	/*
	 * Parse MMIO sub-header and payload.
	 * CONFIG (0x00) and MMIO (0x01) channel types use the
	 * 2-byte MMIO header (Transaction + Reserved, Table 5).
	 * Interface-optimized channels (Serial, IPMI, SPI) do NOT have
	 * MMIO sub-header - payload is directly after common header.
	 */
	obmf_icp_mmio_sub_header_t *sub_hdr = NULL;
	uint8_t *payload;
	uint32_t payload_len;

	/* Parse sub-header based on channel */
	if (channel <= OBMF_ICP_MMIO_CHANNEL) {
		/* MMIO-style channels have sub-header */
		sub_hdr = (obmf_icp_mmio_sub_header_t *)(msg + OBMF_HEADER_SIZE);
		payload = msg + OBMF_FULL_MMIO_HDR_SIZE;
		payload_len = (len > OBMF_FULL_MMIO_HDR_SIZE) ?
			      len - OBMF_FULL_MMIO_HDR_SIZE : 0;
	} else if (channel == OBMF_ICP_IO_CHANNEL) {
		/* I/O channel has its own 1-byte header (Table 52) */
		payload = msg + OBMF_FULL_IO_HDR_SIZE;
		payload_len = (len > OBMF_FULL_IO_HDR_SIZE) ?
			      len - OBMF_FULL_IO_HDR_SIZE : 0;
	} else {
		/* Interface-optimized channels: payload after common header */
		payload = msg + OBMF_HEADER_SIZE;
		payload_len = (len > OBMF_HEADER_SIZE) ?
			      len - OBMF_HEADER_SIZE : 0;
	}

	LOG_HEXDUMP_DBG(msg, len, "BULK_OUT Data:");

	/*
	 * Reject requests larger than the channel advertises in
	 * MAX_REQUEST_PAYLOAD_SIZE before any handler parses them.
	 */
	if (msg_type == OBMF_ICP_MSG_TYPE_REQUEST) {
		const obmf_channel_config_header_t *cfg_hdr =
			obmf_chan_cfg_hdr(channel);

		if (cfg_hdr && len - OBMF_HEADER_SIZE >
			       cfg_hdr->max_request_payload_size) {
			LOG_ERR("CH%d request payload %u exceeds max %u",
				channel, len - OBMF_HEADER_SIZE,
				cfg_hdr->max_request_payload_size);
			obmf_build_err_response(hdr,
						OBMF_ICP_STATUS_SIZE_NOT_SUPPORTED,
						rsp_buf, rsp_len, false);
			return -1;
		}
	}

	/*
	 * A channel serves requests only while the host has it enabled
	 * (CHANNEL_CONTROL.CHANNEL_ENABLE); Channel 0 is always enabled.
	 */
	if (msg_type == OBMF_ICP_MSG_TYPE_REQUEST &&
	    channel != OBMF_ICP_CONTROL_CHANNEL &&
	    !obmf_channel_is_enabled(channel)) {
		LOG_WRN("CH%d request while the channel is disabled", channel);
		obmf_build_err_response(hdr, OBMF_ICP_STATUS_NOT_READY,
					rsp_buf, rsp_len, false);
		return -1;
	}

	if (msg_type == OBMF_ICP_MSG_TYPE_REQUEST) {
		// --- Handle incoming REQUESTS (we are the Producer) ---
		LOG_DBG("Received OBMF request: Channel=%d, Type=0x%02x",
			channel, channel_type);

		switch (channel) {
		case OBMF_ICP_CONTROL_CHANNEL: {
			uint8_t trans = OBMF_ICP_MMIO_GET_TRANS(sub_hdr);
			switch (trans) {
			case OBMF_ICP_TRANS_SHORT_READ:
			case OBMF_ICP_TRANS_LONG_READ:
				handle_ch0_read_req(hdr, payload, payload_len,
						    rsp_buf, rsp_len);
				break;
			case OBMF_ICP_TRANS_SHORT_WRITE:
			case OBMF_ICP_TRANS_LONG_WRITE:
				handle_ch0_write_req(hdr, payload, payload_len,
						     rsp_buf, rsp_len);
				break;
			default:
				LOG_ERR("Unsupported transaction type %d for"
					" Channel 0", trans);
				obmf_build_err_response(hdr,
							OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
							rsp_buf, rsp_len, true);
				break;
			}
			break;
		}
		case OBMF_ICP_MMIO_CHANNEL: {
			uint8_t trans = OBMF_ICP_MMIO_GET_TRANS(sub_hdr);
			switch (trans) {
			case OBMF_ICP_TRANS_SHORT_READ:
			case OBMF_ICP_TRANS_LONG_READ:
				handle_mmio_read_req(hdr, payload, payload_len,
						    rsp_buf, rsp_len);
				break;
			case OBMF_ICP_TRANS_SHORT_WRITE:
			case OBMF_ICP_TRANS_LONG_WRITE:
				handle_mmio_write_req(hdr, payload, payload_len,
						     rsp_buf, rsp_len);
				break;
			default:
				LOG_ERR("Unsupported transaction type %d for"
					" MMIO Channel", trans);
				obmf_build_err_response(hdr,
							OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
							rsp_buf, rsp_len, true);
				break;
			}
			break;
		}
		case OBMF_ICP_SERIAL_CHANNEL_0:
		case OBMF_ICP_SERIAL_CHANNEL_1: {
			/* Host sends serial data to us (Producer side) */
			handle_serial_request(hdr, payload, payload_len,
					     rsp_buf, rsp_len);
			break;
		}
		case OBMF_ICP_IPMI_CHANNEL_NUM:
		case OBMF_ICP_MEM_DEV_CHANNEL:
		case OBMF_ICP_I2C_TARGET_CHANNEL_0:
		case OBMF_ICP_I2C_TARGET_CHANNEL_1:
			/*
			 * We are the Consumer on these channels and the host,
			 * as Producer, is not a Requester (Tables 29, 44, 59).
			 * Still answer, so the host is not left waiting.
			 */
			LOG_WRN("Unexpected request on consumer channel %d",
				channel);
			obmf_build_err_response(hdr,
						OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
						rsp_buf, rsp_len, false);
			break;
		case OBMF_ICP_SPI_CHANNEL: {
			/* Host sends SPI flash requests to us (Producer side) */
			handle_spi_request(hdr, payload, payload_len,
					   rsp_buf, rsp_len);
			break;
		}
		case OBMF_ICP_VW_GPIO_CHANNEL: {
			/* Host sends GPIO requests to us (Producer side) */
			handle_gpio_request(hdr, payload, payload_len,
					    rsp_buf, rsp_len);
			break;
		}
		case OBMF_ICP_SGPIO1_CHANNEL: {
			/* Host sends SGPIO requests to us (same GPIO protocol) */
			handle_gpio_request(hdr, payload, payload_len,
					    rsp_buf, rsp_len);
			break;
		}
		case OBMF_ICP_SMC_GPIO_CHANNEL: {
			/* Host sends platform GPIO requests to us (same GPIO protocol) */
			handle_gpio_request(hdr, payload, payload_len,
					    rsp_buf, rsp_len);
			break;
		}
		case OBMF_ICP_SGPIO0_CHANNEL: {
			/* Host sends SGPIO0 requests to us (same GPIO protocol) */
			handle_gpio_request(hdr, payload, payload_len,
					    rsp_buf, rsp_len);
			break;
		}
		case OBMF_ICP_IO_CHANNEL: {
			/* Host sends port I/O requests to us (Producer side) */
			handle_io_request(hdr, payload, payload_len,
					  rsp_buf, rsp_len);
			break;
		}
		case OBMF_ICP_I2C_CONTROLLER_CHANNEL_0:
		case OBMF_ICP_I2C_CONTROLLER_CHANNEL_1:
		case OBMF_ICP_I2C_CONTROLLER_CHANNEL_2:
		case OBMF_ICP_I2C_CONTROLLER_CHANNEL_3: {
			/* Host (BMC) drives an I2C transaction through us
			 * (Producer side) */
			handle_i2c_request(hdr, payload, payload_len,
					   rsp_buf, rsp_len);
			break;
		}
		case OBMF_ICP_I3C_CONTROLLER_CHANNEL:
		case OBMF_ICP_I3C_CONTROLLER_CHANNEL_1: {
			/* Host (BMC) drives an I3C transaction through us
			 * (Producer side) */
			handle_i3c_request(hdr, payload, payload_len,
					   rsp_buf, rsp_len);
			break;
		}
		default:
			LOG_ERR("Request received for unsupported/consumer"
				" channel %d", channel);
			obmf_build_err_response(hdr,
						OBMF_ICP_ERR_UNKNOWN_CHANNEL,
						rsp_buf, rsp_len, true);
			break;
		}

		LOG_HEXDUMP_DBG(rsp_buf, *rsp_len, "Response Data:");
	} else {
		// --- Handle incoming RESPONSES (we are the Consumer) ---
		LOG_DBG("Received OBMF response: Channel=%d, Type=0x%02x",
			channel, channel_type);

		/* Check if there is an outstanding request */
		if (!channel_tags[channel].is_outstanding) {
			LOG_WRN("Response received without outstanding request "
				"on channel %d, dropping", channel);
			return 0;
		}

		/* Dispatch to channel-specific response handler */
		switch (channel) {
		case OBMF_ICP_MMIO_CHANNEL:
			handle_mmio_response(hdr, payload, payload_len);
			break;
		case OBMF_ICP_SERIAL_CHANNEL_0:
		case OBMF_ICP_SERIAL_CHANNEL_1:
			handle_serial_response(hdr, payload, payload_len);
			break;
		case OBMF_ICP_IPMI_CHANNEL_NUM:
			handle_ipmi_response(hdr, payload, payload_len);
			break;
		case OBMF_ICP_MEM_DEV_CHANNEL:
			handle_mem_dev_response(hdr, payload, payload_len);
			break;
		case OBMF_ICP_VW_GPIO_CHANNEL:
			handle_gpio_response(hdr, payload, payload_len);
			break;
		case OBMF_ICP_SGPIO1_CHANNEL:
			handle_gpio_response(hdr, payload, payload_len);
			break;
		case OBMF_ICP_SMC_GPIO_CHANNEL:
			handle_gpio_response(hdr, payload, payload_len);
			break;
		case OBMF_ICP_SGPIO0_CHANNEL:
			handle_gpio_response(hdr, payload, payload_len);
			break;
		case OBMF_ICP_IO_CHANNEL:
			handle_io_response(hdr, payload, payload_len);
			break;
		case OBMF_ICP_I2C_TARGET_CHANNEL_0:
		case OBMF_ICP_I2C_TARGET_CHANNEL_1:
			handle_i2c_target_response(hdr, payload, payload_len);
			break;
		case OBMF_ICP_I3C_CONTROLLER_CHANNEL:
		case OBMF_ICP_I3C_CONTROLLER_CHANNEL_1:
			handle_i3c_response(hdr, payload, payload_len);
			break;
		default:
			LOG_WRN("Response received for unhandled channel %d, "
				"dropping", channel);
			channel_tags[channel].is_outstanding = false;
			break;
		}
	}

	return 0;
}

int obmf_send_request(uint8_t channel, uint8_t *req_buf, uint32_t req_len)
{
	OBMF_RETURN_ERR_IF(channel > OBMF_MAX_CHANNELS,
			   "Invalid channel %d", channel);

	/* v0.9: Check channel enable via CHANNEL_CONTROL bit per channel */
	obmf_channel_config_header_t *cfg_hdr = obmf_chan_cfg_hdr(channel);
	uint8_t chan_ctrl = cfg_hdr ? cfg_hdr->channel_control : 0;

	/* Quiet path: a disabled channel is polled repeatedly by its sender */
	if (!(chan_ctrl & CHAN_CONTROL_ENABLE)) {
		LOG_DBG("channel %d is not enabled", channel);
		return -ENETDOWN;
	}

	/* One outstanding request per channel, until it is answered or abandoned */
	if (channel_tags[channel].is_outstanding) {
		if (k_uptime_get() - channel_tags[channel].sent_at <
		    OBMF_REQUEST_TIMEOUT_MS) {
			LOG_DBG("OBMF channel %d request is outstanding",
				channel);
			return -EBUSY;
		}
		LOG_WRN("OBMF channel %d: request timed out, abandoned",
			channel);
	}

	obmf_icp_header_t *hdr = (obmf_icp_header_t *)req_buf;

	/* Set common header fields */
	OBMF_ICP_SET_CHANNEL(hdr, channel);
	OBMF_ICP_SET_RQRESP(hdr, OBMF_ICP_MSG_TYPE_REQUEST);
	/* Size = total payload size (everything after common header) */
	OBMF_ICP_SET_SIZE(hdr, req_len - OBMF_HEADER_SIZE);

	/* Send request via TX thread message queue */
	struct obmf_transport_msg msg;
	msg.buf = (uint8_t *)malloc(req_len);
	if (!msg.buf) {
		LOG_ERR("Failed to allocate buffer for OBMF request (%d bytes)",
			req_len);
		channel_tags[channel].is_outstanding = false;
		return -ENOMEM;
	}
	memcpy(msg.buf, req_buf, req_len);
	msg.len = req_len;

	channel_tags[channel].is_outstanding = true;
	channel_tags[channel].sent_at = k_uptime_get();

	if (k_msgq_put(&obmf_tx_msgq, &msg, K_NO_WAIT) != 0) {
		LOG_WRN("Failed to queue OBMF request, queue full");
		channel_tags[channel].is_outstanding = false;
		free(msg.buf);
		return -ENOBUFS;
	}
	return 0;
}

static void handle_ch0_read_req(obmf_icp_header_t *req_hdr,
				uint8_t *req_payload,
				uint32_t req_len,
				uint8_t *rsp_buf,
				uint32_t *rsp_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(req_hdr);
	obmf_icp_mmio_sub_header_t *req_sub =
		(obmf_icp_mmio_sub_header_t *)((uint8_t *)req_hdr + OBMF_HEADER_SIZE);

	uint8_t trans_type = OBMF_ICP_MMIO_GET_TRANS(req_sub);
	bool is_long = (trans_type == OBMF_ICP_TRANS_LONG_READ);
	uint32_t min_req_size = is_long
		? OBMF_LONG_READ_REQ_SIZE : OBMF_SHORT_READ_REQ_SIZE;

	if (req_len < min_req_size) {
		LOG_ERR("Invalid CH0 read request payload length: %d"
			" (need %d for %s read)",
			req_len, min_req_size, is_long ? "long" : "short");
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

	uint32_t addr_size = is_long ? OBMF_LONG_ADDR_SIZE : OBMF_SHORT_ADDR_SIZE;
	uint16_t read_size;

	memcpy(&read_size, &req_payload[addr_size], sizeof(uint16_t));

	LOG_DBG("CH0 %s Read Request: addr=0x%llx, size=%d",
		is_long ? "Long" : "Short", addr, read_size);

	/* Validate read_size > 0 */
	if (read_size == 0) {
		LOG_ERR("CH0 read size is 0");
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
					  rsp_buf, rsp_len, true);
	}

	uint32_t max_read = OBMF_CH0_MAX_TRANS_SIZE;
	if (read_size > max_read) {
		LOG_ERR("CH0 read size %d exceeds max %d",
			read_size, max_read);
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_STATUS_SIZE_NOT_SUPPORTED,
					  rsp_buf, rsp_len, true);
	}

	/* A 64-bit Long Read address must not wrap the sum below the end */
	if (addr >= sizeof(discovery_struct) ||
	    read_size > sizeof(discovery_struct) - addr) {
		LOG_ERR("CH0 read out of bounds: addr=0x%llx, size=%d",
			addr, read_size);
		obmf_build_err_response(req_hdr, OBMF_ICP_ERR_ADDR_OUT_OF_RANGE,
					rsp_buf, rsp_len, true);
		return;
	}

	obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;
	obmf_icp_mmio_sub_header_t *rsp_sub =
		(obmf_icp_mmio_sub_header_t *)(rsp_buf + OBMF_HEADER_SIZE);
	uint8_t *rsp_payload = rsp_buf + OBMF_FULL_MMIO_HDR_SIZE;

	/* Build response common header */
	OBMF_ICP_SET_CHANNEL(rsp_hdr, channel);
	/* v1.0.0 RC1: Status in common header */
	OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr, OBMF_ICP_MSG_TYPE_RESPONSE,
				   OBMF_ICP_SUCCESS);

	/* Echo the MMIO header transaction */
	obmf_mmio_rsp_sub_header(rsp_sub, req_sub);

	/* v1.0.0 RC1 Read Response: data in payload */
	memcpy(rsp_payload, (uint8_t *)&discovery_struct + addr, read_size);

	/* Size = MMIO header + read_size */
	OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_MMIO_SUB_HDR_SIZE + read_size);

	*rsp_len = OBMF_FULL_MMIO_HDR_SIZE + read_size;
}

/*
 * CHANNEL_CONTROL write (Table 65). Only CHANNEL_ENABLE is stored; RESET,
 * APPLY_CONFIGURATION, MANUF_CONFIG_LOCK_REQUEST and CLEAR_CHANNEL_ERROR
 * are write-1 self-clearing requests and the reserved bits read as 0.
 */
static void obmf_channel_control_write(obmf_channel_config_header_t *ch_hdr,
				       uint8_t new_ctrl)
{
	uint8_t ch_no = ch_hdr->channel_number;
	bool was_enabled = (ch_hdr->channel_control & CHAN_CONTROL_ENABLE) != 0;
	bool now_enabled = (new_ctrl & CHAN_CONTROL_ENABLE) != 0;
	uint8_t status = ch_hdr->channel_status;

	ch_hdr->channel_control = new_ctrl & CHAN_CONTROL_ENABLE;

	/* Explicit host intent overrides a pending resume restore */
	obmf_suspended_mask &= ~BIT(ch_no);

	if (new_ctrl & CHAN_CONTROL_RESET) {
		/* Drop the outstanding request and any partial message */
		LOG_INF("Channel %d reset", ch_no);
		channel_tags[ch_no].is_outstanding = false;
		if (obmf_frag_bufs[ch_no].buf) {
			free(obmf_frag_bufs[ch_no].buf);
			obmf_frag_bufs[ch_no].buf = NULL;
		}
		obmf_frag_bufs[ch_no].offset = 0;
		obmf_frag_bufs[ch_no].discarding = false;
		status |= CHAN_STATUS_RESET_DONE;
	}

	/* Configuration Data writes take effect at once, so it is applied */
	if (new_ctrl & CHAN_CONTROL_APPLY_CONFIG) {
		status |= CHAN_STATUS_CONFIG_VALID;
		discovery_struct.base.config_status |=
			CH0_CONFIG_STATUS_RUNTIME_APPLIED;
	}

	/* Manufacturing Mode is not supported: a lock request has no effect */

	/* No error condition is tracked, so it is never still present */
	if (new_ctrl & CHAN_CONTROL_CLEAR_ERROR) {
		status &= ~CHAN_STATUS_ERROR_INDICATOR;
	}

	if (was_enabled != now_enabled) {
		LOG_INF("Channel %d %s", ch_no, now_enabled ? "enabled" : "disabled");
		channel_tags[ch_no].is_outstanding = false;
	}

	/* Health follows the enable state unless an error is still flagged */
	if (!(status & CHAN_STATUS_ERROR_INDICATOR)) {
		status = (status & ~CHAN_STATUS_HEALTH_MASK) |
			 (now_enabled ? CHAN_HEALTH_READY : CHAN_HEALTH_DISABLED);
	}
	ch_hdr->channel_status = status;
}

/*
 * Whether byte @off of a channel configuration space may be written by the
 * host: CHANNEL_NAME in the common header (Table 65), and the "rw" fields of
 * the channel type's CONFIGURATION_DATA (Tables 75-87).  CHANNEL_CONTROL is
 * handled separately; CHANNEL_SPECIFIC_CONTROL controls are not implemented.
 */
static bool ch0_cfg_byte_writable(uint8_t type, uint32_t off)
{
	uint32_t o;

	if (off >= CHAN_CFG_NAME_OFFSET && off < CHAN_CFG_DEVICE_ROLE_OFFSET) {
		return true;
	}
	if (off < CHAN_CFG_CONFIG_DATA_OFFSET) {
		return false;
	}
	o = off - CHAN_CFG_CONFIG_DATA_OFFSET;

	switch (type) {
	case OBMF_ICP_CHANNEL_TYPE_MMIO:
		/* MAX_MMIO_READ/WRITE_SIZE; the window is read-only */
		return o < offsetof(obmf_mmio_config_data_t,
				    mmio_window_base_address);
	case OBMF_ICP_CHANNEL_TYPE_GPIO:
		/* GPIO_COUNT and each entry's trailing RESERVED are read-only */
		if (o < offsetof(obmf_gpio_config_data_t, entries)) {
			return false;
		}
		o = (o - offsetof(obmf_gpio_config_data_t, entries)) %
		    sizeof(obmf_gpio_config_entry_t);
		return o < offsetof(obmf_gpio_config_entry_t, reserved);
	case OBMF_ICP_CHANNEL_TYPE_SERIAL:
		return true;
	case OBMF_ICP_CHANNEL_TYPE_I2C_CONTROLLER:
		/* I2C_CONTROLLER_OPTIONS is read-only */
		return o == offsetof(obmf_i2c_config_data_t, controller_speed);
	case OBMF_ICP_CHANNEL_TYPE_I2C_TARGET:
		/* I2C_TARGET_ADDRESS_COUNT is read-only */
		return o != offsetof(obmf_i2c_target_config_data_t,
				     target_address_count);
	case OBMF_ICP_CHANNEL_TYPE_I3C_CONTROLLER:
		/* DEV_NACK_RETRY_COUNT and the IBI target array */
		return o < offsetof(obmf_i3c_config_data_t, i3c_feature_flags) ||
		       o >= offsetof(obmf_i3c_config_data_t, ibi_targets);
	case OBMF_ICP_CHANNEL_TYPE_SPI_CONTROLLER:
		/* SPI_SUPPORTED_CONFIG_FLAGS and RESERVED are read-only */
		return (o >= offsetof(obmf_spi_config_data_t, config_flags) &&
			o < offsetof(obmf_spi_config_data_t, reserved)) ||
		       o >= offsetof(obmf_spi_config_data_t, rd_max_speed_hz);
	case OBMF_ICP_CHANNEL_TYPE_IO:
		/* The header and each range's reserved flags byte are read-only */
		if (o < offsetof(obmf_io_config_data_t, ranges)) {
			return false;
		}
		o = (o - offsetof(obmf_io_config_data_t, ranges)) %
		    sizeof(obmf_io_range_cfg_t);
		return o != offsetof(obmf_io_range_cfg_t, reserved);
	default:
		/* IPMI has no Configuration Data; MEM_DEV_CFG is read-only */
		return false;
	}
}

/* Force the read-only bits inside writable bytes back to their values */
static void ch0_cfg_sanitize(obmf_channel_config_header_t *hdr)
{
	uint8_t *data = (uint8_t *)hdr + CHAN_CFG_CONFIG_DATA_OFFSET;

	switch (hdr->channel_type) {
	case OBMF_ICP_CHANNEL_TYPE_GPIO: {
		obmf_gpio_config_data_t *gpio = (obmf_gpio_config_data_t *)data;

		/* GPIO_INDEX[15:12] read as 0 */
		for (int i = 0; i < OBMF_GPIO_MAX_ENTRIES; i++) {
			gpio->entries[i].gpio_index &= 0x0FFF;
		}
		break;
	}
	case OBMF_ICP_CHANNEL_TYPE_IO: {
		obmf_io_config_data_t *io = (obmf_io_config_data_t *)data;

		/* IO_RANGE_FLAGS[15:1] are reserved */
		for (int i = 0; i < OBMF_IO_MAX_RANGES; i++) {
			io->ranges[i].range_flags &= OBMF_IO_RANGE_ENABLE;
		}
		break;
	}
	case OBMF_ICP_CHANNEL_TYPE_I3C_CONTROLLER:
		obmf_i3c_config_written((obmf_i3c_config_data_t *)data);
		break;
	default:
		break;
	}
}

static void handle_ch0_write_req(obmf_icp_header_t *req_hdr,
				 uint8_t *req_payload,
				 uint32_t req_len,
				 uint8_t *rsp_buf,
				 uint32_t *rsp_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(req_hdr);
	obmf_icp_mmio_sub_header_t *req_sub_w =
		(obmf_icp_mmio_sub_header_t *)((uint8_t *)req_hdr + OBMF_HEADER_SIZE);

	uint8_t trans_type = OBMF_ICP_MMIO_GET_TRANS(req_sub_w);
	bool is_long = (trans_type == OBMF_ICP_TRANS_LONG_WRITE);
	uint32_t addr_size = is_long ? OBMF_LONG_ADDR_SIZE : OBMF_SHORT_ADDR_SIZE;
	uint32_t min_write_size = addr_size + 1;

	if (req_len < min_write_size) {
		LOG_ERR("Invalid CH0 write request: payload_len=%d"
			" (need >= %d for %s write)",
			req_len, min_write_size, is_long ? "long" : "short");
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

	/* The data runs from after the address to the end of the message */
	uint16_t write_size = req_len - addr_size;
	uint8_t *write_data = req_payload + addr_size;

	LOG_DBG("CH0 %s Write Request: addr=0x%llx, size=%d",
		is_long ? "Long" : "Short", addr, write_size);

	uint32_t max_write = OBMF_CH0_MAX_TRANS_SIZE;
	if (write_size > max_write) {
		LOG_ERR("CH0 write size %d exceeds max %d",
			write_size, max_write);
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_STATUS_SIZE_NOT_SUPPORTED,
					  rsp_buf, rsp_len, true);
	}

	/* A 64-bit Long Write address must not wrap the sum below the end */
	if (addr >= sizeof(discovery_struct) ||
	    write_size > sizeof(discovery_struct) - addr) {
		LOG_ERR("CH0 write out of bounds: addr=0x%llx, size=%d",
			addr, write_size);
		obmf_build_err_response(req_hdr, OBMF_ICP_ERR_ADDR_OUT_OF_RANGE,
				rsp_buf, rsp_len, true);
		return;
	}

	obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;
	obmf_icp_mmio_sub_header_t *rsp_sub =
		(obmf_icp_mmio_sub_header_t *)(rsp_buf + OBMF_HEADER_SIZE);

	/* Build response common header */
	OBMF_ICP_SET_CHANNEL(rsp_hdr, channel);
	OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr, OBMF_ICP_MSG_TYPE_RESPONSE,
				   OBMF_ICP_SUCCESS);

	/* Echo the MMIO header transaction */
	obmf_mmio_rsp_sub_header(rsp_sub, req_sub_w);

	/*
	 * Writable fields (Table 63 "rw"): CONFIG_STATUS, and in each
	 * channel configuration space CHANNEL_NAME, CHANNEL_CONTROL and the
	 * rw fields of its CONFIGURATION_DATA, up to CONFIG_SIZE.  Anything
	 * else inside the discovery structure is read-only.
	 */
	for (uint8_t ch = 1; ch <= OBMF_MAX_CHANNELS; ch++) {
		obmf_channel_config_header_t *ch_hdr = obmf_chan_cfg_hdr(ch);
		uint32_t base, end, rel;

		if (!ch_hdr || !obmf_channel_is_present(ch)) {
			continue;
		}
		base = (uint8_t *)ch_hdr - (uint8_t *)&discovery_struct;
		end = base + CHAN_CFG_CONFIG_DATA_OFFSET + ch_hdr->config_size;
		if (addr < base || addr >= end) {
			continue;
		}
		rel = addr - base;

		if (rel == CHAN_CFG_CONTROL_OFFSET && write_size == 1) {
			obmf_channel_control_write(ch_hdr, write_data[0]);
			goto success;
		}
		if (write_size > end - addr) {
			goto denied;
		}
		for (uint16_t i = 0; i < write_size; i++) {
			if (!ch0_cfg_byte_writable(ch_hdr->channel_type,
						   rel + i)) {
				goto denied;
			}
		}

		memcpy((uint8_t *)&discovery_struct + addr, write_data,
		       write_size);
		ch0_cfg_sanitize(ch_hdr);
		goto success;
	}

	/*
	 * CONFIG_STATUS: only MANUF_CONFIG_LOCK_REQUEST is writable, and this
	 * device does not support Manufacturing Mode, so the request is
	 * accepted and ignored; the bit reads back as 0.
	 */
	if (addr == offsetof(obmf_ch0_base_t, config_status) && write_size == 1) {
		goto success;
	}

denied:
	/* Inside the discovery structure, but not a writable field */
	LOG_ERR("CH0 Write rejected: addr=0x%llx size=%u is read-only",
		addr, write_size);
	OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr, OBMF_ICP_MSG_TYPE_RESPONSE,
				   OBMF_ICP_ERR_INSUFFICIENT_PRIVILEGE);
	OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_MMIO_SUB_HDR_SIZE);
	*rsp_len = OBMF_FULL_MMIO_HDR_SIZE;
	return;

success:
	/* Write response has no payload, status in header only */
	OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_MMIO_SUB_HDR_SIZE);
	*rsp_len = OBMF_FULL_MMIO_HDR_SIZE;
}
