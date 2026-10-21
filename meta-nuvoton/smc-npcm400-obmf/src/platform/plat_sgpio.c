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
#include <stdint.h>
#include <devicetree.h>
#include "hal_sgpio.h"
#include "plat_sgpio.h"

#include "obmf.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(plat_sgpio, LOG_LEVEL_DBG);

/* =====================================================================
 * SGPIO via Zephyr GPIO driver (nuvoton,npcm4xx-sgpio)
 *   Pin 0~63   : Output (SGPIO1_0 to SGPIO1_7, BIC drives)
 *   Pin 64~127 : Input  (SGPIO1_8 to SGPIO1_f, loopback/external)
 * ===================================================================== */

/* SGPIO device name table for smc-npcm400-obmf */
#define PLAT_SGPIO_GROUP_DEV_COUNT 16
#define PLAT_SGPIO_DEV_COUNT (PLAT_SGPIO_GROUP_DEV_COUNT * 2)

#define SGPIO_BANK_NAME(prefix, idx) \
	(DT_NODE_HAS_STATUS(DT_NODELABEL(prefix##idx), okay) ? \
	 DEVICE_DT_NAME(DT_NODELABEL(prefix##idx)) : NULL)

#define SGPIO_BANK_LINE_NAME(prefix, bank, bit) \
	COND_CODE_1(DT_NODE_HAS_STATUS(DT_NODELABEL(prefix##bank), okay), \
		(COND_CODE_1(DT_NODE_HAS_PROP(DT_NODELABEL(prefix##bank), gpio_line_names), \
			(DT_PROP_BY_IDX(DT_NODELABEL(prefix##bank), gpio_line_names, bit)), \
			(NULL))), \
		(NULL))


#define SGPIO_GROUP_DEV_NAMES(prefix) \
	{ \
		SGPIO_BANK_NAME(prefix, 0), SGPIO_BANK_NAME(prefix, 1), \
		SGPIO_BANK_NAME(prefix, 2), SGPIO_BANK_NAME(prefix, 3), \
		SGPIO_BANK_NAME(prefix, 4), SGPIO_BANK_NAME(prefix, 5), \
		SGPIO_BANK_NAME(prefix, 6), SGPIO_BANK_NAME(prefix, 7), \
		SGPIO_BANK_NAME(prefix, 8), SGPIO_BANK_NAME(prefix, 9), \
		SGPIO_BANK_NAME(prefix, a), SGPIO_BANK_NAME(prefix, b), \
		SGPIO_BANK_NAME(prefix, c), SGPIO_BANK_NAME(prefix, d), \
		SGPIO_BANK_NAME(prefix, e), SGPIO_BANK_NAME(prefix, f), \
	}

#define SGPIO_BANK_LINE_NAMES(prefix, bank) \
	{ \
		SGPIO_BANK_LINE_NAME(prefix, bank, 0), \
		SGPIO_BANK_LINE_NAME(prefix, bank, 1), \
		SGPIO_BANK_LINE_NAME(prefix, bank, 2), \
		SGPIO_BANK_LINE_NAME(prefix, bank, 3), \
		SGPIO_BANK_LINE_NAME(prefix, bank, 4), \
		SGPIO_BANK_LINE_NAME(prefix, bank, 5), \
		SGPIO_BANK_LINE_NAME(prefix, bank, 6), \
		SGPIO_BANK_LINE_NAME(prefix, bank, 7), \
	}

#define SGPIO_GROUP_LINE_NAMES(prefix) \
	{ \
		SGPIO_BANK_LINE_NAMES(prefix, 0), SGPIO_BANK_LINE_NAMES(prefix, 1), \
		SGPIO_BANK_LINE_NAMES(prefix, 2), SGPIO_BANK_LINE_NAMES(prefix, 3), \
		SGPIO_BANK_LINE_NAMES(prefix, 4), SGPIO_BANK_LINE_NAMES(prefix, 5), \
		SGPIO_BANK_LINE_NAMES(prefix, 6), SGPIO_BANK_LINE_NAMES(prefix, 7), \
		SGPIO_BANK_LINE_NAMES(prefix, 8), SGPIO_BANK_LINE_NAMES(prefix, 9), \
		SGPIO_BANK_LINE_NAMES(prefix, a), SGPIO_BANK_LINE_NAMES(prefix, b), \
		SGPIO_BANK_LINE_NAMES(prefix, c), SGPIO_BANK_LINE_NAMES(prefix, d), \
		SGPIO_BANK_LINE_NAMES(prefix, e), SGPIO_BANK_LINE_NAMES(prefix, f), \
	}

struct sgpio_group_cfg {
	const char *dev_names[PLAT_SGPIO_GROUP_DEV_COUNT];
	const char *line_names[PLAT_SGPIO_GROUP_DEV_COUNT][SGPIO_PINS_PER_PORT];
};

static const struct sgpio_group_cfg sgpio_group_cfgs[] = {
	[SGPIO_GROUP0] = {
		.dev_names = SGPIO_GROUP_DEV_NAMES(sgpio0_),
		.line_names = SGPIO_GROUP_LINE_NAMES(sgpio0_),
	},
	[SGPIO_GROUP1] = {
		.dev_names = SGPIO_GROUP_DEV_NAMES(sgpio1_),
		.line_names = SGPIO_GROUP_LINE_NAMES(sgpio1_),
	},
};

static const char *sgpio_dev_names[PLAT_SGPIO_DEV_COUNT];

/* SGPIO group-local pin boundaries */
#define SGPIO_OUTPUT_END          63U
#define SGPIO_INPUT_START         64U
#define SGPIO_TOTAL_COUNT         SGPIO_PINS_PER_GROUP
/*
 * SGPIO model:
 *   - SGPIO0 and SGPIO1 are independent groups.
 *   - Each group uses local pin index 0~127.
 *   - OBMF channel-visible index space is also per-group 0~127.
 *
 * SGPIO1_BASE_INDEX/SGPIO_DUAL_TOTAL_COUNT are used only for local table
 * construction in this file.
 */
#define SGPIO1_BASE_INDEX         SGPIO_PINS_PER_GROUP
#define SGPIO_DUAL_TOTAL_COUNT    (SGPIO_PINS_PER_GROUP * 2U)

static SGPIO_CFG plat_sgpio_cfg_all[SGPIO_DUAL_TOTAL_COUNT];

static const struct sgpio_group_cfg *sgpio_get_group_cfg(uint8_t group)
{
	if (group > SGPIO_GROUP1) {
		return NULL;
	}

	return &sgpio_group_cfgs[group];
}

static void init_sgpio_dev_name_table(void)
{
	for (int i = 0; i < PLAT_SGPIO_GROUP_DEV_COUNT; i++) {
		sgpio_dev_names[i] = sgpio_group_cfgs[SGPIO_GROUP0].dev_names[i];
		sgpio_dev_names[i + PLAT_SGPIO_GROUP_DEV_COUNT] =
			sgpio_group_cfgs[SGPIO_GROUP1].dev_names[i];
	}
}

static void init_dual_sgpio_cfg(void)
{
	for (uint16_t idx = 0; idx < SGPIO_DUAL_TOTAL_COUNT; idx++) {
		uint8_t group = (idx >= SGPIO1_BASE_INDEX) ? SGPIO_GROUP1 : SGPIO_GROUP0;
		uint16_t local_pin = idx % SGPIO_PINS_PER_GROUP;
		bool is_input = (local_pin >= SGPIO_INPUT_START);

		plat_sgpio_cfg_all[idx].group = group;
		plat_sgpio_cfg_all[idx].number = local_pin;
		plat_sgpio_cfg_all[idx].is_init = ENABLE;
		plat_sgpio_cfg_all[idx].direction = is_input ? GPIO_INPUT : GPIO_OUTPUT;
		plat_sgpio_cfg_all[idx].status = GPIO_LOW;
		plat_sgpio_cfg_all[idx].int_type =
			is_input ? GPIO_INT_EDGE_BOTH : GPIO_INT_DISABLE;
		plat_sgpio_cfg_all[idx].int_cb = NULL;
		plat_sgpio_cfg_all[idx].name = NULL;
	}
}

static bool sgpio_hw_pin_to_obmf_index(uint8_t group, uint16_t hw_pin,
				       uint16_t *obmf_index);

static void sgpio_cb_handler(uint8_t sgpio_group, uint8_t gpio_val,
			     uint8_t gpio_index)
{
	LOG_DBG("SGPIO CB: g=%d pin=%d val=%d", sgpio_group, gpio_index, gpio_val);

	/* Notify OBMF (one pin at a time for clarity) */
	uint8_t gpio_value = (gpio_val > 0) ? OBMF_GPIO_VALUE_HIGH
						: OBMF_GPIO_VALUE_LOW;
	uint16_t obmf_index = 0;
	int ret;

	if (!sgpio_hw_pin_to_obmf_index(sgpio_group, gpio_index, &obmf_index)) {
		LOG_WRN("OBMF SGPIO notify skipped unmapped pin g=%d pin=%d",
			sgpio_group, gpio_index);
		return;
	}
	uint8_t obmf_channel = (sgpio_group == SGPIO_GROUP1) ?
		OBMF_ICP_SGPIO1_CHANNEL : OBMF_ICP_SGPIO0_CHANNEL;

	uint16_t idx_array[1] = { obmf_index };
	uint8_t val_array[1] = { gpio_value };
	ret = obmf_gpio_set_values(idx_array, val_array, 1, obmf_channel);
	if (ret != 0 && ret != -ENETDOWN) {
		LOG_ERR("obmf_gpio_set_values failed: %d", ret);
	}
}

/* =====================================================================
 * OBMF SGPIO channel (channel 6) - same pattern as obmf_gpio_config_init
 * in plat_vw_gpio.c
 * ===================================================================== */
#define SGPIO_OBMF_INVALID_INDEX UINT16_MAX

static uint16_t sgpio_obmf_to_hw_pin[2][OBMF_GPIO_MAX_ENTRIES];
static uint16_t sgpio_hw_to_obmf_index[2][SGPIO_TOTAL_COUNT];
static uint16_t sgpio_obmf_pin_count[2];

static bool sgpio_obmf_index_to_hw_pin(uint8_t group, uint16_t obmf_index,
					   uint16_t *hw_pin)
{
	if (group > SGPIO_GROUP1 || hw_pin == NULL) {
		return false;
	}

	if (obmf_index >= sgpio_obmf_pin_count[group] ||
	    obmf_index >= OBMF_GPIO_MAX_ENTRIES) {
		return false;
	}

	uint16_t mapped = sgpio_obmf_to_hw_pin[group][obmf_index];
	if (mapped == SGPIO_OBMF_INVALID_INDEX || mapped >= SGPIO_TOTAL_COUNT) {
		return false;
	}

	*hw_pin = mapped;
	return true;
}

static bool sgpio_hw_pin_to_obmf_index(uint8_t group, uint16_t hw_pin,
					   uint16_t *obmf_index)
{
	if (group > SGPIO_GROUP1 || obmf_index == NULL || hw_pin >= SGPIO_TOTAL_COUNT) {
		return false;
	}

	uint16_t mapped = sgpio_hw_to_obmf_index[group][hw_pin];
	if (mapped == SGPIO_OBMF_INVALID_INDEX ||
	    mapped >= sgpio_obmf_pin_count[group]) {
		return false;
	}

	*obmf_index = mapped;
	return true;
}

static uint8_t obmf_sgpio_get_cb(uint16_t index, void *ctx)
{
	uint8_t group = (uint8_t)(uintptr_t)ctx;
	uint16_t hw_pin = 0;
	if (!sgpio_obmf_index_to_hw_pin(group, index, &hw_pin)) {
		LOG_WRN("OBMF SGPIO get unavailable index g%d:%d", group, index);
		return OBMF_GPIO_VALUE_HIGH;
	}

	int val = sgpio_get_by_group(group, hw_pin);
	if (val < 0)
		return OBMF_GPIO_VALUE_HIGH;

	return val ? OBMF_GPIO_VALUE_HIGH : OBMF_GPIO_VALUE_LOW;
}

static void obmf_sgpio_set_cb(uint16_t index, uint8_t value, void *ctx)
{
	uint8_t group = (uint8_t)(uintptr_t)ctx;
	uint16_t hw_pin = 0;
	if (!sgpio_obmf_index_to_hw_pin(group, index, &hw_pin)) {
		LOG_ERR("OBMF SGPIO set unavailable index g%d:%d", group, index);
		return;
	}

	/* Only output SGPIOs (0~63) can be written by BMC */
	if (hw_pin > SGPIO_OUTPUT_END) {
		LOG_WRN("OBMF SGPIO %d is input, BMC SET rejected", index);
		return;
	}

	int ret = sgpio_set_by_group(group, hw_pin,
				     (value == OBMF_GPIO_VALUE_HIGH) ? 1 : 0);
	if (ret < 0) {
		LOG_ERR("OBMF SGPIO set failed g%d idx=%d hw=%d ret=%d",
			group, index, hw_pin, ret);
	}
}

struct obmf_sgpio_group_ctx {
	uint8_t channel;
	uint8_t group;
	const char *default_name_prefix;
	obmf_gpio_config_entry_t *entries;
	const char *const *dev_names;
	uint16_t gpio_count;
	bool enabled;
};

static bool sgpio_group_has_any_bank(const char *const dev_names[], int count)
{
	for (int i = 0; i < count; i++) {
		if (dev_names[i] != NULL) {
			return true;
		}
	}

	return false;
}

static uint16_t sgpio_group_available_pin_count(const char *const dev_names[],
						int bank_count)
{
	uint16_t count = 0;

	for (int i = 0; i < bank_count; i++) {
		if (dev_names[i] != NULL) {
			count += SGPIO_PINS_PER_PORT;
		}
	}

	return count;
}

static bool sgpio_group_pin_is_available(const char *const dev_names[],
					 uint16_t pin)
{
	if (pin >= SGPIO_TOTAL_COUNT) {
		return false;
	}

	uint8_t bank = (uint8_t)(pin / SGPIO_PINS_PER_PORT);
	return (bank < PLAT_SGPIO_GROUP_DEV_COUNT && dev_names[bank] != NULL);
}

static const char *sgpio_get_line_name_by_group(uint8_t group, uint16_t pin)
{
	const struct sgpio_group_cfg *cfg = sgpio_get_group_cfg(group);
	if (cfg == NULL) {
		return NULL;
	}

	if (pin >= SGPIO_TOTAL_COUNT) {
		return NULL;
	}

	uint8_t bank = (uint8_t)(pin / SGPIO_PINS_PER_PORT);
	uint8_t bit = (uint8_t)(pin % SGPIO_PINS_PER_PORT);
	if (bank >= PLAT_SGPIO_GROUP_DEV_COUNT || bit >= SGPIO_PINS_PER_PORT) {
		return NULL;
	}

	return cfg->line_names[bank][bit];
}

static void obmf_sgpio_fill_entry(obmf_gpio_config_entry_t *entry, uint16_t pin,
					 uint8_t group,
					 uint16_t exposed_index,
					 const char *default_name_prefix)
{
	int cur_val = sgpio_get_by_group(group, pin);
	uint8_t default_output = (cur_val > 0) ? OBMF_GPIO_DEFAULT_HIGH
					      : OBMF_GPIO_DEFAULT_LOW;
	const char *dts_line_name = sgpio_get_line_name_by_group(group, pin);

	entry->gpio_index = exposed_index;
	if (dts_line_name != NULL && dts_line_name[0] != '\0') {
		snprintf(entry->gpio_name, sizeof(entry->gpio_name), "%s",
			 dts_line_name);
	} else {
		snprintf(entry->gpio_name, sizeof(entry->gpio_name), "%s_%d",
			 default_name_prefix, exposed_index);
	}

	if (pin <= SGPIO_OUTPUT_END) {
		entry->gpio_direction = OBMF_GPIO_DIR_OUTPUT;
	} else {
		entry->gpio_direction = OBMF_GPIO_DIR_INPUT;
	}

	entry->gpio_default_output = default_output;
	entry->gpio_output_drive_cfg = OBMF_GPIO_DRIVE_PUSH_PULL;
	entry->gpio_output_persist = OBMF_GPIO_PERSIST_FALSE;
	entry->gpio_bias_pull_cfg = OBMF_GPIO_BIAS_NO_PULL;
	entry->gpio_bias_pull_strength = 0;
	entry->gpio_output_drive_strength = 0;
	entry->gpio_slew_rate = 0;
	entry->gpio_input_debounce = 0;
}

static void obmf_sgpio_init_group(const struct obmf_sgpio_group_ctx *group)
{
	if (!group->enabled) {
		return;
	}

	memset(group->entries, 0,
	       sizeof(obmf_gpio_config_entry_t) * OBMF_GPIO_MAX_ENTRIES);

	obmf_register_gpio_channel_get_cb(group->channel,
					  obmf_sgpio_get_cb,
					  (void *)(uintptr_t)group->group);
	obmf_register_gpio_channel_set_cb(group->channel,
					  obmf_sgpio_set_cb,
					  (void *)(uintptr_t)group->group);

	for (uint16_t i = 0; i < OBMF_GPIO_MAX_ENTRIES; i++) {
		sgpio_obmf_to_hw_pin[group->group][i] = SGPIO_OBMF_INVALID_INDEX;
	}
	for (uint16_t pin = 0; pin < SGPIO_TOTAL_COUNT; pin++) {
		sgpio_hw_to_obmf_index[group->group][pin] = SGPIO_OBMF_INVALID_INDEX;
	}
	sgpio_obmf_pin_count[group->group] = 0;

	uint16_t entry_idx = 0;
	for (uint16_t pin = 0; pin < SGPIO_TOTAL_COUNT; pin++) {
		if (!sgpio_group_pin_is_available(group->dev_names, pin)) {
			continue;
		}

		if (entry_idx >= group->gpio_count ||
		    entry_idx >= OBMF_GPIO_MAX_ENTRIES) {
			break;
		}

		sgpio_obmf_to_hw_pin[group->group][entry_idx] = pin;
		sgpio_hw_to_obmf_index[group->group][pin] = entry_idx;

		obmf_sgpio_fill_entry(&group->entries[entry_idx], pin,
				      group->group,
				      entry_idx,
				      group->default_name_prefix);
		entry_idx++;
	}

	sgpio_obmf_pin_count[group->group] = entry_idx;

	if (entry_idx != group->gpio_count) {
		LOG_WRN("OBMF SGPIO entry mismatch ch=%d expect=%d actual=%d",
			group->channel, group->gpio_count, entry_idx);
	}
}

static void obmf_sgpio_config_init(void)
{
	obmf_gpio_config_entry_t *sgpio1_entries = obmf_sgpio1_get_config_entries();
	obmf_gpio_config_entry_t *sgpio0_entries = obmf_sgpio0_get_config_entries();
	const struct sgpio_group_cfg *sgpio0_cfg = &sgpio_group_cfgs[SGPIO_GROUP0];
	const struct sgpio_group_cfg *sgpio1_cfg = &sgpio_group_cfgs[SGPIO_GROUP1];
	bool has_sgpio0 = sgpio_group_has_any_bank(sgpio0_cfg->dev_names,
					     PLAT_SGPIO_GROUP_DEV_COUNT);
	bool has_sgpio1 = sgpio_group_has_any_bank(sgpio1_cfg->dev_names,
					     PLAT_SGPIO_GROUP_DEV_COUNT);
	uint16_t sgpio0_count = sgpio_group_available_pin_count(
		sgpio0_cfg->dev_names, PLAT_SGPIO_GROUP_DEV_COUNT);
	uint16_t sgpio1_count = sgpio_group_available_pin_count(
		sgpio1_cfg->dev_names, PLAT_SGPIO_GROUP_DEV_COUNT);

	obmf_sgpio0_set_config_count(sgpio0_count);
	obmf_sgpio1_set_config_count(sgpio1_count);

	const struct obmf_sgpio_group_ctx groups[] = {
		{
			.channel = OBMF_ICP_SGPIO0_CHANNEL,
			.group = SGPIO_GROUP0,
			.default_name_prefix = "SGPIO0",
			.entries = sgpio0_entries,
			.dev_names = sgpio0_cfg->dev_names,
			.gpio_count = sgpio0_count,
			.enabled = has_sgpio0,
		},
		{
			.channel = OBMF_ICP_SGPIO1_CHANNEL,
			.group = SGPIO_GROUP1,
			.default_name_prefix = "SGPIO1",
			.entries = sgpio1_entries,
			.dev_names = sgpio1_cfg->dev_names,
			.gpio_count = sgpio1_count,
			.enabled = has_sgpio1,
		},
	};

	for (int i = 0; i < ARRAY_SIZE(groups); i++) {
		obmf_sgpio_init_group(&groups[i]);
	}
}

/*
 * pal_load_sgpio_config - entry point called from plat_init.c pal_post_init().
 * Same pattern as pal_load_vw_gpio_config():
 *   1. Call sgpio_group_init() to bind devices, check ready, configure pins, register callbacks
 *   2. Call obmf_sgpio_config_init() to register OBMF channel
 */
bool pal_load_sgpio_config(void)
{
	init_sgpio_dev_name_table();
	init_dual_sgpio_cfg();

	if (!sgpio_group_init(sgpio_dev_names, PLAT_SGPIO_DEV_COUNT,
			plat_sgpio_cfg_all, ARRAY_SIZE(plat_sgpio_cfg_all),
			sgpio_cb_handler)) {
		LOG_ERR("Failed to initialize SGPIO");
		return false;
	}

	obmf_sgpio_config_init();

	return true;
}

#endif /* CONFIG_SGPIO_NPCM4XX */
