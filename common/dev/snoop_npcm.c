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

#ifdef CONFIG_SNOOP_NPCM

#include <zephyr.h>
#include <device.h>
#include <stdlib.h>
#include <drivers/misc/npcm/snoop_npcm.h>
#include "libutil.h"
#include "snoop_npcm.h"
#include <logging/log.h>
#include "util_sys.h"
#include "pldm_oem.h"
#include "plat_mctp.h"

#define POST_CODE_SIZE 4

LOG_MODULE_REGISTER(snoop);

K_THREAD_STACK_DEFINE(process_postcode_thread, PROCESS_POSTCODE_STACK_SIZE);
static struct k_thread process_postcode_thread_handler;

const struct device *snoop_dev;
static uint32_t snoop_read_buffer[SNOOP_BUFFER_LEN];
static uint16_t snoop_read_len = 0, snoop_read_index = 0;
static bool proc_4byte_postcode_ok = false;
static struct k_sem get_postcode_sem;
static uint8_t bmc_interface = 0;

uint16_t copy_snoop_read_buffer(uint16_t start, uint16_t length, uint8_t *buffer, uint16_t buffer_len)
{
	if ((buffer == NULL) || (buffer_len < (length * 4))) {
		return 0;
	}

	uint16_t current_index, i = 0;
	uint16_t current_read_len = snoop_read_len;
	uint16_t current_read_index = snoop_read_index;
	if (start < current_read_index) {
		current_index = current_read_index - start - 1;
	} else {
		current_index = current_read_index + SNOOP_BUFFER_LEN - start - 1;
	}

	for (; (i < length) && ((i + start) < current_read_len); i++) {
		buffer[4 * i] = snoop_read_buffer[current_index] & 0xFF;
		buffer[(4 * i) + 1] = (snoop_read_buffer[current_index] >> 8) & 0xFF;
		buffer[(4 * i) + 2] = (snoop_read_buffer[current_index] >> 16) & 0xFF;
		buffer[(4 * i) + 3] = (snoop_read_buffer[current_index] >> 24) & 0xFF;

		if (current_index == 0) {
			current_index = SNOOP_BUFFER_LEN - 1;
		} else {
			current_index--;
		}
	}
	return 4 * i;
}

bool pldm_send_post_code_to_bmc(uint16_t send_index)
{
	pldm_msg msg = { 0 };
	uint8_t bmc_bus = 0;

	if (bmc_interface == BMC_INTERFACE_I3C) {
		bmc_bus = I3C_BUS_TARGET_TO_BMC;
		msg.ext_params.type = MCTP_MEDIUM_TYPE_TARGET_I3C;
		msg.ext_params.i3c_ext_params.addr = I3C_STATIC_ADDR_BMC;
		msg.ext_params.ep = MCTP_EID_BMC_I3C;
	} else if (bmc_interface == BMC_INTERFACE_USB) {
		bmc_bus = 0;
		msg.ext_params.type = MCTP_MEDIUM_TYPE_USB;
	}

	msg.hdr.pldm_type = PLDM_TYPE_OEM;
	msg.hdr.cmd = PLDM_OEM_WRITE_FILE_IO;
	msg.hdr.rq = 1;

	struct pldm_oem_write_file_io_req *ptr = (struct pldm_oem_write_file_io_req *)malloc(
		sizeof(struct pldm_oem_write_file_io_req) + (POST_CODE_SIZE * sizeof(uint8_t)));

	if (ptr == NULL) {
		LOG_ERR("Memory allocation failed.");
		return false;
	}

	ptr->cmd_code = POST_CODE;
	ptr->data_length = POST_CODE_SIZE;
	ptr->messages[0] = snoop_read_buffer[send_index] & 0xFF;
	ptr->messages[1] = (snoop_read_buffer[send_index] >> 8) & 0xFF;
	ptr->messages[2] = (snoop_read_buffer[send_index] >> 16) & 0xFF;
	ptr->messages[3] = (snoop_read_buffer[send_index] >> 24) & 0xFF;

	msg.buf = (uint8_t *)ptr;
	msg.len = sizeof(struct pldm_oem_write_file_io_req) + POST_CODE_SIZE;

	uint8_t resp_len = sizeof(struct pldm_oem_write_file_io_resp);
	uint8_t rbuf[resp_len];

	if (!mctp_pldm_read(find_mctp_by_medium_type(msg.ext_params.type), &msg, rbuf, resp_len)) {
		SAFE_FREE(ptr);
		LOG_ERR("mctp_pldm_read fail");
		return false;
	}

	struct pldm_oem_write_file_io_resp *resp = (struct pldm_oem_write_file_io_resp *)rbuf;
	if (resp->completion_code != PLDM_SUCCESS) {
		LOG_ERR("Check reponse completion code fail %x", resp->completion_code);
	}

	SAFE_FREE(ptr);
	return true;
}

static void process_postcode(void *arvg0, void *arvg1, void *arvg2)
{
	uint16_t send_index = 0;
	while (1) {
		k_sem_take(&get_postcode_sem, K_FOREVER);
		uint16_t current_read_index = snoop_read_index;
		for (; send_index != current_read_index;
		     send_index = (send_index + 1) % SNOOP_BUFFER_LEN) {
			pldm_send_post_code_to_bmc(send_index);

			k_yield();
		}
	}
}

void snoop_rx_callback(const uint8_t *data, int len)
{
	int i = 0;

	LOG_DBG("snoop_rx_callback len %d \r\n", len);

	if (len == 4)
		proc_4byte_postcode_ok = true;
	else
		proc_4byte_postcode_ok = false;

	do {
		if (proc_4byte_postcode_ok) {
			snoop_read_buffer[snoop_read_index] = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);  
			i+=4;
		} else {
			snoop_read_buffer[snoop_read_index] = data[i];
			i++;
		}

		snoop_read_index++;
		if (snoop_read_index == SNOOP_BUFFER_LEN)
			snoop_read_index = 0;

	} while (i != len);

	k_sem_give(&get_postcode_sem);
}

void snoop_init()
{

	LOG_INF("snoop_init");

	snoop_dev = device_get_binding(DT_LABEL(DT_NODELABEL(snoop)));
	if (!snoop_dev) {
		LOG_ERR("No snoop device found.");
		return;
	}
	bmc_interface = pal_get_bmc_interface();

	if (snoop_npcm_register_rx_callback(snoop_dev, snoop_rx_callback)) {
		LOG_ERR("Cannot register SNOOP RX callback.");
	}

	k_sem_init(&get_postcode_sem, 0, 1);

	k_thread_create(&process_postcode_thread_handler, process_postcode_thread,
			K_THREAD_STACK_SIZEOF(process_postcode_thread), process_postcode, NULL,
			NULL, NULL, CONFIG_MAIN_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&process_postcode_thread_handler, "process_postcode_thread");

	return;
}

void reset_snoop_buffer()
{
	snoop_read_len = 0;
	return;
}

bool get_4byte_postcode_ok()
{
	return proc_4byte_postcode_ok;
}

void reset_4byte_postcode_ok()
{
	proc_4byte_postcode_ok = false;
}

#endif
