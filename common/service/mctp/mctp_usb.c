/*
 * Copyright (c) 2023 Nuvoton Technology Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mctp.h"
#include "libutil.h"
#include <usb/class/usb_mctp.h>
#include <logging/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/util.h>
#include <sys/byteorder.h>
#include <sys/printk.h>
#include <zephyr.h>
#include <sys/ring_buffer.h>


LOG_MODULE_DECLARE(mctp, LOG_LEVEL_DBG);

static const struct device *mctp_dev;

struct k_sem mctp_sem;
RING_BUF_DECLARE(mctp_ringbuf, MCTP_USB_BTU << 4);


uint32_t mctp_ringbuf_space(void)
{
	return ring_buf_space_get(&mctp_ringbuf);
}

/* Copy the oldest bytes without consuming them (no ring_buf_peek() in Zephyr 2.6) */
static uint32_t mctp_ringbuf_peek(uint8_t *data, uint32_t size)
{
	uint32_t total = 0, part;
	uint8_t *src;

	do {
		part = ring_buf_get_claim(&mctp_ringbuf, &src, size - total);
		memcpy(data + total, src, part);
		total += part;
	} while (total < size && part);

	ring_buf_get_finish(&mctp_ringbuf, 0);
	return total;
}

static uint16_t mctp_ringbuf_read(uint8_t *buf, uint32_t len, mctp_ext_params *extra_data)
{
	uint16_t ret = 0, rx_len, id;
	uint8_t rx_buff[MCTP_USB_BTU] = { 0 };
	struct mctp_usb_hdr *hdr;

	rx_len = ring_buf_capacity_get(&mctp_ringbuf) - ring_buf_space_get(&mctp_ringbuf);
	if (rx_len < sizeof(*hdr))
		return 0;

	/*
	 * Peek rather than consume: a frame that has not fully arrived yet must
	 * stay in the buffer, or its header is lost and every following frame is
	 * read at the wrong offset.
	 */
	mctp_ringbuf_peek(rx_buff, sizeof(*hdr));
	hdr = (struct mctp_usb_hdr *)rx_buff;
	LOG_DBG("mctp packet len: %d", hdr->len);

	if (hdr->len <= sizeof(*hdr) || hdr->len > MCTP_USB_BTU) {
		/* Not a valid header, so the stream is misaligned - resync */
		LOG_ERR("%s: invalid packet length %d, resyncing", __func__, hdr->len);
		ring_buf_get(&mctp_ringbuf, NULL, 1);
		return 0;
	}

	if (rx_len < hdr->len) {
		/* Rest of the frame is still in flight, try again later */
		return 0;
	}

	/* Whole frame is present, take it */
	rx_len = ring_buf_get(&mctp_ringbuf, rx_buff, hdr->len);
	if (rx_len == hdr->len) {
		id = sys_le16_to_cpu(hdr->id);

		if (sys_be16_to_cpu(id) != MCTP_USB_DMTF_ID) {
			LOG_ERR("%s: invalid id %04x", __func__, id);
			return 0;
		}

		extra_data->type = MCTP_MEDIUM_TYPE_USB;
		ret = hdr->len - sizeof(*hdr);
		memcpy(buf, rx_buff + sizeof(*hdr), hdr->len - sizeof(*hdr));
	}

	return ret;
}

static uint16_t mctp_usb_read(void *mctp_p, uint8_t *buf, uint32_t len,
				mctp_ext_params *extra_data)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_p, 0);
	CHECK_NULL_ARG_WITH_RETURN(buf, 0);
	CHECK_ARG_WITH_RETURN(!len, 0);
	CHECK_NULL_ARG_WITH_RETURN(extra_data, 0);

	uint16_t rt_size = 0;

	rt_size = mctp_ringbuf_read(buf, len, extra_data);

	while(rt_size == 0) {
		k_sem_take(&mctp_sem, K_FOREVER);
		rt_size = mctp_ringbuf_read(buf, len, extra_data);
	}
	
	return rt_size;
}

