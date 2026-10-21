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

#define OBMF_DEFAULT_MAX_TRANS_SIZE 64
#define OBMF_PRIMARY_ENDPOINT 0
#define OBMF_MIN_MSG_SIZE OBMF_HEADER_SIZE
#define OBMF_RSP_BUF_SIZE 256
/* Short transactions use a 32-bit address, Long a 64-bit one; the
 * Read/Write Length field is 2 bytes in both cases.
 */
#define OBMF_SHORT_ADDR_SIZE 4
#define OBMF_LONG_ADDR_SIZE 8
#define OBMF_SIZE_FIELD_LEN 2
#define OBMF_SHORT_READ_REQ_SIZE (OBMF_SHORT_ADDR_SIZE + OBMF_SIZE_FIELD_LEN)
#define OBMF_LONG_READ_REQ_SIZE (OBMF_LONG_ADDR_SIZE + OBMF_SIZE_FIELD_LEN)
#define OBMF_SHORT_WRITE_MIN_SIZE (OBMF_SHORT_ADDR_SIZE + OBMF_SIZE_FIELD_LEN + 1)
#define OBMF_LONG_WRITE_MIN_SIZE (OBMF_LONG_ADDR_SIZE + OBMF_SIZE_FIELD_LEN + 1)

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
			 * Note: Last fragment with len < MPS will be treated
			 * as EOM by receiver. If last fragment happens to be
			 * exactly MPS, the receiver's short packet detection
			 * will not trigger. In that case, we may need to
			 * send a ZLP or rely on host-side framing
			 * (each fragment has header).
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
} obmf_frag_msg_buf;

static obmf_frag_msg_buf obmf_frag_bufs[OBMF_MAX_CHANNELS + 1];

static void obmf_out_cb(const struct device *dev, uint32_t len, uint8_t *data);

/*
 * USB bus-reset / disconnect notification.
 *
 * Runs in USB interrupt context, so it must stay ISR-safe: just flag the
 * event and let the RX thread perform the real cleanup (freeing fragment
 * buffers and clearing channel tags) at a safe point before processing the
 * next request. This resynchronizes the tag sequence after a host reboot.
 */
static void obmf_bus_reset_cb(const struct device *dev)
{
	ARG_UNUSED(dev);
	atomic_set(&obmf_reset_pending, 1);
}

static struct obmf_ops obmf_usb_ops = {
	.read = obmf_out_cb,
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
		 * buffers and resets the tag sequence so the new fragment is
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
			k_yield();
			continue;
		}

		obmf_frag_msg_buf *frag_buf = &obmf_frag_bufs[channel];

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
			LOG_DBG("OBMF SOM: allocated fragment buffer for"
				" channel %u", channel);
		}

		/* Check buffer overflow */
		if (frag_buf->offset + fragment_len > OBMF_FRAG_BUF_SIZE) {
			LOG_ERR("OBMF fragment buffer overflow on channel"
				" %u: current %u + new %u > %u",
				channel, frag_buf->offset,
				fragment_len, OBMF_FRAG_BUF_SIZE);
			free(frag_buf->buf);
			frag_buf->buf = NULL;
			frag_buf->offset = 0;
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
		 * Detect end of message (EOM):
		 * If received length < MPS, this is the last fragment
		 * (short packet). USB host uses short packets to
		 * indicate end of transfer.
		 */
		if (fragment_len < CONFIG_OBMF_BULK_EP_MPS) {
			is_complete = true;
			LOG_DBG("OBMF EOM detected on channel %u:"
				" fragment len %u (< MPS %u), total %u bytes",
				channel, fragment_len,
				CONFIG_OBMF_BULK_EP_MPS, frag_buf->offset);
		} else {
			LOG_DBG("OBMF fragment on channel %u: len %u,"
				" accumulated %u bytes (waiting for more)",
				channel, fragment_len, frag_buf->offset);
		}

		/* Process complete message */
		if (is_complete) {
			uint8_t rsp_buf[OBMF_RSP_BUF_SIZE];
			uint32_t rsp_len = 0;
			obmf_get_response(frag_buf->buf, frag_buf->offset,
					  rsp_buf, &rsp_len);

			if (rsp_len > 0) {
				/* Send response to TX thread for handling */
				struct obmf_transport_msg msg;
				msg.buf = (uint8_t *)malloc(rsp_len);
				if (!msg.buf) {
					LOG_ERR("Failed to allocate buffer for"
						" OBMF response (%d bytes)",
						rsp_len);
				} else {
					memcpy(msg.buf, rsp_buf, rsp_len);
					msg.len = rsp_len;

					if (k_msgq_put(&obmf_tx_msgq, &msg,
						       K_NO_WAIT) != 0) {
						LOG_WRN("Failed to queue OBMF "
							"response, queue full");
						free(msg.buf);
					}
				}
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
 * reboot). Frees any in-progress fragment reassembly buffers and clears the
 * channel tags so the tag sequence restarts at 0, matching the host which
 * also restarts its tag sequence after reconnecting.
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

		channel_tags[i].is_outstanding = false;
		channel_tags[i].producer_tag = 0;
		channel_tags[i].consumer_tag = 0;
	}

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
	default:
		return NULL;
	}
}

