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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <drivers/flash.h>
#include <drivers/spi_nor.h>
#include <plat_power_seq.h>
#include <soc.h>
#include "hal_gpio.h"
#include "util_spi.h"
#include "util_sys.h"
#include "plat_gpio.h"
#include "libutil.h"
#include <logging/log.h>

LOG_MODULE_REGISTER(plat_spi);

#define CXL_FLASH_TO_BIC	1
#define CXL_FLASH_TO_CXL	0

#define BIOS_UPDATE_MAX_OFFSET	0x4000000
#define BIC_UPDATE_MAX_OFFSET	0x50000
#define CXL_UPDATE_MAX_OFFSET	0x2000000

#define BIC_UPDATE_DEVICE	"spi_spim0_cs0"

#ifdef CONFIG_XIP
static struct npcm4xx_fw_write_bitmap bitmap = {0};
static uint32_t npcm4xx_fw_end = 0;
#endif

int pal_get_bios_flash_position(void)
{
	return DEVSPI_SPI1_CS1;
}

static bool switch_cxl_spi_mux(int gpio_status)
{
#if 0
	if (gpio_status != CXL_FLASH_TO_BIC && gpio_status != CXL_FLASH_TO_CXL) {
		LOG_ERR("Invalid argument");
		return false;
	}

	if (gpio_set(SPI_MASTER_SEL, gpio_status)) {
		LOG_ERR("Fail to switch the flash to %s",
			(gpio_status == CXL_FLASH_TO_BIC) ? "BIC" : "PIONEER");
		return false;
	}
#endif
	return true;
}

static bool control_flash_power(int power_state)
{
#if 0
	int control_mode = 0;

	switch (power_state) {
	case POWER_OFF:
		control_mode = DISABLE_POWER_MODE;
		break;
	case POWER_ON:
		control_mode = ENABLE_POWER_MODE;
		break;
	default:
		return false;
	}

	for (int retry = 3;; retry--) {
		if (gpio_get(P1V8_ASIC_PG_R) == power_state) {
			return true;
		}

		if (!retry) {
			break;
		}
		control_power_stage(control_mode, P1V8_ASIC_EN_R);
		k_msleep(CHKPWR_DELAY_MSEC);
	}

	LOG_ERR("Fail to %s the ASIC_1V8", (power_state == POWER_OFF) ? "disable" : "enable");
#endif
	return false;
}

uint8_t fw_update_cxl(uint32_t offset, uint16_t msg_len, uint8_t *msg_buf, bool sector_end)
{
	uint8_t ret = FWUPDATE_UPDATE_FAIL;

	if (offset > CXL_UPDATE_MAX_OFFSET) {
		return FWUPDATE_OVER_LENGTH;
	}

	// Enable the P1V8_ASCI to power the flash
	if (control_flash_power(POWER_ON) == false) {
		return FWUPDATE_UPDATE_FAIL;
	}

	// Set high to choose the BIC as the host
	if (switch_cxl_spi_mux(CXL_FLASH_TO_BIC) == false) {
		LOG_ERR("Fail to switch PIONEER flash to BIC");
		return FWUPDATE_UPDATE_FAIL;
	}

	ret = fw_update(offset, msg_len, msg_buf, sector_end, DEVSPI_SPI2_CS0);

	switch_cxl_spi_mux(CXL_FLASH_TO_CXL);

	return ret;
}

#ifdef CONFIG_XIP
static void set_update_bitmap(uint32_t op_addr, bool val)
{
	uint8_t bitmap_count, bitmap_index, bitmap_offset;
	uint32_t *bitmap_list = &bitmap.bitmap_lists[0];

	bitmap_count = (op_addr / SPI_NOR_SECTOR_SIZE);

	/* index in bitmap_lists */
	bitmap_index = bitmap_count / BITMAP_ARRAY_PER_SIZE;
	/* offset in BITMAP_ARRAY_PER_SIZE */
	bitmap_offset = bitmap_count % BITMAP_ARRAY_PER_SIZE;

	bitmap_list = bitmap_list + bitmap_index;

	if (val)
		*bitmap_list |= BIT(bitmap_offset);
	else
		*bitmap_list &= ~BIT(bitmap_offset);
}
#endif

static int do_erase_write_verify(const struct device *flash_device, uint32_t op_addr,
				 uint8_t *write_buf, uint8_t *read_back_buf, uint32_t erase_sz)
{
	uint32_t ret = 0;

	ret = flash_erase(flash_device, op_addr, erase_sz);
	if (ret != 0) {
		LOG_ERR("Failed to erase %u.", op_addr);
		goto end;
	}

	ret = flash_write(flash_device, op_addr, write_buf, erase_sz);
	if (ret != 0) {
		LOG_ERR("Failed to write %u.", op_addr);
		goto end;
	}

	ret = flash_read(flash_device, op_addr, read_back_buf, erase_sz);
	if (ret != 0) {
		LOG_ERR("Failed to read %u.", op_addr);
		goto end;
	}

	if (memcmp(write_buf, read_back_buf, erase_sz) != 0) {
		ret = -EINVAL;
		LOG_ERR("Failed to write flash at 0x%x.", op_addr);
		LOG_HEXDUMP_ERR(write_buf, erase_sz, "to be written:");
		LOG_HEXDUMP_ERR(read_back_buf, erase_sz, "readback:");
		goto end;
	}

end:
	return ret;
}

