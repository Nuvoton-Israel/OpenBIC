/*
 * Copyright (c) 2024 Argentum Systems Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <kernel.h>
#include <device.h>
#include <drivers/uart.h>
#include <sys/ring_buffer.h>
#include <sys/atomic.h>
#include <stdio.h>
#include <string.h>

void plat_uart_bridge_init(void);

#include <logging/log.h>
LOG_MODULE_REGISTER(uart_bridge, LOG_LEVEL_INF);

#define RING_BUF_SIZE 128

/*
 * OBMF Serial channel bridges: bidirectional, one instance per
 * 'openbic,obmf-serial-N' chosen UART.
 *   UART RX -> OBMF Serial channel (send to host)
 *   OBMF Serial channel (from host) -> UART TX
 */
#include "obmf.h"

/*
 * Single source of truth for the bridge instances: adding a bridge means
 * adding one entry here plus the matching 'chosen' property.
 */
#define UART_BRIDGE_FOREACH(fn)                                                \
	IF_ENABLED(DT_HAS_CHOSEN(openbic_obmf_serial_0),                       \
		   (fn(0, openbic_obmf_serial_0,                               \
		       OBMF_ICP_SERIAL_CHANNEL_0, "ser0")))                    \
	IF_ENABLED(DT_HAS_CHOSEN(openbic_obmf_serial_1),                       \
		   (fn(1, openbic_obmf_serial_1,                               \
		       OBMF_ICP_SERIAL_CHANNEL_1, "ser1")))

#define UART_BRIDGE_COUNT_ONE(...) +1
#define OBMF_BRIDGE_COUNT (0 UART_BRIDGE_FOREACH(UART_BRIDGE_COUNT_ONE))

#if OBMF_BRIDGE_COUNT == 0

/* 'chosen' has no binding, so a mistyped property fails silently otherwise. */
#warning "No 'openbic,obmf-serial-N' chosen entry; OBMF UART bridge disabled"

void plat_uart_bridge_init(void)
{
	LOG_WRN("No 'openbic,obmf-serial-N' chosen entry; bridge disabled");
}

#else /* OBMF_BRIDGE_COUNT > 0 */

#define UART_BRIDGE_THREAD_STACK_SIZE 1024
#define UART_BRIDGE_THREAD_PRIORITY 8
#define UART_BRIDGE_UART_TX_PRIORITY 9

#define OBMF_SEND_BUF_SIZE    255
#define OBMF_FLUSH_TIMEOUT_MS 50
#define OBMF_TX_QUEUE_DEPTH   8

struct obmf_tx_msg {
	uint8_t data[OBMF_SEND_BUF_SIZE];
	uint16_t len;
};

struct uart_bridge_cfg {
	const struct device *uart;
	uint8_t channel;
	const char *name;
	const char *collector_name;
	const char *sender_name;
	const char *uart_tx_name;
	struct ring_buf *rb_tx; /* UART RX -> OBMF */
	struct ring_buf *rb_rx; /* OBMF -> UART TX */
	struct k_msgq *tx_queue;
	k_thread_stack_t *collector_stack;
	k_thread_stack_t *sender_stack;
	k_thread_stack_t *uart_tx_stack;
	size_t stack_size;
};

struct uart_bridge_data {
	const struct uart_bridge_cfg *cfg;
	struct k_sem rx_sem;
	struct k_sem tx_sem;
	struct k_thread collector_thread;
	struct k_thread sender_thread;
	struct k_thread uart_tx_thread;

	/*
	 * Aggregate count of UART RX bytes dropped because the ring buffer was
	 * full (e.g. the host streams a burst before the OBMF serial channel is
	 * enabled on first boot). Updated from the RX ISR and reported,
	 * rate-limited, by the collector thread so we never log per dropped
	 * byte.
	 */
	atomic_t drop_count;
};

/*
 * uart_irq_callback_user_data_set() allows a single callback per device, so a
 * bridge on the shell UART would silently replace the shell's handler.
 */
#if DT_HAS_CHOSEN(zephyr_shell_uart)
#define UART_BRIDGE_ASSERT_NOT_SHELL(chosen_id)                                \
	BUILD_ASSERT(!DT_SAME_NODE(DT_CHOSEN(chosen_id),                       \
				   DT_CHOSEN(zephyr_shell_uart)),              \
		     "OBMF UART bridge must not use the shell UART")