bool obmf_channel_is_enabled(uint8_t channel)
{
	obmf_channel_config_header_t *hdr = obmf_chan_cfg_hdr(channel);

	return hdr && (hdr->channel_control & CHAN_CONTROL_ENABLE);
}

/*
 * Tags are deliberately left untouched: a plain USB suspend may be followed
 * by a resume without a bus reset, and the host's tag sequence continues.
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

		*rsp_sub = *req_sub;
		OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_MMIO_SUB_HDR_SIZE);
		*rsp_len = OBMF_FULL_MMIO_HDR_SIZE;
	} else if (channel == OBMF_ICP_IO_CHANNEL) {
		/* I/O channel still carries a Tag; echo its sub-header */
		obmf_icp_io_sub_header_t *req_sub =
			(obmf_icp_io_sub_header_t *)((uint8_t *)req_hdr + OBMF_HEADER_SIZE);
		obmf_icp_io_sub_header_t *rsp_sub =
			(obmf_icp_io_sub_header_t *)(rsp_buf + OBMF_HEADER_SIZE);

		*rsp_sub = *req_sub;
		OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_ICP_IO_SUB_HDR_SIZE);
		*rsp_len = OBMF_FULL_IO_HDR_SIZE;

		if (alt) {
			channel_tags[channel].producer_tag ^= 1;
		}
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

