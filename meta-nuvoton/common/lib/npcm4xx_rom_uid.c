/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <string.h>
#include <errno.h>
#include <logging/log.h>
#include "npcm4xx_rom_uid.h"

LOG_MODULE_REGISTER(npcm4xx_rom_uid, LOG_LEVEL_INF);

/*
 * The boot ROM provides a 32-byte device ID in its handoff area at the top of
 * SRAM, outside the Zephyr image.
 */
#define ROM_EXCHG_INFO_BASE 0x100C7F00UL
#define ROM_EXCHG_VERSION_OFFSET 0x30
#define ROM_EXCHG_UID_OFFSET 0x38
#define ROM_EXPECTED_VERSION 0x171E

static uint8_t cached_uid[NPCM4XX_ROM_UID_LEN];
static bool cached_valid;
static bool cached_done;

static bool uid_is_blank(const uint8_t *uid)
{
	uint8_t all_00 = 0x00;
	uint8_t all_ff = 0xFF;

	for (int i = 0; i < NPCM4XX_ROM_UID_LEN; i++) {
		all_00 |= uid[i];
		all_ff &= uid[i];
	}

	return (all_00 == 0x00) || (all_ff == 0xFF);
}

int npcm4xx_rom_uid_get(uint8_t uid[NPCM4XX_ROM_UID_LEN])
{
	if (uid == NULL) {
		return -EINVAL;
	}

	/* Snapshot once so later RAM reuse cannot change the ID */
	if (!cached_done) {
		const volatile uint32_t *ver =
			(const volatile uint32_t *)(ROM_EXCHG_INFO_BASE + ROM_EXCHG_VERSION_OFFSET);
		const uint8_t *src = (const uint8_t *)(ROM_EXCHG_INFO_BASE + ROM_EXCHG_UID_OFFSET);

		cached_done = true;

		if (*ver != ROM_EXPECTED_VERSION) {
			LOG_WRN("Unsupported ROM version 0x%x, no unique ID", *ver);
		} else {
			memcpy(cached_uid, src, NPCM4XX_ROM_UID_LEN);
			if (uid_is_blank(cached_uid)) {
				LOG_WRN("ROM unique ID is blank");
			} else {
				cached_valid = true;
			}
		}
	}

	if (!cached_valid) {
		return -ENODATA;
	}

	memcpy(uid, cached_uid, NPCM4XX_ROM_UID_LEN);
	return 0;
}

int npcm4xx_rom_uuid_get(uint8_t uuid[NPCM4XX_ROM_UUID_LEN])
{
	uint8_t uid[NPCM4XX_ROM_UID_LEN];
	int ret;

	if (uuid == NULL) {
		return -EINVAL;
	}

	ret = npcm4xx_rom_uid_get(uid);
	if (ret != 0) {
		return ret;
	}

	memcpy(uuid, uid, NPCM4XX_ROM_UUID_LEN);
	uuid[6] = (uuid[6] & 0x0F) | 0x50; /* version 5 */
	uuid[8] = (uuid[8] & 0x3F) | 0x80; /* RFC 4122 variant */

	return 0;
}
