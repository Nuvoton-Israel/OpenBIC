/*
 * Copyright (c) 2024 Nuvoton Technology Corporation.
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

#include <kernel.h>
#include <drivers/espi.h>
#include <drivers/espi_npcm4xx.h>
#include <drivers/flash.h>
#include <logging/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/byteorder.h>
#include "libutil.h"

#include "util_sys.h"
#include "pldm_oem.h"
#include "plat_mctp.h"

LOG_MODULE_REGISTER(hal_edaf_npcm);
#define safs_isr_STACK_SIZE 2048

K_THREAD_STACK_DEFINE(safs_isr_thread, safs_isr_STACK_SIZE);
static struct k_thread safs_isr_thread_handler;
static struct k_sem get_safs_isr_sem;

static const struct device *espi_dev;
static struct espi_callback flashrx_cb;
uint32_t prefetch_addr = 0;
uint32_t prefetch_len = 0;
uint8_t prefetch_buf[ESPI_FLASH_BUF_SIZE] = {0};
bool prefetch = false;

bool pldm_send_flashrx_to_bmc(struct espi_npcm4xx_ioc *ioc, struct espi_npcm4xx_ioc *resp_ioc)
{
	pldm_msg msg = { 0 };
	uint8_t bmc_bus = 0;
	uint8_t bmc_interface = pal_get_bmc_interface();

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

	struct pldm_oem_meta_write_file_req *ptr = (struct pldm_oem_meta_write_file_req *)malloc(
		sizeof(struct pldm_oem_meta_write_file_req) + (ioc->pkt_len * sizeof(uint8_t)));

	if (ptr == NULL) {
		LOG_ERR("Memory allocation failed.");
		return false;
	}

	ptr->file_handle = FLASH_ACCESS;
	ptr->length = ioc->pkt_len;

	memcpy(ptr->file_data, ioc->pkt, ioc->pkt_len);
	msg.buf = (uint8_t *)ptr;
	msg.len = sizeof(struct pldm_oem_meta_write_file_req) + ioc->pkt_len;

	uint16_t resp_len = sizeof(struct pldm_oem_meta_write_file_resp) +
				ESPI_FLASH_BUF_SIZE * sizeof(uint8_t);

	uint8_t rbuf[resp_len];

	if (!mctp_pldm_read(find_mctp_by_medium_type(msg.ext_params.type), &msg, rbuf, resp_len)) {
		SAFE_FREE(ptr);
		LOG_ERR("mctp_pldm_read fail");
		return false;
	}

	uint8_t *tmp = resp_ioc->pkt;
	struct pldm_oem_meta_write_file_resp *resp = (struct pldm_oem_meta_write_file_resp *)rbuf;

	if (resp->completion_code != PLDM_SUCCESS) {
		SAFE_FREE(ptr);
		LOG_ERR("Check reponse completion code fail %x", resp->completion_code);
		return false;
	}

	if (prefetch) {
		memcpy(prefetch_buf, resp->data, ESPI_FLASH_BUF_SIZE);
	 } else {
		*tmp = resp->pkgLen;
		tmp++;
		memcpy(tmp, &(resp->ctype), resp->pkgLen);
	}
	SAFE_FREE(ptr);
	return true;
}

static void safs_isr(void *arvg0, void *arvg1, void *arvg2)
{
	int rc;
	struct espi_npcm4xx_ioc ioc = {0};
	struct espi_npcm4xx_ioc resp_ioc = {0};
	struct espi_flash_rwe *rwe_pkt = (struct espi_flash_rwe *)ioc.pkt;
	uint32_t cyc, tag, len, addr;


	while (1) {
		k_sem_take(&get_safs_isr_sem, K_FOREVER);

		rc = espi_npcm4xx_flash_get_rx(espi_dev, &ioc, true);
		if (rc) {
			printk("failed to get flash packet, rc=%d\n", rc);
			continue;
		}

		addr = __bswap_32(rwe_pkt->addr_be);
		cyc = rwe_pkt->cyc;
		tag = rwe_pkt->tag;
		len = (rwe_pkt->len_h << 8) | (rwe_pkt->len_l & 0xff);

		if (prefetch && (rwe_pkt->cyc == ESPI_FLASH_READ_CYCLE_TYPE) && (prefetch_addr > addr))
			prefetch = false;

		if (prefetch && (rwe_pkt->cyc == ESPI_FLASH_READ_CYCLE_TYPE)  &&  ((addr + len) <= (prefetch_addr + prefetch_len)))
		{
			resp_ioc.pkt[0] = ESPI_FLASH_RESP_LEN + len;
			resp_ioc.pkt[1] = ESPI_FLASH_SUC_CMPLT_D_ONLY;
			resp_ioc.pkt[2] = tag << 4;
			resp_ioc.pkt[3] = len;
			memcpy(&resp_ioc.pkt[4], &prefetch_buf[addr - prefetch_addr], len);
		} else {
			prefetch_len = ESPI_FLASH_BUF_SIZE;
			if (rwe_pkt->cyc == ESPI_FLASH_READ_CYCLE_TYPE) {
				rwe_pkt->len_h = prefetch_len >> 8;
				rwe_pkt->len_l = prefetch_len & 0xff;
				prefetch_addr = addr;
				prefetch= true;
			}

			resp_ioc.pkt[0] = ESPI_FLASH_RESP_LEN;
			resp_ioc.pkt[1] = ESPI_FLASH_UNSUC_CMPLT;
			resp_ioc.pkt[2] = tag << 4;
			resp_ioc.pkt[3] = 0;

			if (!pldm_send_flashrx_to_bmc(&ioc, &resp_ioc)) {
				LOG_ERR("safs: pldm send failed");
			} else {
				resp_ioc.pkt[3] = len;
				if (rwe_pkt->cyc == ESPI_FLASH_READ_CYCLE_TYPE) {
					resp_ioc.pkt[0] = ESPI_FLASH_RESP_LEN + len;
					resp_ioc.pkt[1] = ESPI_FLASH_SUC_CMPLT_D_ONLY;
					memcpy(&resp_ioc.pkt[4], &prefetch_buf[addr - prefetch_addr], len);
				} else {
					resp_ioc.pkt[1] = ESPI_FLASH_SUC_CMPLT;
				}
			}
		}
		rc = espi_npcm4xx_flash_put_tx(espi_dev, &resp_ioc);
		if (rc) {
			printk("failed to tx flash packet, rc=%d\n", rc);
			continue;
		}
		k_yield();
	}
}

/* Handler for EDAF flash access request from eSPI FLASH channel */
static void flashrx_handler(const struct device *dev, struct espi_callback *cb, struct espi_event event)
{
	if (event.evt_type == ESPI_BUS_EVENT_FLASH_RECEIVED) {
		k_sem_give(&get_safs_isr_sem);
	}
}

bool edaf_npcm_init(void)
{
	espi_dev = device_get_binding("ESPI_0");
	if (!espi_dev) {
		LOG_ERR("failed to get espi device");
		return false;
	}

	espi_init_callback(&flashrx_cb, flashrx_handler, ESPI_BUS_EVENT_FLASH_RECEIVED);

	if (espi_add_callback(espi_dev, &flashrx_cb) < 0) {
		LOG_ERR("failed to add espi callback function");
		return false;
	}

	k_sem_init(&get_safs_isr_sem, 0, 1);

	k_thread_create(&safs_isr_thread_handler, safs_isr_thread,
			K_THREAD_STACK_SIZEOF(safs_isr_thread), safs_isr, NULL,
			NULL, NULL, CONFIG_MAIN_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&safs_isr_thread_handler, "tafs_isr_thread");

	LOG_INF("%s", __func__);

	return true;
}
