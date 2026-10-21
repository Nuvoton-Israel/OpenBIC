#include "plat_vw_gpio.h"
#include "plat_isr.h"

#include <devicetree.h>
#include <logging/log.h>
#include <sys/util.h>
#include <stdio.h>

#ifdef CONFIG_USB_DEVICE_OBMF
#include "obmf.h"
#endif

LOG_MODULE_REGISTER(plat_vw_gpio);

/*
 * Bank enable mask for both VWGPSM (output) and VWGPMS (input).
 * bit0 -> bank0 ... bit15 -> bank15
 */
#define PLAT_VW_GPIO_BANK_MASK 0xFFFFU

#define VW_GPIO_BANK_NAME(mask, prefix, idx) \
	(((mask) & BIT(idx)) ? DEVICE_DT_NAME(DT_NODELABEL(prefix##idx)) : NULL)

#define VW_GPIO_BANK_LINE_NAME(prefix, bank, bit) \
	COND_CODE_1(DT_NODE_HAS_STATUS(DT_NODELABEL(prefix##bank), okay), \
		(COND_CODE_1(DT_NODE_HAS_PROP(DT_NODELABEL(prefix##bank), gpio_line_names), \
			(DT_PROP_BY_IDX(DT_NODELABEL(prefix##bank), gpio_line_names, bit)), \
			(NULL))), \
		(NULL))

#define VW_GPIO_GROUP_DEV_NAMES(prefix) \
	{ \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 0), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 1), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 2), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 3), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 4), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 5), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 6), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 7), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 8), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 9), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 10), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 11), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 12), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 13), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 14), \
		VW_GPIO_BANK_NAME(PLAT_VW_GPIO_BANK_MASK, prefix, 15), \
	}

#define VW_GPIO_BANK_LINE_NAMES(prefix, bank) \
	{ \
		VW_GPIO_BANK_LINE_NAME(prefix, bank, 0), \
		VW_GPIO_BANK_LINE_NAME(prefix, bank, 1), \
		VW_GPIO_BANK_LINE_NAME(prefix, bank, 2), \
		VW_GPIO_BANK_LINE_NAME(prefix, bank, 3), \
	}

/* Expand all 16 banks, each bank has 4 bit-level gpio-line-names. */
#define VW_GPIO_ALL_BANK_LINE_NAMES(prefix) \
	{ \
		VW_GPIO_BANK_LINE_NAMES(prefix, 0), VW_GPIO_BANK_LINE_NAMES(prefix, 1), \
		VW_GPIO_BANK_LINE_NAMES(prefix, 2), VW_GPIO_BANK_LINE_NAMES(prefix, 3), \
		VW_GPIO_BANK_LINE_NAMES(prefix, 4), VW_GPIO_BANK_LINE_NAMES(prefix, 5), \
		VW_GPIO_BANK_LINE_NAMES(prefix, 6), VW_GPIO_BANK_LINE_NAMES(prefix, 7), \
		VW_GPIO_BANK_LINE_NAMES(prefix, 8), VW_GPIO_BANK_LINE_NAMES(prefix, 9), \
		VW_GPIO_BANK_LINE_NAMES(prefix, 10), VW_GPIO_BANK_LINE_NAMES(prefix, 11), \
		VW_GPIO_BANK_LINE_NAMES(prefix, 12), VW_GPIO_BANK_LINE_NAMES(prefix, 13), \
		VW_GPIO_BANK_LINE_NAMES(prefix, 14), VW_GPIO_BANK_LINE_NAMES(prefix, 15), \
	}

struct vw_gpio_cb_override {
	uint8_t pin;
	vw_gpio_irq_cb_t int_cb;
};

struct vw_gpio_cfg {
	const char *vwgpsm_dev_names[VW_GPIO_NUM_BANKS];
	const char *vwgpms_dev_names[VW_GPIO_NUM_BANKS];
	/* Per-bank/per-bit line names from DTS gpio-line-names: [bank][bit]. */
	const char *vwgpsm_line_names[VW_GPIO_NUM_BANKS][VW_GPIO_PINS_PER_BANK];
	const char *vwgpms_line_names[VW_GPIO_NUM_BANKS][VW_GPIO_PINS_PER_BANK];
	struct vw_gpio_cb_override cb_overrides[2];
	uint8_t cb_override_count;
};

