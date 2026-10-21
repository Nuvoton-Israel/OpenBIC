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

#ifdef CONFIG_GPIO_NPCM4XX_VW

#include "hal_vw_gpio.h"
#include <zephyr.h>
#include <device.h>
#include <drivers/gpio.h>
#include <logging/log.h>

LOG_MODULE_REGISTER(hal_vw_gpio);

/*
 * VW GPIO global pin -> bank/wire mapping:
 *   VWGPSM n, wire w  ->  global pin = n*4 + w          (0~63,  output BIC->Host)
 *   VWGPMS n, wire w  ->  global pin = 64 + n*4 + w     (64~127, input Host->BIC)
 *
 * Each bank device has only 4 pins (wire 0~3), so standard gpio_pin_get_raw /
 * gpio_pin_set_raw are safe -- BIT(0)~BIT(3) never wraps on any architecture.
 */

/* One GPIO callback struct per VWGPMS bank */
static struct gpio_callback vw_gpio_cbs[VW_GPIO_NUM_BANKS];

struct vw_gpio_work_item {
	struct k_work work;
	uint8_t gpio_index;
};

static struct vw_gpio_work_item vw_gpio_work_items[VW_GPIO_COUNT];
static struct k_work_q vw_gpio_work_queue;
static K_THREAD_STACK_DEFINE(vw_gpio_work_stack, 2048);
static bool vw_gpio_work_queue_started;
static vw_gpio_irq_cb_t vw_gpio_platform_cb;

static bool vw_gpio_initialized;
static const struct device *dev_vwgpsm[VW_GPIO_NUM_BANKS];
static const struct device *dev_vwgpms[VW_GPIO_NUM_BANKS];
static uint8_t vw_gpio_num_banks;

static void vw_gpio_work_handler(struct k_work *work)
{
	struct vw_gpio_work_item *item =
		CONTAINER_OF(work, struct vw_gpio_work_item, work);
	uint8_t gpio_index = item->gpio_index;

	if (vw_gpio_platform_cb == NULL) {
		return;
	}

	int gpio_val = vw_gpio_get(gpio_index);
	if (gpio_val < 0) {
		return;
	}

	vw_gpio_platform_cb((uint8_t)gpio_val, gpio_index);
}

static void vw_gpio_irq_callback(const struct device *port,
				 struct gpio_callback *cb,
				 gpio_port_pins_t pin)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);

	uint8_t gpio_index = (uint8_t)pin;

	if (gpio_index < VW_GPIO_INPUT_START || gpio_index >= VW_GPIO_COUNT) {
		return;
	}

	if (vw_gpio_platform_cb == NULL) {
		return;
	}

	k_work_submit_to_queue(&vw_gpio_work_queue, &vw_gpio_work_items[gpio_index].work);
}

int vw_gpio_get(uint8_t pin)
{
	if (!vw_gpio_initialized || pin >= VW_GPIO_COUNT) {
		return -1;
	}

	const struct device *dev;
	gpio_pin_t wire;

	if (pin < VW_GPIO_OUTPUT_PINS) {
		uint8_t bank = pin / VW_GPIO_PINS_PER_BANK;

		if (bank >= vw_gpio_num_banks) {
			return -1;
		}

		dev  = dev_vwgpsm[bank];
		wire = pin % VW_GPIO_PINS_PER_BANK;
	} else {
		uint8_t local = pin - VW_GPIO_OUTPUT_PINS;
		uint8_t bank = local / VW_GPIO_PINS_PER_BANK;

		if (bank >= vw_gpio_num_banks) {
			return -1;
		}

		dev  = dev_vwgpms[bank];
		wire = local % VW_GPIO_PINS_PER_BANK;
	}

	if (dev == NULL || !device_is_ready(dev)) {
		return -1;
	}

	int val = gpio_pin_get_raw(dev, wire);

	LOG_DBG("vw_gpio_get: pin=%d wire=%d val=%d", pin, wire, val);
	return val;
}

int vw_gpio_set(uint8_t pin, uint8_t value)
{
	if (!vw_gpio_initialized || pin >= VW_GPIO_OUTPUT_PINS) {
		return -1;
	}

	uint8_t bank = pin / VW_GPIO_PINS_PER_BANK;

	if (bank >= vw_gpio_num_banks) {
		return -1;
	}

	const struct device *dev = dev_vwgpsm[bank];

	if (dev == NULL || !device_is_ready(dev)) {
		return -1;
	}

	gpio_pin_t wire = pin % VW_GPIO_PINS_PER_BANK;
	int ret = gpio_pin_set_raw(dev, wire, value);

	LOG_DBG("vw_gpio_set: pin=%d wire=%d val=%d ret=%d", pin, wire, value, ret);
	return ret < 0 ? -1 : 0;
}

