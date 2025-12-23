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

#include "pldm_fru.h"
#include <string.h>
#include <stdlib.h>
#include <zephyr.h>
#include <logging/log.h>
#include "libutil.h"

LOG_MODULE_REGISTER(plat_pldm_fru);

#define FRU_RECORD_SET_ID 1
#define FRU_RECORD_TYPE_GENERAL 1

// Sample FRU Data
static uint8_t fru_record_table[] = {
	// FRU Record Header
	0x00, 0x01, // Record Set Identifier
	0x01, // Record Type (General)
	0x05, // Number of FRU fields
	0x01, // Encoding type (ASCII)

	// FRU Field 1: Manufacturer
	PLDM_FRU_FIELD_TYPE_MANUFAC, // Type
	0x07, // Length
	'N', 'u', 'v', 'o', 't', 'o', 'n', // Value

	// FRU Field 2: Product Name (Model)
	PLDM_FRU_FIELD_TYPE_MODEL, // Type
	0x7, // Length
	'O', 'p', 'e', 'n', 'B', 'I', 'C', // Value

	// FRU Field 3: Version
	PLDM_FRU_FIELD_TYPE_VERSION, // Type
	0x05, // Length
	'v', '1', '.', '0', '0', // Value

	// FRU Field 4: Serial Number
	PLDM_FRU_FIELD_TYPE_SN, // Type
	0x0D, // Length
	'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', // Value

	// FRU Field 5: IANA
	PLDM_FRU_FIELD_TYPE_IANA, // Type
	0x04, // Length
	0x1D, 0x10, 0x00, 0x00, // Value
};

uint8_t pldm_fru_get_record_table_metadata(void *mctp_p, uint8_t *buf, uint16_t len,
					   uint8_t inst, uint8_t *resp, uint16_t *resp_len,
					   void *ext_params)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_p, PLDM_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(buf, PLDM_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(resp, PLDM_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(resp_len, PLDM_ERROR);

	LOG_INF("pldm_fru_get_record_table_metadata");

	struct pldm_get_fru_record_table_metadata_resp *res =
		(struct pldm_get_fru_record_table_metadata_resp *)resp;

	res->completion_code = PLDM_SUCCESS;
	res->fru_data_major_version = 1;
	res->fru_data_minor_version = 0;
	res->fru_table_maximum_size = sizeof(fru_record_table);
	res->fru_table_length = sizeof(fru_record_table);
	res->total_record_set_identifiers = 1;
	res->total_table_records = 1;
	res->checksum = 0; // CRC32 implementation needed or 0 if not checked

	*resp_len = sizeof(struct pldm_get_fru_record_table_metadata_resp);
	return PLDM_SUCCESS;
}

uint8_t pldm_fru_get_record_table(void *mctp_p, uint8_t *buf, uint16_t len,
				  uint8_t inst, uint8_t *resp, uint16_t *resp_len,
				  void *ext_params)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_p, PLDM_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(buf, PLDM_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(resp, PLDM_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(resp_len, PLDM_ERROR);

	LOG_INF("pldm_fru_get_record_table");

	struct pldm_get_fru_record_table_req *req = (struct pldm_get_fru_record_table_req *)buf;
	struct pldm_get_fru_record_table_resp *res = (struct pldm_get_fru_record_table_resp *)resp;

	if (req->data_transfer_handle != 0) {
		res->completion_code = PLDM_ERROR_INVALID_DATA;
		*resp_len = 1;
		return PLDM_SUCCESS;
	}

	res->completion_code = PLDM_SUCCESS;
	res->next_data_transfer_handle = 0;
	res->transfer_flag = PLDM_START_AND_END;
	
	memcpy(res->fru_record_table_data, fru_record_table, sizeof(fru_record_table));

	*resp_len = sizeof(struct pldm_get_fru_record_table_resp) - 1 + sizeof(fru_record_table);
	return PLDM_SUCCESS;
}
