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

#ifndef HAL_VW_GPIO_H
#define HAL_VW_GPIO_H

#ifdef CONFIG_GPIO_NPCM4XX_VW

#include <stdbool.h>
#include <stdint.h>
#include <drivers/gpio.h>

/*
 * VW GPIO hardware layout (NPCM4XX eSPI Virtual Wire GPIO):
 *   VWGPSM0~15: 16 output banks (BIC -> Host), global pins 0~63
 *   VWGPMS0~15: 16 input  banks (Host -> BIC), global pins 64~127
 *   Each bank = one 4-byte register, 4 wires (pins 0~3 within bank).
 */
#define VW_GPIO_NUM_BANKS      16
#define VW_GPIO_PINS_PER_BANK  4
#define VW_GPIO_OUTPUT_PINS    (VW_GPIO_NUM_BANKS * VW_GPIO_PINS_PER_BANK)  /* 64  */
#define VW_GPIO_COUNT          (VW_GPIO_OUTPUT_PINS * 2)                     /* 128 */
#define VW_GPIO_OUTPUT_START   0
#define VW_GPIO_OUTPUT_END     (VW_GPIO_OUTPUT_PINS - 1)
#define VW_GPIO_INPUT_START    VW_GPIO_OUTPUT_PINS
#define VW_GPIO_INPUT_END      (VW_GPIO_COUNT - 1)


/* Direction values (used in IPMI VW GPIO responses) */
#define VW_GPIO_DIR_INPUT   0
#define VW_GPIO_DIR_OUTPUT  1

/* Legacy direction enum aliases kept for compatibility */
#define VW_GPIO_INPUT  VW_GPIO_DIR_INPUT
#define VW_GPIO_OUTPUT VW_GPIO_DIR_OUTPUT

typedef void (*vw_gpio_irq_cb_t)(uint8_t gpio_value, uint8_t gpio_index);

/**
 * vw_gpio_get() - Read a VW GPIO pin value via direct register access.
 * @pin: Pin number (0~127)
 * Return: 1 = high, 0 = low, -1 = error/out-of-range
 */
int vw_gpio_get(uint8_t pin);

/**
 * vw_gpio_set() - Write a VW GPIO output pin via direct register access.
 * @pin:   Pin number (0~63, output only)
 * @value: 1 = high, 0 = low
 * Return: 0 = success, -1 = error
 */
int vw_gpio_set(uint8_t pin, uint8_t value);

/**
 * vw_gpio_init() - Initialize VW GPIO devices and register input callbacks.
 * @vwgpsm_dev_names: Platform-provided output bank device names.
 * @vwgpms_dev_names: Platform-provided input bank device names.
 * @dev_count: Number of banks in the name arrays.
 * @handler: Platform callback invoked in HAL work queue context.
 *           Arguments are (gpio_value, gpio_index).
 * Return: true on success
 */
bool vw_gpio_init(const char *const vwgpsm_dev_names[],
		  const char *const vwgpms_dev_names[],
		  uint8_t dev_count,
		  vw_gpio_irq_cb_t handler);

#else /* !CONFIG_GPIO_NPCM4XX_VW */

#include <drivers/espi.h>

// Aspeed ESPI register definition
#define AST_ESPI_BASE 0x7E6EE000
#define AST_ESPI_SYSEVT 0x98
#define AST_ESPI_GPIO_VAL 0x9C
#define AST_ESPI_GPIO_DIR 0xC0

#define VW_GPIO_ENABLE true
#define VW_GPIO_DISABLE false

enum vw_gpio_direction {
	VW_GPIO_OUTPUT = 0,
	VW_GPIO_INPUT,
};

enum vw_gpio_value {
	VW_GPIO_LOW = 0,
	VW_GPIO_HIGH,
	VW_GPIO_UNKNOWN,
};

typedef struct _vw_gpio_ {
	uint8_t number;
	bool is_enabled;
	uint8_t direction;
	uint8_t value;
	void (*int_cb)(uint8_t value);
} vw_gpio;

bool vw_gpio_get(int number, uint8_t *value);
bool vw_gpio_set(int number, uint8_t value);
void vw_gpio_reset(void);
bool vw_gpio_init(vw_gpio *config, uint8_t size);

#endif /* CONFIG_GPIO_NPCM4XX_VW */

#endif /* HAL_VW_GPIO_H */