int do_update(const struct device *flash_device, off_t offset, uint8_t *buf, size_t len)
{
	int ret = 0;
	uint32_t flash_sz = flash_get_flash_size(flash_device);
	uint32_t sector_sz = flash_get_write_block_size(flash_device);
	uint32_t flash_offset = (uint32_t)offset;
	uint32_t remain, op_addr = 0, end_sector_addr;
	uint8_t *update_ptr = buf, *op_buf = NULL, *read_back_buf = NULL;
	bool update_it = false;
	uint32_t direct_addr = 0;

	if (flash_sz < flash_offset + len) {
		LOG_ERR("Update boundary exceeds flash size. (%u, %u, %u)", flash_sz, flash_offset,
			(unsigned int)len);
		ret = -EINVAL;
		goto end;
	}

	op_buf = (uint8_t *)malloc(sector_sz);
	if (op_buf == NULL) {
		LOG_ERR("Failed to allocate op_buf.");
		ret = -EINVAL;
		goto end;
	}

	read_back_buf = (uint8_t *)malloc(sector_sz);
	if (read_back_buf == NULL) {
		LOG_ERR("Failed to allocate read_back_buf.");
		ret = -EINVAL;
		goto end;
	}

	/* initial op_addr */
	op_addr = (flash_offset / sector_sz) * sector_sz;

	/* handle the start part which is not multiple of sector size */
	if (flash_offset % sector_sz != 0) {
		ret = flash_read(flash_device, op_addr, op_buf, sector_sz);
		if (ret != 0)
			goto end;

		remain = MIN(sector_sz - (flash_offset % sector_sz), len);
		memcpy((uint8_t *)op_buf + (flash_offset % sector_sz), update_ptr, remain);

		direct_addr = op_addr;
#ifdef CONFIG_XIP
		if (strcmp(flash_device->name, BIC_UPDATE_DEVICE) == 0){
			set_update_bitmap(direct_addr, true);
			direct_addr = direct_addr + BIC_UPDATE_MAX_OFFSET;
		}
#endif

		ret = do_erase_write_verify(flash_device, direct_addr, op_buf, read_back_buf,
					    sector_sz);
		if (ret != 0)
			goto end;

		op_addr += sector_sz;
		update_ptr += remain;
	}

	end_sector_addr = (flash_offset + len) / sector_sz * sector_sz;
	/* handle body */
	for (; op_addr < end_sector_addr;) {
		ret = flash_read(flash_device, op_addr, op_buf, sector_sz);
		if (ret != 0)
			goto end;

		/* reset update_it flag */
		update_it = false;

		if (memcmp(op_buf, update_ptr, sector_sz) != 0)
			update_it = true;

		if (update_it) {
			direct_addr = op_addr;
#ifdef CONFIG_XIP
			if (strcmp(flash_device->name, BIC_UPDATE_DEVICE) == 0) {
				set_update_bitmap(op_addr, true);
				direct_addr = direct_addr + BIC_UPDATE_MAX_OFFSET;
			}
#endif
			ret = do_erase_write_verify(flash_device, direct_addr, update_ptr,
						    read_back_buf, sector_sz);
			if (ret != 0)
				goto end;
		}

		op_addr += sector_sz;
		update_ptr += sector_sz;
	}

	/* handle remain part */
	if (end_sector_addr < flash_offset + len) {
		ret = flash_read(flash_device, op_addr, op_buf, sector_sz);
		if (ret != 0)
			goto end;

		remain = flash_offset + len - end_sector_addr;
		memcpy((uint8_t *)op_buf, update_ptr, remain);

		direct_addr = op_addr;
#ifdef CONFIG_XIP
		if (strcmp(flash_device->name, BIC_UPDATE_DEVICE) == 0) {
			set_update_bitmap(direct_addr, true);
			direct_addr = direct_addr + BIC_UPDATE_MAX_OFFSET;
		}
#endif
		ret = do_erase_write_verify(flash_device, direct_addr, op_buf, read_back_buf,
					    sector_sz);
		if (ret != 0)
			goto end;

		op_addr += remain;
	}

#ifdef CONFIG_XIP
	if (strcmp(flash_device->name, BIC_UPDATE_DEVICE) == 0) {
		if ((offset + len) > npcm4xx_fw_end)
			npcm4xx_fw_end = offset + len;

		LOG_HEXDUMP_INF(bitmap.bitmap_lists, sizeof(bitmap.bitmap_lists), "update bitmap:");

		/* update bic firmware always start from offset 0x0, so fw_end == size */
		npcm4xx_set_update_fw_spi_nor_address(BIC_UPDATE_MAX_OFFSET,
							npcm4xx_fw_end, &bitmap);
	}
#endif

end:
	SAFE_FREE(op_buf);
	SAFE_FREE(read_back_buf);

	return ret;
}
