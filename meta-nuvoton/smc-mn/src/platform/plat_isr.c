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


#include <zephyr.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libutil.h>
#include <logging/log.h>
#include "hal_gpio.h"
#include "hal_vw_gpio.h"
#include "plat_isr.h"
#include "util_worker.h"
#include "sensor.h"
//#include "plat_hook.h"
#include "mctp.h"
#include "util_sys.h"

#ifdef ENABLE_PLDM
#include "pldm_oem.h"
#include "plat_mctp.h"
#endif

LOG_MODULE_REGISTER(plat_isr);

#ifdef ENABLE_PLDM
#define vw_gpio_isr_STACK_SIZE 1024
K_THREAD_STACK_DEFINE(vw_gpio_isr_thread, vw_gpio_isr_STACK_SIZE);
static struct k_thread vw_gpio_isr_thread_handler;
static struct k_sem get_isr_sem;
uint8_t val[2] = {0};


bool pldm_send_post_complete_to_bmc(uint8_t gpio_value, uint8_t gpio_index)
{
	pldm_msg msg = { 0 };
	uint8_t bmc_bus = 0;
	uint8_t bmc_interface = pal_get_bmc_interface();

	if (bmc_interface == BMC_INTERFACE_I3C) {
		bmc_bus = I3C_BUS_BMC;
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
		sizeof(struct pldm_oem_write_file_io_req) + (2 * sizeof(uint8_t)));

	if (ptr == NULL) {
		LOG_ERR("Memory allocation failed.");
		return false;
	}

	ptr->cmd_code = VW_GPIO;
	ptr->data_length = 2;
	ptr->messages[0] = gpio_value;
	ptr->messages[1] = gpio_index;
	msg.buf = (uint8_t *)ptr;
	msg.len = sizeof(struct pldm_oem_write_file_io_req) + 2;

	uint8_t resp_len = sizeof(struct pldm_oem_write_file_io_resp);
	uint8_t rbuf[resp_len];

	if (!mctp_pldm_read(find_mctp_by_bus(bmc_bus), &msg, rbuf, resp_len)) {
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


static void vw_gpio_isr(void *arvg0, void *arvg1, void *arvg2)
{
	while (1) {
		k_sem_take(&get_isr_sem, K_FOREVER);
		pldm_send_post_complete_to_bmc(val[0], val[1]);
		k_yield();
	}
}

void plat_isr_init()
{

	LOG_INF("plat_isr_init");

	k_sem_init(&get_isr_sem, 0, 1);

	k_thread_create(&vw_gpio_isr_thread_handler, vw_gpio_isr_thread,
			K_THREAD_STACK_SIZEOF(vw_gpio_isr_thread), vw_gpio_isr, NULL,
			NULL, NULL, CONFIG_MAIN_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&vw_gpio_isr_thread_handler, "vw_gpio_isr_thread");

	return;
}
#endif

void ISR_POST_COMPLETE(uint8_t gpio_value)
{
}

void ISR_VW_GPIO(uint8_t gpio_value, uint8_t gpio_index)
{
	val[0] = gpio_value;
	val[1] = gpio_index;
#ifdef ENABLE_PLDM	
	k_sem_give(&get_isr_sem);
#endif
}
