/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef CONFIG_SGPIO_NPCM4XX

#include <zephyr.h>
#include <stdio.h>
#include <string.h>
#include <device.h>
#include <drivers/gpio.h>
#include <logging/log.h>
#include "hal_sgpio.h"

LOG_MODULE_REGISTER(hal_sgpio);

#define SGPIO_WORK_STACK_SIZE 2048

/* Device handles bound during sgpio_group_init() */
static const struct device *dev_sgpio[SGPIO_MAX_PORTS];
static uint8_t sgpio_port_count;

struct sgpio_port_callback {
	struct gpio_callback cb;
	uint8_t port;
};

struct sgpio_work_item {
	struct k_work work;
	uint16_t sgpio_num;
};

/* One GPIO callback struct per port (for input interrupt) */
static struct sgpio_port_callback sgpio_cbs[SGPIO_MAX_PORTS];
static bool sgpio_cb_registered[SGPIO_MAX_PORTS];
static struct sgpio_work_item sgpio_work_items[SGPIO_MAX_PINS];
static struct k_work_q sgpio_work_queue;
static K_THREAD_STACK_DEFINE(sgpio_work_stack, SGPIO_WORK_STACK_SIZE);
static bool sgpio_work_queue_started;
static uint16_t sgpio_work_items_inited;
static sgpio_irq_cb_t sgpio_platform_cb;

static bool sgpio_initialized;

static bool sgpio_parse_bank_char(char bank_char, uint8_t *bank)
{
	if (bank == NULL) {
		return false;
	}

	if (bank_char >= '0' && bank_char <= '9') {
		*bank = (uint8_t)(bank_char - '0');
		return true;
	}

	if (bank_char >= 'a' && bank_char <= 'f') {
		*bank = (uint8_t)(10 + bank_char - 'a');
		return true;
	}

	if (bank_char >= 'A' && bank_char <= 'F') {
		*bank = (uint8_t)(10 + bank_char - 'A');
		return true;
	}

	return false;
}

/*
 * Parse names like "SGPIO0_3" / "SGPIO1_a" into logical port index.
 * Returns true only when the canonical SGPIO naming format is matched.
 */
static bool sgpio_parse_logical_port(const char *name, uint8_t *logical_port)
{
	if (name == NULL || logical_port == NULL) {
		return false;
	}

	if (strlen(name) < 8 || strncmp(name, "SGPIO", 5) != 0 ||
	    name[6] != '_') {
		return false;
	}

	char group_char = name[5];
	if (group_char != '0' && group_char != '1') {
		return false;
	}

	uint8_t group = (uint8_t)(group_char - '0');
	uint8_t bank = 0;
	if (!sgpio_parse_bank_char(name[7], &bank)) {
		return false;
	}

	if (bank >= SGPIO_PORTS_PER_GROUP) {
		return false;
	}

	*logical_port = (uint8_t)(group * SGPIO_PORTS_PER_GROUP + bank);
	return true;
}

static bool sgpio_resolve_group_pin(uint8_t group, uint16_t gpio_index,
				    uint8_t *port, uint8_t *pin)
{
	if (group >= SGPIO_MAX_GROUPS || gpio_index >= SGPIO_PINS_PER_GROUP ||
	    port == NULL || pin == NULL) {
		return false;
	}

	uint8_t local_port = (uint8_t)(gpio_index / SGPIO_PINS_PER_PORT);
	*port = (uint8_t)(group * SGPIO_PORTS_PER_GROUP + local_port);
	*pin = (uint8_t)(gpio_index % SGPIO_PINS_PER_PORT);

	return true;
}

static void sgpio_work_handler(struct k_work *work)
{
	struct sgpio_work_item *item =
		CONTAINER_OF(work, struct sgpio_work_item, work);

	if (sgpio_platform_cb == NULL) {
		return;
	}

	uint8_t group = (uint8_t)(item->sgpio_num / SGPIO_PINS_PER_GROUP);
	uint8_t gpio_index = (uint8_t)(item->sgpio_num % SGPIO_PINS_PER_GROUP);

	int gpio_val = sgpio_get_by_group(group, gpio_index);
	if (gpio_val < 0) {
		return;
	}

	sgpio_platform_cb(group, (uint8_t)gpio_val, gpio_index);
}