#else
#define UART_BRIDGE_ASSERT_NOT_SHELL(chosen_id) BUILD_ASSERT(1, "")
#endif

#define UART_BRIDGE_DEFINE(idx, chosen_id, chan, nm)                           \
	RING_BUF_DECLARE(rb_tx_##idx, RING_BUF_SIZE);                          \
	RING_BUF_DECLARE(rb_rx_##idx, RING_BUF_SIZE);                          \
	K_MSGQ_DEFINE(tx_queue_##idx, sizeof(struct obmf_tx_msg),              \
		      OBMF_TX_QUEUE_DEPTH, 4);                                 \
	K_THREAD_STACK_DEFINE(collector_stack_##idx,                           \
			      UART_BRIDGE_THREAD_STACK_SIZE);                  \
	K_THREAD_STACK_DEFINE(sender_stack_##idx,                              \
			      UART_BRIDGE_THREAD_STACK_SIZE);                  \
	K_THREAD_STACK_DEFINE(uart_tx_stack_##idx,                             \
			      UART_BRIDGE_THREAD_STACK_SIZE);                  \
	static const struct uart_bridge_cfg bridge_cfg_##idx = {               \
		.uart = DEVICE_DT_GET(DT_CHOSEN(chosen_id)),                   \
		.channel = (chan),                                             \
		.name = nm,                                                    \
		.collector_name = nm "_col",                                   \
		.sender_name = nm "_snd",                                      \
		.uart_tx_name = nm "_tx",                                      \
		.rb_tx = &rb_tx_##idx,                                         \
		.rb_rx = &rb_rx_##idx,                                         \
		.tx_queue = &tx_queue_##idx,                                   \
		.collector_stack = collector_stack_##idx,                      \
		.sender_stack = sender_stack_##idx,                            \
		.uart_tx_stack = uart_tx_stack_##idx,                          \
		.stack_size = K_THREAD_STACK_SIZEOF(collector_stack_##idx),    \
	};                                                                     \
	static struct uart_bridge_data bridge_data_##idx = {                   \
		.cfg = &bridge_cfg_##idx,                                      \
	};                                                                     \
	UART_BRIDGE_ASSERT_NOT_SHELL(chosen_id);

#define UART_BRIDGE_PTR(idx, chosen_id, chan, nm) &bridge_data_##idx,

UART_BRIDGE_FOREACH(UART_BRIDGE_DEFINE)

static struct uart_bridge_data *const bridges[] = {
	UART_BRIDGE_FOREACH(UART_BRIDGE_PTR)
};

BUILD_ASSERT(ARRAY_SIZE(bridges) <= OBMF_MAX_SERIAL_CHANNELS,
	     "more UART bridges than OBMF Serial channels");

/* ISR: UART RX -> ring buffer, UART TX <- ring buffer */
static void uart_rx_cb(const struct device *dev, void *ctx)
{
	struct uart_bridge_data *bridge = ctx;
	uint8_t buf[16];
	int len;

	if (!uart_irq_update(dev))
		return;

	if (uart_irq_rx_ready(dev)) {
		len = uart_fifo_read(dev, buf, sizeof(buf));
		if (len > 0) {
			int ret = ring_buf_put(bridge->cfg->rb_tx, buf, len);
			if (ret < len) {
				/*
				 * Don't log here: at first boot this can fire
				 * per byte and flood the console. Just count;
				 * the collector thread reports periodically.
				 */
				atomic_add(&bridge->drop_count, len - ret);
			}
			k_sem_give(&bridge->rx_sem);
		}
	}
}

/* Thread: drain ring buffer → queue packets (never blocks on serial send) */
static void uart_to_obmf_thread(void *arg1, void *arg2, void *arg3)
{
	struct uart_bridge_data *bridge = arg1;
	const struct uart_bridge_cfg *cfg = bridge->cfg;
	struct obmf_tx_msg msg = { .len = 0 };
	/* Fixed drain chunk: keep stack small and independent of RING_BUF_SIZE */
	uint8_t tmp[128];

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		int ret = k_sem_take(&bridge->rx_sem,
				     K_MSEC(OBMF_FLUSH_TIMEOUT_MS));

		/*
		 * Report any UART RX drops at most once per flush period so a
		 * burst (common on first boot before the host enables the
		 * serial channel) produces a single summary line instead of
		 * thousands of per-byte warnings.
		 */
		uint32_t dropped = (uint32_t)atomic_clear(&bridge->drop_count);
		if (dropped > 0) {
			LOG_WRN("%s: UART RX overflow: dropped %u bytes",
				cfg->name, dropped);
		}

		/*
		 * Channel down (host gone or not enabled yet): discard console
		 * output at the source, otherwise the ring and the TX queue
		 * saturate and every wake logs an overflow.
		 */
		if (!obmf_channel_is_enabled(cfg->channel)) {
			ring_buf_reset(cfg->rb_tx);
			msg.len = 0;
			continue;
		}

		uint32_t got = ring_buf_get(cfg->rb_tx, tmp, sizeof(tmp));
		if (got > 0) {
			uint32_t avail = OBMF_SEND_BUF_SIZE - msg.len;
			uint32_t copy = MIN(got, avail);

			memcpy(msg.data + msg.len, tmp, copy);
			msg.len += copy;

			if (msg.len >= OBMF_SEND_BUF_SIZE) {
				if (k_msgq_put(cfg->tx_queue, &msg,
					       K_NO_WAIT) != 0) {
					LOG_WRN("%s: TX queue full, drop %d bytes",
						cfg->name, msg.len);
				}
				msg.len = 0;
				uint32_t leftover = got - copy;
				if (leftover > 0) {
					memcpy(msg.data, tmp + copy, leftover);
					msg.len = leftover;
				}
			}
		}

		/* Flush on timeout with pending data */
		if (ret == -EAGAIN && msg.len > 0) {
			if (k_msgq_put(cfg->tx_queue, &msg, K_NO_WAIT) != 0) {
				LOG_WRN("%s: TX queue full, drop %d bytes",
					cfg->name, msg.len);
			} else {
				msg.len = 0;
			}
		}
	}
}