static const struct vw_gpio_cfg vw_gpio_cfg = {
	.vwgpsm_dev_names = VW_GPIO_GROUP_DEV_NAMES(vwgpsm),
	.vwgpms_dev_names = VW_GPIO_GROUP_DEV_NAMES(vwgpms),
	.vwgpsm_line_names = VW_GPIO_ALL_BANK_LINE_NAMES(vwgpsm),
	.vwgpms_line_names = VW_GPIO_ALL_BANK_LINE_NAMES(vwgpms),
	.cb_overrides = {
		{ .pin = 127, .int_cb = ISR_POST_COMPLETE },
	},
	.cb_override_count = 1,
};

static const char *vw_gpio_get_dts_line_name(uint16_t pin)
{
	if (pin >= VW_GPIO_COUNT) {
		return NULL;
	}

	uint16_t local_pin = pin;
	const char *const (*line_names)[VW_GPIO_PINS_PER_BANK] =
		vw_gpio_cfg.vwgpsm_line_names;

	if (pin >= VW_GPIO_INPUT_START) {
		local_pin = pin - VW_GPIO_INPUT_START;
		line_names = vw_gpio_cfg.vwgpms_line_names;
	}

	uint8_t bank = (uint8_t)(local_pin / VW_GPIO_PINS_PER_BANK);
	uint8_t bit = (uint8_t)(local_pin % VW_GPIO_PINS_PER_BANK);

	if (bank >= VW_GPIO_NUM_BANKS || bit >= VW_GPIO_PINS_PER_BANK) {
		return NULL;
	}

	return line_names[bank][bit];
}

#ifndef CONFIG_USB_DEVICE_OBMF
static vw_gpio_irq_cb_t vw_gpio_get_override_cb(uint8_t gpio_index)
{
	if (gpio_index >= VW_GPIO_COUNT) {
		return NULL;
	}

	for (int i = 0; i < vw_gpio_cfg.cb_override_count; i++) {
		if (vw_gpio_cfg.cb_overrides[i].pin == gpio_index) {
			return vw_gpio_cfg.cb_overrides[i].int_cb;
		}
	}

	return NULL;
}
#endif

/* =====================================================================
 * VW GPIO via Zephyr GPIO driver (nuvoton,npcm4xx-vw-gpio)
 *   Pin 0~63:   Output (VWGPSM, BIC to Host)
 *   Pin 64~127: Input  (VWGPMS, Host to BIC)
 * ===================================================================== */

/* VW GPIO counts are defined in hal_vw_gpio.h */

static void vw_gpio_cb_handler(uint8_t gpio_val, uint8_t gpio_index)
{
#ifdef CONFIG_USB_DEVICE_OBMF
	{
		uint16_t gpio_indices[1] = { gpio_index };
		uint8_t gpio_values[1] = { gpio_val > 0 ? OBMF_GPIO_VALUE_HIGH
						  : OBMF_GPIO_VALUE_LOW };
		int ret;

		ret = obmf_gpio_set_values(gpio_indices, gpio_values, 1, OBMF_ICP_VW_GPIO_CHANNEL);
		if (ret != 0 && ret != -ENETDOWN) {
			LOG_ERR("obmf_gpio_set_values failed: %d", ret);
		}
	}
#else
	vw_gpio_irq_cb_t cb = vw_gpio_get_override_cb(gpio_index);
	if (cb != NULL) {
		cb((uint8_t)gpio_val, gpio_index);
	}

#endif
}


#ifdef CONFIG_USB_DEVICE_OBMF
static uint8_t obmf_gpio_get_cb(uint16_t index, void *ctx)
{
	LOG_DBG("obmf_gpio_get_cb: index=%d", index);
	if (index >= VW_GPIO_COUNT) {
		return OBMF_GPIO_VALUE_HIGH;
	}

	int val = vw_gpio_get(index);
	if (val < 0) {
		return OBMF_GPIO_VALUE_HIGH;
	}

	return val ? OBMF_GPIO_VALUE_HIGH : OBMF_GPIO_VALUE_LOW;
}