static void sgpio_irq_callback(const struct device *port,
			       struct gpio_callback *cb,
			       gpio_port_pins_t pins)
{
	ARG_UNUSED(port);

	if (sgpio_platform_cb == NULL) {
		return;
	}

	struct sgpio_port_callback *port_cb =
		CONTAINER_OF(cb, struct sgpio_port_callback, cb);
	uint8_t port_index = port_cb->port;
	unsigned long pending = (unsigned long)pins;

	while (pending != 0UL) {
		uint8_t pin = (uint8_t)__builtin_ctzl(pending);
		pending &= (pending - 1UL);

		uint16_t sgpio_num =
			(uint16_t)(port_index * SGPIO_PINS_PER_PORT + pin);

		if (sgpio_num >= SGPIO_MAX_PINS) {
			continue;
		}

		k_work_submit_to_queue(&sgpio_work_queue,
					      &sgpio_work_items[sgpio_num].work);
	}

}

int sgpio_get_by_group(uint8_t group, uint16_t gpio_index)
{
	if (!sgpio_initialized)
		return -1;

	uint8_t port = 0;
	uint8_t pin = 0;
	if (!sgpio_resolve_group_pin(group, gpio_index, &port, &pin)) {
		return -1;
	}

	if (port >= sgpio_port_count || !dev_sgpio[port])
		return -1;

	return gpio_pin_get(dev_sgpio[port], pin);
}

int sgpio_get_port_index(const struct device *dev)
{
	if (!sgpio_initialized || !dev)
		return -1;

	for (int p = 0; p < sgpio_port_count; p++) {
		if (dev_sgpio[p] == dev)
			return p;
	}
	return -1;
}

int sgpio_set_by_group(uint8_t group, uint16_t gpio_index, uint8_t value)
{
	if (!sgpio_initialized)
		return -1;

	uint8_t port = 0;
	uint8_t pin = 0;
	if (!sgpio_resolve_group_pin(group, gpio_index, &port, &pin)) {
		return -1;
	}

	if (port >= sgpio_port_count || !dev_sgpio[port])
		return -1;

	return gpio_pin_set(dev_sgpio[port], pin, value);
}

static void sgpio_prepare_work_items(uint8_t active_ports)
{
	uint16_t required_work_items =
		(uint16_t)(active_ports * SGPIO_PINS_PER_PORT);

	if (required_work_items > SGPIO_MAX_PINS) {
		required_work_items = SGPIO_MAX_PINS;
	}

	for (uint16_t i = sgpio_work_items_inited; i < required_work_items; i++) {
		sgpio_work_items[i].sgpio_num = i;
		k_work_init(&sgpio_work_items[i].work, sgpio_work_handler);
	}

	sgpio_work_items_inited = required_work_items;
}

static void sgpio_start_work_queue_if_needed(void)
{
	if (sgpio_work_queue_started) {
		return;
	}

	k_work_queue_start(&sgpio_work_queue, sgpio_work_stack,
			   K_THREAD_STACK_SIZEOF(sgpio_work_stack),
			   K_PRIO_PREEMPT(CONFIG_MAIN_THREAD_PRIORITY),
			   NULL);
	k_thread_name_set(&sgpio_work_queue.thread, "sgpio_workq");
	sgpio_work_queue_started = true;
}

static void sgpio_unregister_port_callbacks(void)
{
	for (int p = 0; p < SGPIO_MAX_PORTS; p++) {
		if (dev_sgpio[p] == NULL || !sgpio_cb_registered[p]) {
			continue;
		}

		if (gpio_remove_callback(dev_sgpio[p], &sgpio_cbs[p].cb) < 0) {
			LOG_WRN("SGPIO: gpio_remove_callback failed for port %d", p);
		}

		sgpio_cb_registered[p] = false;
	}
}

static bool sgpio_register_port_callbacks(uint8_t port_count)
{
	for (int p = 0; p < port_count; p++) {
		if (dev_sgpio[p] == NULL) {
			continue;
		}

		sgpio_cbs[p].port = p;
		gpio_init_callback(&sgpio_cbs[p].cb, sgpio_irq_callback, UINT32_MAX);
		if (gpio_add_callback(dev_sgpio[p], &sgpio_cbs[p].cb) < 0) {
			LOG_ERR("SGPIO: gpio_add_callback failed for port %d", p);
			sgpio_unregister_port_callbacks();
			return false;
		}

		sgpio_cb_registered[p] = true;
	}

	return true;
}