bool vw_gpio_init(const char *const vwgpsm_dev_names[],
		  const char *const vwgpms_dev_names[],
		  uint8_t dev_count,
		  vw_gpio_irq_cb_t handler)
{
	if (vwgpsm_dev_names == NULL || vwgpms_dev_names == NULL) {
		LOG_ERR("invalid vw gpio name table");
		return false;
	}

	if (dev_count == 0 || dev_count > VW_GPIO_NUM_BANKS) {
		LOG_ERR("invalid vw gpio bank count: %d", dev_count);
		return false;
	}

	vw_gpio_num_banks = dev_count;
	vw_gpio_platform_cb = handler;

	if (!vw_gpio_work_queue_started) {
		for (int i = 0; i < VW_GPIO_COUNT; i++) {
			vw_gpio_work_items[i].gpio_index = i;
			k_work_init(&vw_gpio_work_items[i].work, vw_gpio_work_handler);
		}

		k_work_queue_start(&vw_gpio_work_queue, vw_gpio_work_stack,
				   K_THREAD_STACK_SIZEOF(vw_gpio_work_stack),
				   K_PRIO_PREEMPT(CONFIG_MAIN_THREAD_PRIORITY), NULL);
		k_thread_name_set(&vw_gpio_work_queue.thread, "vw_gpio_workq");
		vw_gpio_work_queue_started = true;
	}

	for (int i = 0; i < dev_count; i++) {
		dev_vwgpsm[i] = NULL;
		dev_vwgpms[i] = NULL;

		if (vwgpsm_dev_names[i] != NULL) {
			dev_vwgpsm[i] = device_get_binding(vwgpsm_dev_names[i]);
			if (dev_vwgpsm[i] == NULL) {
				LOG_ERR("VWGPSM%d bind failed: %s", i, vwgpsm_dev_names[i]);
				return false;
			}
			if (!device_is_ready(dev_vwgpsm[i])) {
				LOG_ERR("VWGPSM%d not ready", i);
				return false;
			}
		}

		if (vwgpms_dev_names[i] != NULL) {
			dev_vwgpms[i] = device_get_binding(vwgpms_dev_names[i]);
			if (dev_vwgpms[i] == NULL) {
				LOG_ERR("VWGPMS%d bind failed: %s", i, vwgpms_dev_names[i]);
				return false;
			}
			if (!device_is_ready(dev_vwgpms[i])) {
				LOG_ERR("VWGPMS%d not ready", i);
				return false;
			}
		}

		if (handler != NULL && dev_vwgpms[i] != NULL) {
			gpio_init_callback(&vw_gpio_cbs[i], vw_gpio_irq_callback, UINT32_MAX);

			if (gpio_add_callback(dev_vwgpms[i], &vw_gpio_cbs[i]) < 0) {
				LOG_ERR("VWGPMS%d: gpio_add_callback failed", i);
				return false;
			}
		}
	}

	for (int i = dev_count; i < VW_GPIO_NUM_BANKS; i++) {
		dev_vwgpsm[i] = NULL;
		dev_vwgpms[i] = NULL;
	}

	vw_gpio_initialized = true;
	LOG_INF("VW GPIO initialized (%d output banks + %d input banks)",
		vw_gpio_num_banks, vw_gpio_num_banks);
	return true;
}

#else /* !CONFIG_GPIO_NPCM4XX_VW */

#include "hal_vw_gpio.h"
#include <logging/log.h>
#include <stdlib.h>
#include <string.h>
#include "libutil.h"

LOG_MODULE_REGISTER(hal_vw_gpio);

static const struct device *espi_dev;
static struct espi_callback vw_cb;
static vw_gpio *vw_gpio_cfg;
static uint8_t vw_gpio_size = 0;

extern void ISR_VW_GPIO(uint8_t gpio_value, uint8_t gpio_index);

bool vw_gpio_get(int number, uint8_t *value)
{
	CHECK_NULL_ARG_WITH_RETURN(value, false);

#ifdef CONFIG_ESPI_ASPEED
	uint32_t reg_value = sys_read32(AST_ESPI_BASE + AST_ESPI_GPIO_DIR);
	value[0] = GETBIT(reg_value, number) ? VW_GPIO_OUTPUT : VW_GPIO_INPUT;
	reg_value = sys_read32(AST_ESPI_BASE + AST_ESPI_GPIO_VAL);
	value[1] = GETBIT(reg_value, number) ? VW_GPIO_HIGH : VW_GPIO_LOW;
	return true;
#else
	return false;
#endif
}

bool vw_gpio_set(int number, uint8_t value)
{
#ifdef CONFIG_ESPI_ASPEED
	// Check if the vw gpio is master input and bic output pin
	uint32_t reg_value = sys_read32(AST_ESPI_BASE + AST_ESPI_GPIO_DIR);
	if (GETBIT(reg_value, number))
		return false;

	uint8_t i;
	for (i = 0; i < vw_gpio_size; i++) {
		if ((vw_gpio_cfg[i].number == number) && (vw_gpio_cfg[i].is_enabled) &&
		    (vw_gpio_cfg[i].direction == VW_GPIO_OUTPUT)) {
			reg_value = sys_read32(AST_ESPI_BASE + AST_ESPI_GPIO_VAL);
			if (value == VW_GPIO_HIGH)
				reg_value = SETBIT(reg_value, number);
			else
				reg_value = CLEARBIT(reg_value, number);
			sys_write32(reg_value, AST_ESPI_BASE + AST_ESPI_GPIO_VAL);
			return true;
		}
	}
#endif
	return false;
}