void obmf_service_init(void)
{
	const struct device *dev =
		device_get_binding(CONFIG_USB_OBMF_DEVICE_NAME "_0");
	if (dev) {
		usb_obmf_register_device(dev, &obmf_usb_ops);
	} else {
		LOG_ERR("Failed to get OBMF USB device for"
			" callback registration");
	}

	/* Initialize ring buffer and semaphore */
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
		return;
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
		return;
	}
	k_thread_name_set(obmf_tx_thread_id, "obmf_tx");

	/* Initialize I2C Target channel message queues */
	obmf_i2c_target_channels_init();

	/* Initialize Serial channel response semaphores */
	obmf_serial_channels_init();

	LOG_DBG("OBMF Secondary Service Initialized");

	// --- Populate Channel 0 Discovery Structure (v0.9 format) ---
	memset(&discovery_struct, 0, sizeof(discovery_struct));

	/* Base registers */
	discovery_struct.base.version_minor_bcd = 0x00; /* v1.0 minor */
	discovery_struct.base.version_major_bcd = 0x01; /* v1.0 major */
	discovery_struct.base.vendor_id = 0xFFFF; /* No PCI-SIG ID */
	discovery_struct.base.device_id = 0x0001;
	strncpy(discovery_struct.base.device_name, "OpenBIC-OBMF",
		OBMF_DEVICE_NAME_SIZE - 1);
	discovery_struct.base.num_of_channels = OBMF_MAX_CHANNELS; /* MMIO, Serial×2, IPMI, SPI, GPIO×4, I/O, I2C×4+Target×2, I3C */
	discovery_struct.base.config_status = 0;
	discovery_struct.base.vendor_config_offset = 0; /* Not supported */

	/*
	 * Channel offset table is indexed by (channel_number - 1).
	 * Keep this mapping aligned with OBMF_ICP_*_CHANNEL constants.
	 */
	discovery_struct.channel_offsets[OBMF_ICP_MMIO_CHANNEL - 1] =
		CH0_MMIO_CHAN_CONFIG_OFFSET;
	discovery_struct.channel_offsets[OBMF_ICP_SERIAL_CHANNEL_0 - 1] =
		CH0_SERIAL_CHAN_CONFIG_OFFSET;
	discovery_struct.channel_offsets[OBMF_ICP_IPMI_CHANNEL_NUM - 1] =
		CH0_IPMI_CHAN_CONFIG_OFFSET;
	discovery_struct.channel_offsets[OBMF_ICP_SPI_CHANNEL - 1] =
		CH0_SPI_CHAN_CONFIG_OFFSET;
	discovery_struct.channel_offsets[OBMF_ICP_MEM_DEV_CHANNEL - 1] =
		CH0_MEM_DEV_CHAN_CONFIG_OFFSET;
	discovery_struct.channel_offsets[OBMF_ICP_VW_GPIO_CHANNEL - 1] =
		CH0_GPIO_CHAN_CONFIG_OFFSET;
	discovery_struct.channel_offsets[OBMF_ICP_SGPIO1_CHANNEL - 1] =
		CH0_SGPIO1_CHAN_CONFIG_OFFSET;
	discovery_struct.channel_offsets[OBMF_ICP_SMC_GPIO_CHANNEL - 1] =
		CH0_SMC_GPIO_CHAN_CONFIG_OFFSET;
	discovery_struct.channel_offsets[OBMF_ICP_SGPIO0_CHANNEL - 1] =
		CH0_SGPIO0_CHAN_CONFIG_OFFSET;
	discovery_struct.channel_offsets[OBMF_ICP_IO_CHANNEL - 1] =
		CH0_IO_CHAN_CONFIG_OFFSET;
	discovery_struct.channel_offsets[OBMF_ICP_I2C_CONTROLLER_CHANNEL_0 - 1] =
		CH0_I2C_CHAN_CONFIG_OFFSET;
	discovery_struct.channel_offsets[OBMF_ICP_I2C_CONTROLLER_CHANNEL_1 - 1] =
		CH0_I2C1_CHAN_CONFIG_OFFSET;
	discovery_struct.channel_offsets[OBMF_ICP_I2C_CONTROLLER_CHANNEL_2 - 1] =
		CH0_I2C2_CHAN_CONFIG_OFFSET;
	discovery_struct.channel_offsets[OBMF_ICP_I2C_CONTROLLER_CHANNEL_3 - 1] =
		CH0_I2C3_CHAN_CONFIG_OFFSET;
	for (int i = 0; i < OBMF_MAX_I2C_TARGET_CHANNELS; i++) {
		discovery_struct.channel_offsets[OBMF_I2C_TGT_CHAN_NUM(i) - 1] =
			CH0_I2C_TARGET_CHAN_CONFIG_OFFSET(i);
	}
	discovery_struct.channel_offsets[OBMF_ICP_SERIAL_CHANNEL_1 - 1] =
		CH0_SERIAL_CHAN_1_CONFIG_OFFSET;
	discovery_struct.channel_offsets[OBMF_ICP_I3C_CONTROLLER_CHANNEL - 1] =
		CH0_I3C_CHAN_CONFIG_OFFSET;

	/* MMIO Channel Configuration Space */
	obmf_mmio_channel_config_t *mmio_cfg =
		&discovery_struct.mmio_channel_config;
	mmio_cfg->header.channel_type = OBMF_ICP_CHANNEL_TYPE_MMIO;
	mmio_cfg->header.channel_number = OBMF_ICP_MMIO_CHANNEL;
	strncpy(mmio_cfg->header.channel_name, "MMIO", 15);
	mmio_cfg->header.channel_status = CHAN_HEALTH_READY;
	mmio_cfg->header.channel_control = 0; /* Disabled by default */
	mmio_cfg->header.config_size = sizeof(obmf_mmio_config_data_t);
	mmio_cfg->config.max_mmio_read_size = OBMF_DEFAULT_MAX_TRANS_SIZE;
	mmio_cfg->config.max_mmio_write_size = OBMF_DEFAULT_MAX_TRANS_SIZE;

	/* Serial Channels Configuration Space (ch2, ch16) */
	static const uint8_t serial_chans[OBMF_MAX_SERIAL_CHANNELS] = {
		OBMF_ICP_SERIAL_CHANNEL_0,
		OBMF_ICP_SERIAL_CHANNEL_1,
	};
	static const char * const serial_names[OBMF_MAX_SERIAL_CHANNELS] = {
		"Serial",
		"Serial_1",
	};
	for (int i = 0; i < OBMF_MAX_SERIAL_CHANNELS; i++) {
		obmf_serial_channel_config_t *cfg =
			&discovery_struct.serial_channel_configs[i];
		cfg->header.channel_type = OBMF_ICP_CHANNEL_TYPE_SERIAL;
		cfg->header.channel_number = serial_chans[i];
		strncpy(cfg->header.channel_name, serial_names[i], 15);
		cfg->header.channel_status = CHAN_HEALTH_READY;
		cfg->header.channel_control = 0;
		cfg->header.config_size = sizeof(obmf_serial_config_data_t);
		cfg->config.uart_speed_hz = 115200;
		cfg->config.uart_data_bits = 8;
		cfg->config.uart_stop_bits = 1;
		cfg->config.uart_line_control = OBMF_SERIAL_PARITY_NONE;
	}

	/* IPMI Channel Configuration Space */
	obmf_ipmi_channel_config_t *ipmi_cfg =
		&discovery_struct.ipmi_channel_config;
	ipmi_cfg->header.channel_type = OBMF_ICP_CHANNEL_TYPE_IPMI;
	ipmi_cfg->header.channel_number = OBMF_ICP_IPMI_CHANNEL_NUM;
	strncpy(ipmi_cfg->header.channel_name, "IPMI", 15);
	ipmi_cfg->header.device_role = OBMF_CH_DEVICE_ROLE_CONSUMER; /* SMC requests, BMC responds */
	ipmi_cfg->header.channel_status = CHAN_HEALTH_READY;
	ipmi_cfg->header.channel_control = 0;
	ipmi_cfg->header.config_size = 0; /* No config data for IPMI */

	/* SPI Controller Channel Configuration Space (ch17: Producer, BMC sends to SMC / shd_dev) */
	obmf_spi_channel_config_t *spi_cfg = &discovery_struct.spi_channel_config;
	spi_cfg->header.channel_type = OBMF_ICP_CHANNEL_TYPE_SPI_CONTROLLER;
	spi_cfg->header.channel_number = OBMF_ICP_SPI_CHANNEL;
	strncpy(spi_cfg->header.channel_name, "SPI_SHD", 15);
	spi_cfg->header.device_role = OBMF_CH_DEVICE_ROLE_PRODUCER;
	spi_cfg->header.channel_status = CHAN_HEALTH_READY;
	spi_cfg->header.channel_control = 0;
	spi_cfg->header.config_size = sizeof(obmf_spi_config_data_t);
	spi_cfg->config.supported_config_flags =
		OBMF_SPI_FLAG_CPHA | OBMF_SPI_FLAG_CPOL;
	spi_cfg->config.config_flags = 0;
	spi_cfg->config.num_devices = 1;
	spi_cfg->config.rd_max_speed_hz = 50000000; /* 50 MHz */
	spi_cfg->config.wr_max_speed_hz = 50000000;

	/* Memory Device Channel Configuration Space (ch4: Consumer, SMC sends to BMC / eSPI-EDAF) */
	obmf_mem_dev_channel_config_t *mem_dev_cfg =
		&discovery_struct.mem_dev_channel_config;
	mem_dev_cfg->header.channel_type = OBMF_ICP_CHANNEL_TYPE_MEM_DEVICE;
	mem_dev_cfg->header.channel_number = OBMF_ICP_MEM_DEV_CHANNEL;
	strncpy(mem_dev_cfg->header.channel_name, "MEM_EDAF", 15);
	mem_dev_cfg->header.device_role = OBMF_CH_DEVICE_ROLE_CONSUMER;
	mem_dev_cfg->header.channel_status = CHAN_HEALTH_READY;
	mem_dev_cfg->header.channel_control = 0; /* Disabled by default */
	mem_dev_cfg->header.config_size = sizeof(obmf_mem_dev_config_data_t);
	mem_dev_cfg->config.capabilities = OBMF_MEM_DEV_CAP_WRITEABLE;
	mem_dev_cfg->config.device_size = 0x4000000; /* 64 MB */
	mem_dev_cfg->config.erase_size = 4;   /* 4 KB */
	mem_dev_cfg->config.read_size = 960;
	mem_dev_cfg->config.write_size = 256;

	/* GPIO Channel Configuration Space (ch5: VW GPIO) */
	obmf_gpio_channel_config_t *gpio_cfg =
		&discovery_struct.gpio_channel_config;
	gpio_cfg->header.channel_type = OBMF_ICP_CHANNEL_TYPE_GPIO;
	gpio_cfg->header.channel_number = OBMF_ICP_VW_GPIO_CHANNEL;
	strncpy(gpio_cfg->header.channel_name, "VW_GPIO", 15);
	gpio_cfg->header.channel_status = CHAN_HEALTH_READY;
	gpio_cfg->header.channel_control = 0;
	gpio_cfg->header.config_size = sizeof(obmf_gpio_config_data_t);
	gpio_cfg->config.gpio_count = 128; /* 0~63 output, 64~127 input */

	/* SGPIO Channel Configuration Space (ch6: SGPIO1) */
	obmf_gpio_channel_config_t *sgpio_cfg =
		&discovery_struct.sgpio1_channel_config;
	sgpio_cfg->header.channel_type = OBMF_ICP_CHANNEL_TYPE_GPIO;
	sgpio_cfg->header.channel_number = OBMF_ICP_SGPIO1_CHANNEL;
	strncpy(sgpio_cfg->header.channel_name, "SGPIO1", 15);
	sgpio_cfg->header.channel_status = CHAN_HEALTH_READY;
	sgpio_cfg->header.channel_control = 0;
	sgpio_cfg->header.config_size = sizeof(obmf_gpio_config_data_t);
	sgpio_cfg->config.gpio_count = 128; /* 0~63 output, 64~127 input */

	/* Platform GPIO Channel Configuration Space (ch7: BMC GPIO) */
	obmf_gpio_channel_config_t *smc_gpio_cfg =
		&discovery_struct.smc_gpio_channel_config;
	smc_gpio_cfg->header.channel_type = OBMF_ICP_CHANNEL_TYPE_GPIO;
	smc_gpio_cfg->header.channel_number = OBMF_ICP_SMC_GPIO_CHANNEL;
	strncpy(smc_gpio_cfg->header.channel_name, "SMC_GPIO", 15);
	smc_gpio_cfg->header.channel_status = CHAN_HEALTH_READY;
	smc_gpio_cfg->header.channel_control = 0;
	smc_gpio_cfg->header.config_size = sizeof(obmf_gpio_config_data_t);
	smc_gpio_cfg->config.gpio_count = 124;

	/* SGPIO0 Channel Configuration Space (ch8: SGPIO0) */
	obmf_gpio_channel_config_t *sgpio0_cfg =
		&discovery_struct.sgpio0_channel_config;
	sgpio0_cfg->header.channel_type = OBMF_ICP_CHANNEL_TYPE_GPIO;
	sgpio0_cfg->header.channel_number = OBMF_ICP_SGPIO0_CHANNEL;
	strncpy(sgpio0_cfg->header.channel_name, "SGPIO0", 15);
	sgpio0_cfg->header.channel_status = CHAN_HEALTH_READY;
	sgpio0_cfg->header.channel_control = 0;
	sgpio0_cfg->header.config_size = sizeof(obmf_gpio_config_data_t);
	sgpio0_cfg->config.gpio_count = 128;

	/* I/O Port Channel Configuration Space (ch9: I/O Port) */
	obmf_io_channel_config_t *io_cfg =
		&discovery_struct.io_channel_config;
	io_cfg->header.channel_type = OBMF_ICP_CHANNEL_TYPE_IO;
	io_cfg->header.channel_number = OBMF_ICP_IO_CHANNEL;
	strncpy(io_cfg->header.channel_name, "IO_PORT", 15);
	io_cfg->header.channel_status = CHAN_HEALTH_READY;
	io_cfg->header.channel_control = 0;
	io_cfg->header.config_size = sizeof(obmf_io_config_data_t);
	/*
	 * Advertise support for Sequential and Fixed 8-bit read/write
	 * (bits 0-1, 6-7) – the minimal set needed for POST code capture.
	 */
	io_cfg->config.hdr.trans_types_supported =
		(1u << OBMF_ICP_IO_TRANS_SEQ_READ_8)  |
		(1u << OBMF_ICP_IO_TRANS_SEQ_WRITE_8) |
		(1u << OBMF_ICP_IO_TRANS_FIXED_READ_8) |
		(1u << OBMF_ICP_IO_TRANS_FIXED_WRITE_8);
	io_cfg->config.hdr.io_range_count = 1;
	/* Range 0: POST code ports 0x80..0x83 */
	io_cfg->config.ranges[0].range_flags = OBMF_IO_RANGE_ENABLE;
	io_cfg->config.ranges[0].reserved    = 0;
	io_cfg->config.ranges[0].range_start = OBMF_IO_POST_CODE_PORT;
	io_cfg->config.ranges[0].range_end   = OBMF_IO_POST_CODE_PORT +
					       OBMF_IO_POST_CODE_SIZE - 1;
	io_cfg->config.ranges[0].range_mask  = 0x0000;
	io_cfg->config.ranges[0].service_type = OBMF_IO_SERVICE_POST_CODE;

	/* I2C Controller Channels 0-3 (ch10-ch13) */
	static const uint8_t i2c_ctrl_chans[OBMF_MAX_I2C_CONTROLLER_CHANNELS] = {
		OBMF_ICP_I2C_CONTROLLER_CHANNEL_0,
		OBMF_ICP_I2C_CONTROLLER_CHANNEL_1,
		OBMF_ICP_I2C_CONTROLLER_CHANNEL_2,
		OBMF_ICP_I2C_CONTROLLER_CHANNEL_3,
	};
	static const char * const i2c_ctrl_names[OBMF_MAX_I2C_CONTROLLER_CHANNELS] = {
		"I2C0", "I2C1", "I2C2", "I2C3",
	};
	for (int i = 0; i < OBMF_MAX_I2C_CONTROLLER_CHANNELS; i++) {
		obmf_i2c_channel_config_t *cfg =
			&discovery_struct.i2c_channel_configs[i];
		cfg->header.channel_type = OBMF_ICP_CHANNEL_TYPE_I2C_CONTROLLER;
		cfg->header.channel_number = i2c_ctrl_chans[i];
		strncpy(cfg->header.channel_name, i2c_ctrl_names[i], 15);
		cfg->header.channel_status = CHAN_HEALTH_READY;
		cfg->header.channel_control = 0; /* Disabled by default */
		cfg->header.config_size = sizeof(obmf_i2c_config_data_t);
		cfg->config.controller_speed = OBMF_I2C_SPEED_100KHZ;
		cfg->config.controller_options = 0;
	}

	/* I2C Target Channel Configuration Space (ch14-ch15) */
	for (int i = 0; i < OBMF_MAX_I2C_TARGET_CHANNELS; i++) {
		obmf_i2c_target_channel_config_t *tgt =
			&discovery_struct.i2c_target_channel_configs[i];
		tgt->header.channel_type = OBMF_ICP_CHANNEL_TYPE_I2C_TARGET;
		tgt->header.channel_number = OBMF_I2C_TGT_CHAN_NUM(i);
		snprintf(tgt->header.channel_name,
			 sizeof(tgt->header.channel_name),
			 "I2C_TGT%d", i);
		tgt->header.device_role = OBMF_CH_DEVICE_ROLE_CONSUMER; /* SMC requests, BMC responds */
		tgt->header.channel_status = CHAN_HEALTH_READY;
		tgt->header.channel_control = 0; /* Disabled by default */
		tgt->config.target_speed = OBMF_I2C_SPEED_100KHZ;
		tgt->config.reserved = 0;
		tgt->config.target_address_count = 0;
		tgt->header.config_size =
			offsetof(obmf_i2c_target_config_data_t, target_address_list) +
			tgt->config.target_address_count;
	}

	/* I3C Controller Channel Configuration Space (ch17) */
	obmf_i3c_channel_config_t *i3c_cfg =
		&discovery_struct.i3c_channel_config;
	i3c_cfg->header.channel_type = OBMF_ICP_CHANNEL_TYPE_I3C_CONTROLLER;
	i3c_cfg->header.channel_number = OBMF_ICP_I3C_CONTROLLER_CHANNEL;
	strncpy(i3c_cfg->header.channel_name, "I3C0", 15);
	i3c_cfg->header.channel_status = CHAN_HEALTH_READY;
	i3c_cfg->header.channel_control = 0; /* Disabled by default */
	i3c_cfg->header.config_size = sizeof(obmf_i3c_config_data_t);
	i3c_cfg->config.dev_nack_retry_count = 0;
	i3c_cfg->config.i3c_feature_flags = OBMF_I3C_FEATURE_IBI_PRIV_READ;
	i3c_cfg->config.max_chained_ops = 16;
	i3c_cfg->config.max_scl_pp_khz = 12500;  /* 12.5 MHz */
	i3c_cfg->config.max_scl_od_khz = 400;    /* 400 kHz */
	i3c_cfg->config.max_read_turnaround = 1000; /* 1 ms */
	i3c_cfg->config.max_dev_nack_retry = 3;
	i3c_cfg->config.max_payload_size = 256;   /* I3C_MAX_DATA_SIZE */
	i3c_cfg->config.max_ibi_payload_size = 128; /* IBI_PAYLOAD_SIZE */

	/* MAX_REQUEST/RESPONSE_PAYLOAD_SIZE: bounded by the shared internal
	 * response buffer (OBMF_RSP_BUF_SIZE) for every channel.
	 */
	for (int ch = 1; ch <= OBMF_MAX_CHANNELS; ch++) {
		obmf_channel_config_header_t *hdr = obmf_chan_cfg_hdr(ch);

		if (hdr) {
			hdr->max_request_payload_size = OBMF_RSP_BUF_SIZE;
			hdr->max_response_payload_size = OBMF_RSP_BUF_SIZE;
		}
	}

	/* Initialize channel tags */
	for (int i = 0; i < OBMF_MAX_CHANNELS + 1; i++) {
		channel_tags[i].is_outstanding = false;
		// First message on a channel shall use Tag = 0
		channel_tags[i].producer_tag = 0;
		channel_tags[i].consumer_tag = 0;
	}

	/* Initialise SPI producer role (ch17: shd_dev binding) */
	obmf_spi_producer_init();

	/* Initialise Memory Device flash page cache (ch4) */
	obmf_mem_dev_init();
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
			obmf_build_err_response(hdr, OBMF_ICP_ERR_OTHER,
						rsp_buf, rsp_len, true);
		}
		return -1;
	}

	/* Check if channel is valid */
	if (channel > OBMF_MAX_CHANNELS) {
		LOG_ERR("Invalid channel %d", channel);
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
	 * MMIO sub-header format (Transaction + Tag).
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
		/* I/O channel has its own 2-byte sub-header */
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
			/* We are the Consumer (sender) on these channels;
			 * incoming requests are not expected */
			LOG_WRN("Unexpected request on consumer channel %d",
				channel);
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
		case OBMF_ICP_I3C_CONTROLLER_CHANNEL: {
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

	if (channel_tags[channel].is_outstanding) {
		LOG_DBG("OBMF channel %d request is outstanding", channel);
		return -EBUSY;
	}

	obmf_icp_header_t *hdr = (obmf_icp_header_t *)req_buf;

	/* Set common header fields */
	OBMF_ICP_SET_CHANNEL(hdr, channel);
	OBMF_ICP_SET_RQRESP(hdr, OBMF_ICP_MSG_TYPE_REQUEST);
	/* Size = total payload size (everything after common header) */
	OBMF_ICP_SET_SIZE(hdr, req_len - OBMF_HEADER_SIZE);

	/* I/O channel retains a Tag for request/response correlation */
	if (channel == OBMF_ICP_IO_CHANNEL) {
		obmf_icp_io_sub_header_t *sub_hdr =
			(obmf_icp_io_sub_header_t *)(req_buf + OBMF_HEADER_SIZE);
		OBMF_ICP_IO_SET_TAG(sub_hdr, channel_tags[channel].consumer_tag);
		channel_tags[channel].consumer_tag ^= 1;
	}

	channel_tags[channel].is_outstanding = true;

	/* Send request via TX thread message queue */
	struct obmf_transport_msg msg;
	msg.buf = (uint8_t *)malloc(req_len);
	if (!msg.buf) {
		LOG_ERR("Failed to allocate buffer for OBMF request (%d bytes)",
			req_len);
		return -1;
	}
	memcpy(msg.buf, req_buf, req_len);
	msg.len = req_len;

	if (k_msgq_put(&obmf_tx_msgq, &msg, K_NO_WAIT) != 0) {
		LOG_WRN("Failed to queue OBMF request, queue full");
		free(msg.buf);
		return -1;
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

	/*
	 * Validate read_size against max transfer size per v0.9 spec:
	 * Use MMIO channel config max_mmio_read_size
	 */
	uint32_t max_read =
		discovery_struct.mmio_channel_config.config.max_mmio_read_size;
	if (read_size > max_read) {
		LOG_ERR("CH0 read size %d exceeds max %d",
			read_size, max_read);
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_ERR_ADDR_OUT_OF_RANGE,
					  rsp_buf, rsp_len, true);
	}

	if (addr + read_size > sizeof(discovery_struct)) {
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

	/* Copy MMIO sub-header from request */
	*rsp_sub = *req_sub;

	/* v1.0.0 RC1 Read Response: data in payload */
	memcpy(rsp_payload, (uint8_t *)&discovery_struct + addr, read_size);

	/* Size = MMIO sub-header (2) + read_size */
	OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_MMIO_SUB_HDR_SIZE + read_size);

	*rsp_len = OBMF_FULL_MMIO_HDR_SIZE + read_size;
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
	uint32_t min_write_size = addr_size + OBMF_SIZE_FIELD_LEN + 1;

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

	uint16_t write_size;

	memcpy(&write_size, &req_payload[addr_size], sizeof(uint16_t));

	uint8_t *write_data = req_payload + addr_size + OBMF_SIZE_FIELD_LEN;

	/* Validate total payload matches header + size + data */
	if (req_len != addr_size + OBMF_SIZE_FIELD_LEN + write_size) {
		LOG_ERR("CH0 write payload size mismatch: payload=%d,"
			" expected=%d",
			req_len,
			(int)(addr_size + OBMF_SIZE_FIELD_LEN + write_size));
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
					  rsp_buf, rsp_len, true);
	}

	LOG_DBG("CH0 %s Write Request: addr=0x%llx, size=%d",
		is_long ? "Long" : "Short", addr, write_size);

	/* Validate write_size > 0 */
	if (write_size == 0) {
		LOG_ERR("CH0 write size is 0");
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_ERR_CMD_NOT_SUPPORTED,
					  rsp_buf, rsp_len, true);
	}

	/*
	 * Validate write_size against max transfer size per v0.9 spec
	 */
	uint32_t max_write =
		discovery_struct.mmio_channel_config.config.max_mmio_write_size;
	if (write_size > max_write) {
		LOG_ERR("CH0 write size %d exceeds max %d",
			write_size, max_write);
		OBMF_BUILD_ERR_AND_RETURN(req_hdr, OBMF_ICP_ERR_ADDR_OUT_OF_RANGE,
					  rsp_buf, rsp_len, true);
	}

	if (addr + write_size > sizeof(discovery_struct)) {
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

	/* Copy MMIO sub-header from request */
	*rsp_sub = *req_sub_w;

	/*
	 * v0.9 Channel 0 writable fields:
	 * - CONFIG_STATUS (0x2B): MANUF_CONFIG_LOCK_REQUEST bit
	 * - Each channel config space: CHANNEL_CONTROL (offset 0x13)
	 *   and CONFIGURATION_DATA region
	 */

	/* Check all channel config spaces for writable regions */
	struct {
		uint32_t cfg_offset;
		uint32_t cfg_data_size;
	} chan_cfgs[] = {
		{ CH0_MMIO_CHAN_CONFIG_OFFSET,
		  sizeof(obmf_mmio_config_data_t) },
		{ CH0_SERIAL_CHAN_CONFIG_OFFSET,
		  sizeof(obmf_serial_config_data_t) },
		{ CH0_IPMI_CHAN_CONFIG_OFFSET, 0 },
		{ CH0_SPI_CHAN_CONFIG_OFFSET,
		  sizeof(obmf_spi_config_data_t) },
		{ CH0_MEM_DEV_CHAN_CONFIG_OFFSET,
		  sizeof(obmf_mem_dev_config_data_t) },
		{ CH0_GPIO_CHAN_CONFIG_OFFSET,
		  sizeof(obmf_gpio_config_data_t) },
		{ CH0_SGPIO1_CHAN_CONFIG_OFFSET,
		  sizeof(obmf_gpio_config_data_t) },
		{ CH0_SMC_GPIO_CHAN_CONFIG_OFFSET,
		  sizeof(obmf_gpio_config_data_t) },
		{ CH0_SGPIO0_CHAN_CONFIG_OFFSET,
		  sizeof(obmf_gpio_config_data_t) },
		{ CH0_IO_CHAN_CONFIG_OFFSET,
		  sizeof(obmf_io_config_data_t) },
		{ CH0_I2C_CHAN_CONFIG_OFFSET,
		  sizeof(obmf_i2c_config_data_t) },
		{ CH0_I2C1_CHAN_CONFIG_OFFSET,
		  sizeof(obmf_i2c_config_data_t) },
		{ CH0_I2C2_CHAN_CONFIG_OFFSET,
		  sizeof(obmf_i2c_config_data_t) },
		{ CH0_I2C3_CHAN_CONFIG_OFFSET,
		  sizeof(obmf_i2c_config_data_t) },
		{ CH0_I2C_TARGET_CHAN_CONFIG_OFFSET(0),
		  sizeof(obmf_i2c_target_config_data_t) },
		{ CH0_I2C_TARGET_CHAN_CONFIG_OFFSET(1),
		  sizeof(obmf_i2c_target_config_data_t) },
		{ CH0_SERIAL_CHAN_1_CONFIG_OFFSET,
		  sizeof(obmf_serial_config_data_t) },
		{ CH0_I3C_CHAN_CONFIG_OFFSET,
		  sizeof(obmf_i3c_config_data_t) },
	};

	for (int ci = 0; ci < (int)(sizeof(chan_cfgs) / sizeof(chan_cfgs[0])); ci++) {
		uint32_t cfg_base = chan_cfgs[ci].cfg_offset;
		uint32_t ctrl_off = cfg_base + CHAN_CFG_CONTROL_OFFSET;
		uint32_t data_off = cfg_base + CHAN_CFG_CONFIG_DATA_OFFSET;
		uint32_t data_end = data_off + chan_cfgs[ci].cfg_data_size;

		if (addr == ctrl_off && write_size == 1) {
			/* Write to CHANNEL_CONTROL register */
			uint8_t new_ctrl = write_data[0];
			uint8_t *ctrl_ptr = (uint8_t *)&discovery_struct + addr;
			uint8_t old_ctrl = *ctrl_ptr;
			obmf_channel_config_header_t *ch_hdr =
				(obmf_channel_config_header_t *)
				((uint8_t *)&discovery_struct + cfg_base);
			uint8_t ch_no = ch_hdr->channel_number;

			*ctrl_ptr = new_ctrl;

			/* Explicit host intent overrides a pending resume restore */
			obmf_suspended_mask &= ~BIT(ch_no);

			/* Handle channel enable/disable */
			bool was_enabled = (old_ctrl & CHAN_CONTROL_ENABLE) != 0;
			bool now_enabled = (new_ctrl & CHAN_CONTROL_ENABLE) != 0;
			if (was_enabled != now_enabled) {
				if (now_enabled) {
					LOG_INF("Channel %d enabled", ch_no);
					channel_tags[ch_no].producer_tag = 0;
					channel_tags[ch_no].consumer_tag = 0;
					channel_tags[ch_no].is_outstanding = false;
					ch_hdr->channel_status =
						CHAN_HEALTH_READY |
						CHAN_STATUS_CONFIG_VALID;
				} else {
					LOG_INF("Channel %d disabled", ch_no);
					channel_tags[ch_no].is_outstanding = false;
					ch_hdr->channel_status =
						CHAN_HEALTH_DISABLED;
				}
			}
			goto success;
		}

		if (chan_cfgs[ci].cfg_data_size > 0 &&
		    addr >= data_off && addr + write_size <= data_end) {
			/* Write to CONFIGURATION_DATA */
			memcpy((uint8_t *)&discovery_struct + addr,
			       write_data, write_size);
			goto success;
		}
	}

	/* No writable region matched */
	LOG_ERR("CH0 Write rejected: addr=0x%llx is outside"
		" known writable fields.", addr);
	OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr, OBMF_ICP_MSG_TYPE_RESPONSE,
				   OBMF_ICP_ERR_ADDR_OUT_OF_RANGE);
	OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_MMIO_SUB_HDR_SIZE);
	*rsp_len = OBMF_FULL_MMIO_HDR_SIZE;
	return;

success:
	/* Write response has no payload, status in header only */
	OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_MMIO_SUB_HDR_SIZE);
	*rsp_len = OBMF_FULL_MMIO_HDR_SIZE;
}
