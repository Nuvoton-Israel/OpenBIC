/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NPCM4XX_ROM_UID_H
#define NPCM4XX_ROM_UID_H

#include <stdint.h>
#include <stdbool.h>

#define NPCM4XX_ROM_UID_LEN 32
#define NPCM4XX_ROM_UUID_LEN 16

/* Copy the 32-byte ROM device ID into uid. Returns 0 on success. */
int npcm4xx_rom_uid_get(uint8_t uid[NPCM4XX_ROM_UID_LEN]);

/*
 * Build an RFC 4122 name-based (version 5 layout) UUID from the ROM unique ID.
 * Returns 0 on success, negative if the ROM ID is unavailable.
 */
int npcm4xx_rom_uuid_get(uint8_t uuid[NPCM4XX_ROM_UUID_LEN]);

#endif
