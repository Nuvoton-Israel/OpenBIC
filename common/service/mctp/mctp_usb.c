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
RING_BUF_DECLARE(mctp_ringbuf, MCTP_USB_BTU << 2);

static uint16_t mctp_ringbuf_read(uint8_t *buf, uint32_t len, mctp_ext_params *extra_data)
{
	uint16_t ret = 0, rx_len, id;
	uint8_t rx_buff[MCTP_USB_BTU] = { 0 };
	struct mctp_usb_hdr *hdr;

	rx_len = ring_buf_get(&mctp_ringbuf, rx_buff, sizeof(rx_buff));
	if (rx_len) {
		if (rx_len < sizeof(*hdr)) {
			LOG_ERR("recv invalid len %d", rx_len);
			return 0;
		}

		hdr = (struct mctp_usb_hdr *)rx_buff;
		id = sys_le16_to_cpu(hdr->id);

		if (id != MCTP_USB_DMTF_ID) {
			LOG_ERR("%s: invalid id %04x\n", __func__, id);
			return 0;
		}

		extra_data->type = MCTP_MEDIUM_TYPE_USB;
		ret = hdr->len - sizeof(*hdr);
		memcpy(buf, rx_buff + sizeof(*hdr), ret);

		if (hdr->len < rx_len)
			ring_buf_put(&mctp_ringbuf, rx_buff + hdr->len, rx_len - hdr->len);
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
	hdr->id = sys_cpu_to_le16(MCTP_USB_DMTF_ID);;
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
	int wrote;

	wrote = ring_buf_put(&mctp_ringbuf, data, len);
	if (wrote < len)
		LOG_WRN("Ring buffer full, drop %zd bytes", len - wrote);

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