static uint8_t make_send_buf(mctp *mctp_inst, uint8_t *send_buf, uint32_t send_len,
				 uint8_t *mctp_data, uint32_t mctp_data_len, mctp_ext_params extra_data)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(send_buf, MCTP_ERROR);
	CHECK_ARG_WITH_RETURN(!send_len, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(mctp_data, MCTP_ERROR);
	CHECK_ARG_WITH_RETURN(!mctp_data_len, MCTP_ERROR);

	struct mctp_usb_hdr *hdr = (struct mctp_usb_hdr *)send_buf;
	hdr->id = sys_cpu_to_be16(MCTP_USB_DMTF_ID);;
	hdr->len = mctp_data_len + sizeof(*hdr);

	memcpy(send_buf + sizeof(*hdr), mctp_data, mctp_data_len);

	return MCTP_SUCCESS;
}
static uint16_t mctp_usb_write(void *mctp_p, uint8_t *buf, uint32_t len,
				 mctp_ext_params extra_data)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_p, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(buf, MCTP_ERROR);
	CHECK_ARG_WITH_RETURN(!len, MCTP_ERROR);

	mctp *mctp_inst = (mctp *)mctp_p;

	struct mctp_usb_hdr hdr;
	uint32_t send_len = len + sizeof(hdr);
	uint8_t send_buf[send_len], rc;
	uint32_t written = 0;

	LOG_HEXDUMP_DBG(buf, len, "mctp_usb_write receive data");

	if (extra_data.type != MCTP_MEDIUM_TYPE_USB)
		return MCTP_ERROR;

	rc = make_send_buf(mctp_inst, send_buf, send_len, buf, len, extra_data);
	if (rc == MCTP_ERROR) {
		LOG_WRN("make send buf failed!!");
		return MCTP_ERROR;
	}

	LOG_HEXDUMP_DBG(send_buf, send_len, "mctp_usb_write make header");

	rc = mctp_usb_ep_write(mctp_dev, send_buf, send_len, &written);
	if (rc) {
		LOG_DBG("usb_write failed with error %d", rc);
		return MCTP_ERROR;
	}

	if (written != send_len) {
		/* usb_write is expected to send all data */
		LOG_ERR("usb_write: requested %u sent %u",
				send_len, written);
		return MCTP_ERROR;
	}

	return MCTP_SUCCESS;
}

static int mctp_usb_rx(const struct device *dev, int32_t len,
			uint8_t *data)
{
	ARG_UNUSED(dev);

	/*
	 * A partial write leaves a torn frame in the buffer, and the reader has
	 * no way to tell where the next one starts - every frame after it is
	 * mis-parsed. Dropping the whole frame instead costs one retry from the
	 * peer and keeps the stream aligned.
	 */
	if (ring_buf_space_get(&mctp_ringbuf) < (uint32_t)len) {
		LOG_WRN("Ring buffer full, dropping a %d byte frame", len);
		return 0;
	}

	ring_buf_put(&mctp_ringbuf, data, len);

	k_sem_give(&mctp_sem);

	return 0;
}

static const struct mctp_ops mctp_usb_callbacks = {
	.read	= mctp_usb_rx,
};

uint8_t mctp_usb_init(mctp *mctp_inst, mctp_medium_conf medium_conf)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);

	mctp_inst->medium_conf = medium_conf;
	mctp_inst->read_data = mctp_usb_read;
	mctp_inst->write_data = mctp_usb_write;

	mctp_dev = device_get_binding("MCTP_0");
	if (!mctp_dev) {
		LOG_ERR("MCTP USB device not found");
		return MCTP_ERROR;
	}

	usb_mctp_register_device(mctp_dev, &mctp_usb_callbacks);
	k_sem_init(&mctp_sem, 0, 1);

	return MCTP_SUCCESS;
}

uint8_t mctp_usb_deinit(mctp *mctp_inst)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);

	mctp_inst->read_data = NULL;
	mctp_inst->write_data = NULL;
	memset(&mctp_inst->medium_conf, 0, sizeof(mctp_inst->medium_conf));
	return MCTP_SUCCESS;
}