#ifdef CONFIG_ESPI_ASPEED
static void ast_vw_gpio_scan(void)
{
	uint8_t i;
	uint32_t reg_dir = sys_read32(AST_ESPI_BASE + AST_ESPI_GPIO_DIR);
	uint32_t reg_val = sys_read32(AST_ESPI_BASE + AST_ESPI_GPIO_VAL);
	for (i = 0; i < vw_gpio_size; i++) {
		if (!vw_gpio_cfg[i].is_enabled)
			continue;

		if (vw_gpio_cfg[i].direction == VW_GPIO_INPUT) {
			/* Check if controller direction setting
			 * ESPI9C: GPIO direction of virtual wire channel
			 */
			uint8_t gpio_value;
			if (GETBIT(reg_dir, vw_gpio_cfg[i].number) != VW_GPIO_INPUT) {
				gpio_value = VW_GPIO_UNKNOWN;
				LOG_ERR("the vgpio setting at controller side is incorrect.");
				return;
			} else {
				gpio_value = GETBIT(reg_val, vw_gpio_cfg[i].number) ? VW_GPIO_HIGH :
										      VW_GPIO_LOW;
			}

			/* Get virtual gpio value
			 * ESPI09C: GPIO through virtual wire channel
			 */
			if (gpio_value != vw_gpio_cfg[i].value) {
				vw_gpio_cfg[i].value = gpio_value;
				if (vw_gpio_cfg[i].int_cb) {
					vw_gpio_cfg[i].int_cb(gpio_value);
				}
			}
		}
	}
}
#endif

/* Handler for virtual GPIO from eSPI virtual wire channel */
static void vw_handler(const struct device *dev, struct espi_callback *cb, struct espi_event event)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);

#ifdef CONFIG_ESPI_ASPEED
	if (event.evt_type == ESPI_BUS_EVENT_VWIRE_RECEIVED) {
		/* Check if the virtual wire GPIO interrupt is happened
		 * ESPI008: Interrupt Status
		 *   bit[9] - Virtual Wire GPIO Event
		 */
		if (!GETBIT(event.evt_data, 9))
			return;

		ast_vw_gpio_scan();
	}
#else
	if (event.evt_type == ESPI_BUS_EVENT_VWIRE_RECEIVED) {
		uint8_t gpio_index = event.evt_details;
		uint8_t gpio_value = event.evt_data;
		ISR_VW_GPIO(gpio_value, gpio_index);
#if 0
		for (int i = 0; i < vw_gpio_size; i++) {
			if ((!vw_gpio_cfg[i].is_enabled) || (vw_gpio_cfg[i].number != gpio_num))
				continue;
			if (gpio_value != vw_gpio_cfg[i].value) {
				vw_gpio_cfg[i].value = gpio_value;
				if (vw_gpio_cfg[i].int_cb) {
					vw_gpio_cfg[i].int_cb(gpio_value);
				}
			}
		}
#endif
	}

#endif
}

void vw_gpio_reset()
{
	uint8_t i;
	for (i = 0; i < vw_gpio_size; i++) {
		if (vw_gpio_cfg[i].value != VW_GPIO_LOW) {
			vw_gpio_cfg[i].value = VW_GPIO_LOW;
			if (vw_gpio_cfg[i].int_cb) {
				vw_gpio_cfg[i].int_cb(VW_GPIO_LOW);
			}
		}
	}
}

bool vw_gpio_init(vw_gpio *config, uint8_t size)
{
	CHECK_NULL_ARG_WITH_RETURN(config, false);

#ifdef CONFIG_ESPI_ASPEED
	espi_dev = device_get_binding("ESPI");
#else
	espi_dev = device_get_binding("ESPI_0");
#endif
	if (!espi_dev) {
		LOG_ERR("failed to get espi device");
		return false;
	}

	CHECK_ARG_WITH_RETURN(vw_gpio_cfg != NULL, false);
	vw_gpio_cfg = (vw_gpio *)malloc(size * sizeof(vw_gpio));
	if (!vw_gpio_cfg) {
		LOG_ERR("failed to allocate memory");
		return false;
	}
	memcpy(vw_gpio_cfg, config, size * sizeof(vw_gpio));
	vw_gpio_size = size;

	espi_init_callback(&vw_cb, vw_handler, ESPI_BUS_EVENT_VWIRE_RECEIVED);
	if (espi_add_callback(espi_dev, &vw_cb) < 0) {
		LOG_ERR("failed to add espi callback function");
		SAFE_FREE(vw_gpio_cfg);
		vw_gpio_size = 0;
		return false;
	}

	// Initialize vGPIO status during BIC bootup
#ifdef CONFIG_ESPI_ASPEED
	ast_vw_gpio_scan();
#endif
	return true;


	SAFE_FREE(vw_gpio_cfg);
	vw_gpio_size = 0;
	return false;
}

#endif /* CONFIG_GPIO_NPCM4XX_VW */
