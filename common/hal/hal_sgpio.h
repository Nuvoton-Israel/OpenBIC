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

#ifndef HAL_SGPIO_H
#define HAL_SGPIO_H

#include <stdbool.h>
#include <stdint.h>
#include <drivers/gpio.h>

/* Shared constants (also defined in hal_gpio.h) */
#ifndef ENABLE
#define ENABLE  1
#endif
#ifndef DISABLE
#define DISABLE 0
#endif
#ifndef GPIO_LOW
#define GPIO_LOW  0
#endif
#ifndef GPIO_HIGH
#define GPIO_HIGH 1
#endif

#ifdef CONFIG_SGPIO_NPCM4XX

/*
 * SGPIO hardware layout (NPCM4XX):
 *   Each SGPIO controller exposes multiple 8-pin ports via device_get_binding().
 *   Platform defines which devices exist and how many.
 *
 *   Pin numbering: global_pin = port_index * SGPIO_PINS_PER_PORT + pin_within_port
 *   Example (smc-npcm400-obmf):
 *     SGPIO0 Port 0-15  = "SGPIO0_0" to "SGPIO0_f" -> pins 0~127
 *     SGPIO1 Port 16-31 = "SGPIO1_0" to "SGPIO1_f" -> pins 128~255
 */
#define SGPIO_PINS_PER_PORT   8
#define SGPIO_PORTS_PER_GROUP 16
#define SGPIO_MAX_GROUPS      2
#define SGPIO_PINS_PER_GROUP  (SGPIO_PORTS_PER_GROUP * SGPIO_PINS_PER_PORT) /* 128 */
#define SGPIO_MAX_PORTS       (SGPIO_PORTS_PER_GROUP * SGPIO_MAX_GROUPS) /* 32 */
#define SGPIO_MAX_PINS        (SGPIO_MAX_PORTS * SGPIO_PINS_PER_PORT)     /* 256 */

#define SGPIO_GROUP0 0
#define SGPIO_GROUP1 1

typedef void (*sgpio_int_cb_t)(void);
typedef void (*sgpio_irq_cb_t)(uint8_t sgpio_group, uint8_t gpio_value,
				       uint8_t gpio_index);

/* SGPIO pin configuration entry */
typedef struct _SGPIO_CFG_ {
	uint8_t group;       /* SGPIO_GROUP0 / SGPIO_GROUP1 */
	uint8_t number;      /* group-local pin index (0~127) */
	uint8_t is_init;     /* ENABLE / DISABLE */
	uint16_t direction;  /* GPIO_INPUT / GPIO_OUTPUT */
	uint8_t status;      /* GPIO_LOW / GPIO_HIGH (initial output value) */
	int int_type;        /* GPIO_INT_DISABLE / GPIO_INT_EDGE_BOTH / ... */
	sgpio_int_cb_t int_cb; /* interrupt callback (NULL if no interrupt) */
	const char *name;    /* optional OBMF GPIO name override (NULL to use default) */
} SGPIO_CFG;

#define SGPIO_CFG_ENTRY(_group, _number, _is_init, _direction, _status, _int_type, _int_cb) \
	{ .group = (_group), .number = (_number), .is_init = (_is_init),                   \
	  .direction = (_direction),                                                         \
	  .status = (_status), .int_type = (_int_type), .int_cb = (_int_cb), .name = NULL }

#define SGPIO_CFG_ENTRY_NAMED(_group, _number, _is_init, _direction, _status, _int_type, _int_cb, _name) \
	{ .group = (_group), .number = (_number), .is_init = (_is_init),                          \
	  .direction = (_direction),                                                                \
	  .status = (_status), .int_type = (_int_type), .int_cb = (_int_cb), .name = (_name) }

/**
 * sgpio_get_by_group() - Read value of an SGPIO pin in group-local index space.
 * @group: SGPIO_GROUP0 or SGPIO_GROUP1
 * @gpio_index: Group-local pin index (0~127)
 * Return: 0/1 on success, -1 on error
 */
int sgpio_get_by_group(uint8_t group, uint16_t gpio_index);

/**
 * sgpio_set_by_group() - Write value to an SGPIO output pin in group-local index space.
 * @group: SGPIO_GROUP0 or SGPIO_GROUP1
 * @gpio_index: Group-local pin index (0~127)
 * @value: 0 or 1
 * Return: 0 on success, -1 on error
 */
int sgpio_set_by_group(uint8_t group, uint16_t gpio_index, uint8_t value);

/*
 * No flat-index (0~255) helpers here: sgpio_get()/sgpio_set() are already
 * declared by hal_gpio.h for its own CHIP_SGPIO implementation. Use the
 * group-local APIs above with devices bound by sgpio_group_init().
 */

/**
 * sgpio_get_port_index() - Get port index from device pointer.
 * @dev: Device pointer passed to IRQ handler
 * Return: Port index (0 to sgpio_port_count-1), or -1 if not found
 */
int sgpio_get_port_index(const struct device *dev);

/**
 * sgpio_group_init() - Initialize SGPIO devices and configure pins.
 * @dev_names: Array of device name strings (platform-provided)
 * @dev_count: Number of devices in dev_names[]
 * @cfg: Array of SGPIO_CFG entries to configure
 * @cfg_count: Number of entries in cfg[]
 * @handler: Platform callback invoked in HAL work queue context.
 *           Arguments are (sgpio_group, gpio_value, gpio_index).
 * Return: true on success
 *
 * This follows the same pattern as vw_gpio_init():
 *   - Binds all devices via device_get_binding()
 *   - Checks device readiness
 *   - Skips NULL/unavailable ports so partial-port enable is supported
 *   - Uses SGPIO group-local pin space (each group is 0~127)
 *   - Configures output pins
 *   - Registers interrupt callbacks for input pins with int_type != GPIO_INT_DISABLE
 */
bool sgpio_group_init(const char *dev_names[], uint8_t dev_count,
		      SGPIO_CFG cfg[], uint16_t cfg_count,
		      sgpio_irq_cb_t handler);

#endif /* CONFIG_SGPIO_NPCM4XX */

#endif /* HAL_SGPIO_H */
