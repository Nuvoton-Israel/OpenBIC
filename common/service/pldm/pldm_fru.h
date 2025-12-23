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

#ifndef _PLDM_FRU_H
#define _PLDM_FRU_H

#ifdef __cplusplus
extern "C" {
#endif

#include "pldm.h"

#define PLDM_FRU_CMD_GET_FRU_RECORD_TABLE_METADATA 0x01
#define PLDM_FRU_CMD_GET_FRU_RECORD_TABLE 0x02
#define PLDM_FRU_CMD_GET_FRU_RECORD_BY_OPTION 0x03

/* GetFRURecordTableMetadata */
struct pldm_get_fru_record_table_metadata_resp {
	uint8_t completion_code;
	uint8_t fru_data_major_version;
	uint8_t fru_data_minor_version;
	uint32_t fru_table_maximum_size;
	uint32_t fru_table_length;
	uint16_t total_record_set_identifiers;
	uint16_t total_table_records;
	uint32_t checksum;
} __attribute__((packed));

enum PLDM_FRU_FIELD_TYPE {
	PLDM_FRU_FIELD_TYPE_CHASSIS = 0x01,
	PLDM_FRU_FIELD_TYPE_MODEL = 0x02,
	PLDM_FRU_FIELD_TYPE_PN = 0x03,
	PLDM_FRU_FIELD_TYPE_SN = 0x04,
	PLDM_FRU_FIELD_TYPE_MANUFAC = 0x05,
	PLDM_FRU_FIELD_TYPE_MANUFAC_DATE = 0x06,
	PLDM_FRU_FIELD_TYPE_VENDOR = 0x07,
	PLDM_FRU_FIELD_TYPE_NAME = 0x08,
	PLDM_FRU_FIELD_TYPE_SKU = 0x09,
	PLDM_FRU_FIELD_TYPE_VERSION = 0x0A,
	PLDM_FRU_FIELD_TYPE_ASSET_TAG = 0x0B,
	PLDM_FRU_FIELD_TYPE_DESC = 0x0C,
	PLDM_FRU_FIELD_TYPE_EC_LEVEL = 0x0D,
	PLDM_FRU_FIELD_TYPE_OTHER = 0x0E,
	PLDM_FRU_FIELD_TYPE_IANA = 0x0F,
};

/* GetFRURecordTable */
struct pldm_get_fru_record_table_req {
	uint32_t data_transfer_handle;
	uint8_t transfer_operation_flag;
} __attribute__((packed));

struct pldm_get_fru_record_table_resp {
	uint8_t completion_code;
	uint32_t next_data_transfer_handle;
	uint8_t transfer_flag;
	uint8_t fru_record_table_data[1];
} __attribute__((packed));

uint8_t pldm_fru_handler_query(uint8_t code, void **ret_fn);

#ifdef __cplusplus
}
#endif

#endif /* _PLDM_FRU_H */
