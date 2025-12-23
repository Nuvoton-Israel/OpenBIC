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

LOG_MODULE_REGISTER(pldm_fru);

#define PLDM_FRU_DATA_MAJOR_VERSION 1
#define PLDM_FRU_DATA_MINOR_VERSION 0

__weak uint8_t pldm_fru_get_record_table_metadata(void *mctp_p, uint8_t *buf, uint16_t len,
						  uint8_t inst, uint8_t *resp, uint16_t *resp_len,
						  void *ext_params)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_p, PLDM_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(buf, PLDM_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(resp, PLDM_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(resp_len, PLDM_ERROR);

	struct pldm_get_fru_record_table_metadata_resp *res =
		(struct pldm_get_fru_record_table_metadata_resp *)resp;

	// TODO: Implement actual FRU table metadata retrieval
	res->completion_code = PLDM_SUCCESS;
	res->fru_data_major_version = PLDM_FRU_DATA_MAJOR_VERSION;
	res->fru_data_minor_version = PLDM_FRU_DATA_MINOR_VERSION;
	res->fru_table_maximum_size = 0;
	res->fru_table_length = 0;
	res->total_record_set_identifiers = 0;
	res->total_table_records = 0;
	res->checksum = 0;

	*resp_len = sizeof(struct pldm_get_fru_record_table_metadata_resp);
	return PLDM_SUCCESS;
}

__weak uint8_t pldm_fru_get_record_table(void *mctp_p, uint8_t *buf, uint16_t len,
				       uint8_t inst, uint8_t *resp, uint16_t *resp_len,
				       void *ext_params)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_p, PLDM_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(buf, PLDM_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(resp, PLDM_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(resp_len, PLDM_ERROR);

	// struct pldm_get_fru_record_table_req *req = (struct pldm_get_fru_record_table_req *)buf;
	// ARG_UNUSED(req);
	struct pldm_get_fru_record_table_resp *res = (struct pldm_get_fru_record_table_resp *)resp;

	// TODO: Implement actual FRU table retrieval
	res->completion_code = PLDM_SUCCESS;
	res->next_data_transfer_handle = 0;
	res->transfer_flag = PLDM_START_AND_END;
	
	*resp_len = sizeof(struct pldm_get_fru_record_table_resp) - 1; // Subtract data array
	return PLDM_SUCCESS;
}

static pldm_cmd_handler pldm_fru_cmd_tbl[] = {
	{ PLDM_FRU_CMD_GET_FRU_RECORD_TABLE_METADATA, pldm_fru_get_record_table_metadata },
	{ PLDM_FRU_CMD_GET_FRU_RECORD_TABLE, pldm_fru_get_record_table },
};

uint8_t pldm_fru_handler_query(uint8_t code, void **ret_fn)
{
	if (!ret_fn)
		return PLDM_ERROR;

	*ret_fn = NULL;

	for (int i = 0; i < ARRAY_SIZE(pldm_fru_cmd_tbl); i++) {
		if (pldm_fru_cmd_tbl[i].cmd_code == code) {
			*ret_fn = (void *)pldm_fru_cmd_tbl[i].fn;
			return PLDM_SUCCESS;
		}
	}

	return PLDM_ERROR;
}