/*
 * Refill callback for obmf_serial_send(): appends the next queued message
 * into the current (or retry) packet without an extra OBMF round-trip.
 *
 * Use peek-then-get: only dequeue the message if it fits entirely in the
 * remaining space.  If it does not fit, leave it in the queue so that the
 * next round-trip (or the next call from obmf_send_thread) sends it complete.
 * This prevents the previous truncation bug where k_msgq_get was called
 * unconditionally and bytes beyond 'space' were silently discarded.
 */
static uint16_t refill_from_msgq(uint8_t *buf, uint16_t space, void *ctx)
{
	struct uart_bridge_data *bridge = ctx;
	struct obmf_tx_msg fill;

	/* Peek first: do not remove the message until we know it fits. */
	if (k_msgq_peek(bridge->cfg->tx_queue, &fill) != 0)
		return 0;

	if (fill.len > space)
		return 0;

	/* Space is sufficient – now dequeue and copy. */
	if (k_msgq_get(bridge->cfg->tx_queue, &fill, K_NO_WAIT) != 0)
		return 0;

	memcpy(buf, fill.data, fill.len);
	return fill.len;
}

/* Thread: dequeue packets → obmf_serial_send (partial-accept retry) */
static void obmf_send_thread(void *arg1, void *arg2, void *arg3)
{
	struct uart_bridge_data *bridge = arg1;
	const struct uart_bridge_cfg *cfg = bridge->cfg;
	struct obmf_tx_msg msg;

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		k_msgq_get(cfg->tx_queue, &msg, K_FOREVER);

		if (!obmf_channel_is_enabled(cfg->channel)) {
			k_msgq_purge(cfg->tx_queue);
			continue;
		}

		int rc = obmf_serial_send(cfg->channel, msg.data, msg.len,
					  refill_from_msgq, bridge);
		if (rc != 0 && rc != -ENETDOWN) {
			LOG_WRN("%s: obmf_serial_send failed: %d",
				cfg->name, rc);
		}
	}
}

/*
 * Host->UART output ring, drained by a dedicated low-priority thread.
 *
 * obmf_serial_rx_cb() runs in the OBMF RX dispatch thread, which is created at
 * K_PRIO_PREEMPT(1) (priority 1) - far higher than the UART->OBMF collector
 * (priority 8). The previous implementation called uart_poll_out() directly in
 * that callback; uart_poll_out() busy-waits for the TX FIFO (~87us/byte at
 * 115200). When the host streamed a chunk back, the prio-1 thread spun for many
 * milliseconds, starving the collector while the UART RX ISR kept filling
 * rb_tx -> the 128B ring overflowed and dropped bytes.
 *
 * Decouple by having the callback only enqueue into this ring (non-blocking)
 * and letting a low-priority thread do the blocking uart_poll_out, so the
 * prio-1 RX thread returns immediately and never starves the collector.
 */