static void obmf_gpio_set_cb(uint16_t index, uint8_t value, void *ctx)
{
	LOG_DBG("obmf_gpio_set_cb: index=%d, value=%d", index, value);
	if (index >= VW_GPIO_COUNT) {
		return;
	}

	/* Only output GPIOs (0~63) can be written by BMC */
	if (index >= VW_GPIO_INPUT_START) {
		LOG_WRN("OBMF GPIO %d is input, BMC SET rejected", index);
		return;
	}

	vw_gpio_set(index, (value == OBMF_GPIO_VALUE_HIGH) ? 1 : 0);
}

static void obmf_gpio_config_init(void)
{
	/* Register GPIO callbacks */
	obmf_register_gpio_get_cb(obmf_gpio_get_cb, NULL);
	obmf_register_gpio_set_cb(obmf_gpio_set_cb, NULL);

	/* Populate GPIO Configuration Array per OBMF-ICP v0.98 spec 9.2.1 */
	obmf_gpio_config_entry_t *entries = obmf_gpio_get_config_entries();

	for (uint16_t i = 0; i < VW_GPIO_COUNT; i++) {
		entries[i].gpio_index = i;

		/* Name priority: DTS gpio-line-names > default */
		const char *dts_name = vw_gpio_get_dts_line_name(i);
		if (dts_name != NULL && dts_name[0] != '\0') {
			snprintf(entries[i].gpio_name, sizeof(entries[i].gpio_name),
				 "%s", dts_name);
		} else {
			snprintf(entries[i].gpio_name, sizeof(entries[i].gpio_name),
				 "VW_GPIO_%d", i);
		}

		/* Read current pin state from hardware */
		int cur_val = vw_gpio_get(i);
		uint8_t default_output = (cur_val > 0) ? OBMF_GPIO_DEFAULT_HIGH
						       : OBMF_GPIO_DEFAULT_LOW;

		if (i <= VW_GPIO_OUTPUT_END) {
			/* 0~63: Output (BMC drives to host) */
			entries[i].gpio_direction = OBMF_GPIO_DIR_OUTPUT;
			entries[i].gpio_default_output = default_output;
			entries[i].gpio_output_drive_cfg = OBMF_GPIO_DRIVE_PUSH_PULL;
			entries[i].gpio_output_persist = OBMF_GPIO_PERSIST_FALSE;
		} else {
			/* 64~127: Input (host drives to BMC) */
			entries[i].gpio_direction = OBMF_GPIO_DIR_INPUT;
			entries[i].gpio_default_output = default_output;
			entries[i].gpio_output_drive_cfg = OBMF_GPIO_DRIVE_PUSH_PULL;
			entries[i].gpio_output_persist = OBMF_GPIO_PERSIST_FALSE;
		}

		/* Common defaults */
		entries[i].gpio_bias_pull_cfg = OBMF_GPIO_BIAS_NO_PULL;
		entries[i].gpio_bias_pull_strength = 0;    /* unspecified */
		entries[i].gpio_output_drive_strength = 0; /* unspecified */
		entries[i].gpio_slew_rate = 0;             /* unspecified */
		entries[i].gpio_input_debounce = 0;        /* unspecified */
	}
}
#endif /* CONFIG_USB_DEVICE_OBMF */

bool pal_load_vw_gpio_config(void)
{
	if (!vw_gpio_init(vw_gpio_cfg.vwgpsm_dev_names,
			  vw_gpio_cfg.vwgpms_dev_names,
			  VW_GPIO_NUM_BANKS,
			  vw_gpio_cb_handler)) {
		LOG_ERR("Failed to initialize VW GPIO");
		return false;
	}

#ifdef CONFIG_USB_DEVICE_OBMF
	obmf_gpio_config_init();
#endif

	return true;
}
