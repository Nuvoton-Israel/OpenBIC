/*
 * Copyright (c) 2024 Argentum Systems Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <kernel.h>
#include <device.h>
#include <drivers/uart.h>
#include <sys/ring_buffer.h>
#include <usb/usb_device.h>
#include <stdio.h>
#include <string.h>

struct path_info {
	const uint8_t * const name;
	const struct device *rx_dev;
	struct ring_buf *rx_ring_buf;
	const struct device *tx_dev;
};

void plat_uart_bridge_init(void);

#include <logging/log.h>
LOG_MODULE_REGISTER(uart_bridge, LOG_LEVEL_INF);

#define RING_BUF_SIZE 64

RING_BUF_DECLARE(rb_console, RING_BUF_SIZE);
struct path_info front_uart = {
	.name = "front",
	.rx_ring_buf = &rb_console,
};

RING_BUF_DECLARE(rb_other, RING_BUF_SIZE);
struct path_info end_uart = {
	.name = "end",
	.rx_ring_buf = &rb_other,
};

static void uart_cb(const struct device *dev, void *ctx)
{
	struct path_info *path = (struct path_info *)ctx;
	int ret;
	uint8_t buf[16], len;

	if (!(uart_irq_update(path->rx_dev))) { 
		return;
	}

	LOG_DBG("dev %p path->name %s \n", dev, path->name);

	if (uart_irq_rx_ready(path->rx_dev)) {
		len = uart_fifo_read(path->rx_dev, buf, sizeof(buf));
		if (len) {
			ret = ring_buf_put(path->rx_ring_buf, buf, len);
			if (ret < len) {
				LOG_ERR("Drop %zu bytes", len - ret);
			}
		}
	}

	if (uart_irq_tx_ready(path->tx_dev)) {
		len = ring_buf_get(path->rx_ring_buf, buf, sizeof(buf));
		if (!len) 
			return;

		ret = uart_fifo_fill(path->tx_dev, buf, len);
		if (ret < 0) 
			return;
	}
}

void plat_uart_bridge_init(void)
{
	uint32_t dtr = 0U;

	const struct device *acm_dev = device_get_binding("CDC_ACM_0");
	if (!acm_dev) {
		LOG_ERR("CDC_ACM_0 device not found");
		return;
	}

	LOG_INF("Wait for DTR \n");

	while (1) {
		uart_line_ctrl_get(acm_dev, UART_LINE_CTRL_DTR, &dtr);
		if (dtr) {
			break;
		}
		k_sleep(K_MSEC(100));
	}

	front_uart.rx_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_legacy_console));
	end_uart.rx_dev =  acm_dev;
	front_uart.tx_dev = acm_dev;
	end_uart.tx_dev =  DEVICE_DT_GET(DT_CHOSEN(zephyr_legacy_console));


	LOG_INF("Front Console Device: %p\n", front_uart.name);
	LOG_INF("End Console Device:  %p\n", end_uart.name);

	uart_irq_rx_enable(front_uart.rx_dev);
	uart_irq_rx_enable(end_uart.rx_dev);
	uart_irq_callback_user_data_set(front_uart.rx_dev, uart_cb, (void *)&front_uart);
	uart_irq_callback_user_data_set(end_uart.rx_dev, uart_cb, (void *)&end_uart);
}