/* Thread: drain host->UART ring → UART (blocking poll_out) */
static void uart_tx_thread(void *arg1, void *arg2, void *arg3)
{
	struct uart_bridge_data *bridge = arg1;
	const struct uart_bridge_cfg *cfg = bridge->cfg;
	uint8_t tmp[64];

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		k_sem_take(&bridge->tx_sem, K_FOREVER);

		uint32_t got;
		while ((got = ring_buf_get(cfg->rb_rx, tmp, sizeof(tmp))) > 0) {
			for (uint32_t i = 0; i < got; i++) {
				uart_poll_out(cfg->uart, tmp[i]);
			}
		}
	}
}

/*
 * Callback: Host sends serial data to us → forward to UART (non-blocking).
 * Returns the number of bytes written so the OBMF layer can send a partial
 * NACK when rb_rx is full (spec §partial-accept retry rule).
 */
static uint16_t obmf_serial_rx_cb(const uint8_t *data, uint16_t len, void *ctx)
{
	struct uart_bridge_data *bridge = ctx;
	uint16_t accepted = (uint16_t)ring_buf_put(bridge->cfg->rb_rx,
						   data, len);

	k_sem_give(&bridge->tx_sem);
	return accepted;
}

static void uart_bridge_start(struct uart_bridge_data *bridge)
{
	const struct uart_bridge_cfg *cfg = bridge->cfg;
	uint8_t c;

	if (!device_is_ready(cfg->uart)) {
		LOG_ERR("%s: UART device not ready", cfg->name);
		return;
	}

	k_sem_init(&bridge->rx_sem, 0, 1);
	k_sem_init(&bridge->tx_sem, 0, 1);

	/* Drain any pending data */
	uart_irq_rx_disable(cfg->uart);
	while (uart_fifo_read(cfg->uart, &c, 1) > 0) {
		continue;
	}

	/* Set up RX interrupt to capture UART input */
	uart_irq_callback_user_data_set(cfg->uart, uart_rx_cb, bridge);
	uart_irq_rx_enable(cfg->uart);

	/* UART → OBMF collector thread */
	k_thread_create(&bridge->collector_thread, cfg->collector_stack,
			cfg->stack_size, uart_to_obmf_thread, bridge, NULL,
			NULL, UART_BRIDGE_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&bridge->collector_thread, cfg->collector_name);

	/* OBMF TX sender thread */
	k_thread_create(&bridge->sender_thread, cfg->sender_stack,
			cfg->stack_size, obmf_send_thread, bridge, NULL, NULL,
			UART_BRIDGE_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&bridge->sender_thread, cfg->sender_name);

	/* Host → UART drain thread (keeps blocking poll_out off the
	 * high-priority OBMF RX thread) */
	k_thread_create(&bridge->uart_tx_thread, cfg->uart_tx_stack,
			cfg->stack_size, uart_tx_thread, bridge, NULL, NULL,
			UART_BRIDGE_UART_TX_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&bridge->uart_tx_thread, cfg->uart_tx_name);

	/* OBMF → UART: register serial RX callback from host */
	obmf_register_serial_rx_cb(cfg->channel, obmf_serial_rx_cb, bridge);

	LOG_INF("%s: UART-to-OBMF Serial bridge initialized (channel %d)",
		cfg->name, cfg->channel);
}

void plat_uart_bridge_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(bridges); i++) {
		bool duplicate = false;

		/* A second callback on the same UART would replace the first. */
		for (size_t j = 0; j < i; j++) {
			if (bridges[j]->cfg->uart == bridges[i]->cfg->uart) {
				duplicate = true;
				break;
			}
		}

		if (duplicate) {
			LOG_ERR("%s: UART already bridged, skipping",
				bridges[i]->cfg->name);
			continue;
		}

		uart_bridge_start(bridges[i]);
	}
}

#endif /* OBMF_BRIDGE_COUNT == 0 */