bool sgpio_group_init(const char *dev_names[], uint8_t dev_count,
		      SGPIO_CFG cfg[], uint16_t cfg_count,
		      sgpio_irq_cb_t handler)
{
	if (dev_count > SGPIO_MAX_PORTS) {
		LOG_ERR("SGPIO: dev_count %d exceeds max %d", dev_count, SGPIO_MAX_PORTS);
		return false;
	}
	if (dev_count > 0 && dev_names == NULL) {
		LOG_ERR("SGPIO: dev_names is NULL when dev_count=%d", dev_count);
		return false;
	}

	sgpio_initialized = false;
	sgpio_port_count = 0;
	sgpio_unregister_port_callbacks();
	sgpio_platform_cb = handler;

	for (int p = 0; p < SGPIO_MAX_PORTS; p++) {
		dev_sgpio[p] = NULL;
		sgpio_cb_registered[p] = false;
	}

	uint8_t max_logical_port = 0;
	bool any_bound = false;

	/* Bind device handles */
	for (int p = 0; p < dev_count; p++) {
		if (!dev_names[p]) {
			LOG_DBG("SGPIO: dev_names[%d] is NULL, skip", p);
			continue;
		}

		const struct device *dev = device_get_binding(dev_names[p]);
		if (!dev) {
			LOG_DBG("SGPIO: skip unavailable device: %s", dev_names[p]);
			continue;
		}
		if (!device_is_ready(dev)) {
			LOG_DBG("SGPIO: skip not-ready device: %s", dev_names[p]);
			continue;
		}

		uint8_t logical_port = (uint8_t)p;
		if (!sgpio_parse_logical_port(dev_names[p], &logical_port)) {
			LOG_DBG("SGPIO: use fallback logical_port=%d for %s",
				logical_port, dev_names[p]);
		}

		if (logical_port >= SGPIO_MAX_PORTS) {
			LOG_ERR("SGPIO: logical_port %d out of range for %s",
				logical_port, dev_names[p]);
			return false;
		}

		if (dev_sgpio[logical_port] != NULL) {
			LOG_ERR("SGPIO: duplicate logical_port %d for %s",
				logical_port, dev_names[p]);
			return false;
		}

		dev_sgpio[logical_port] = dev;
		any_bound = true;
		if (logical_port > max_logical_port) {
			max_logical_port = logical_port;
		}
		LOG_DBG("SGPIO: device[%d] <= %s (input idx=%d)",
			logical_port, dev_names[p], p);
	}

	if (!any_bound) {
		/*
		 * DTS may intentionally disable SGPIO banks on some platforms.
		 * Keep initialization successful in a degraded 0-port mode.
		 */
		sgpio_port_count = 0;
		sgpio_initialized = true;
		LOG_WRN("SGPIO: no available devices are bound, run in 0-port mode");
		return true;
	}

	sgpio_port_count = (uint8_t)(max_logical_port + 1);

	/* IRQ path needs work items/work queue; skip this block when handler is NULL. */
	if (handler != NULL) {
		sgpio_prepare_work_items(sgpio_port_count);
		sgpio_start_work_queue_if_needed();

		if (!sgpio_register_port_callbacks(sgpio_port_count)) {
			return false;
		}
	}

	/* Configure pins per cfg[] */
	for (int i = 0; i < cfg_count; i++) {
		if (!cfg[i].is_init)
			continue;

		uint8_t port = 0;
		uint8_t pin = 0;
		if (!sgpio_resolve_group_pin(cfg[i].group, cfg[i].number,
					   &port, &pin)) {
			LOG_DBG("SGPIO: skip invalid cfg (group=%d index=%d)",
				cfg[i].group, cfg[i].number);
			continue;
		}

		if (port >= sgpio_port_count || !dev_sgpio[port]) {
			LOG_DBG("SGPIO: skip pin g%d:%d (port %d unavailable)",
				cfg[i].group, cfg[i].number, port);
			continue;
		}

		/* Configure output pins with initial value */
		if (cfg[i].direction == GPIO_OUTPUT) {
			gpio_flags_t f = GPIO_OUTPUT;
			f |= (cfg[i].status == GPIO_HIGH) ?
			     GPIO_OUTPUT_INIT_HIGH : GPIO_OUTPUT_INIT_LOW;
			gpio_pin_configure(dev_sgpio[port], pin, f);
		}

		/* Set up interrupt for input pins */
		if (cfg[i].int_type != GPIO_INT_DISABLE) {
			gpio_pin_interrupt_configure(dev_sgpio[port], pin, cfg[i].int_type);
		}
	}

	sgpio_initialized = true;
	LOG_INF("SGPIO initialized (%d ports, %d pin configs)", sgpio_port_count, cfg_count);
	return true;
}

#endif /* CONFIG_SGPIO_NPCM4XX */
