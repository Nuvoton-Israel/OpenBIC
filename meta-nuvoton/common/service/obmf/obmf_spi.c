/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * OBMF SPI Controller Channel.
 *  - Producer role (ch17 / OBMF_ICP_SPI_CHANNEL): the BMC sends us SPI
 *    flash requests targeting our local shd_dev (FIU0 CS1) flash device.
 *
 * The eSPI-facing Consumer role (formerly ch4) moved to the Memory Device
 * Channel; see obmf_mem_dev.c.
 */

#include "obmf_internal.h"
#include <kernel.h>
#include <device.h>
#include <drivers/flash.h>

LOG_MODULE_DECLARE(obmf);

static const struct device *shd_dev;

/* starts the low-priority flash erase / write worker thread */
static void spi_flash_worker_init(void);

void obmf_spi_producer_init(void)
{
#if DT_NODE_HAS_STATUS(DT_NODELABEL(spi_fiu0_cs1), okay)
	shd_dev = DEVICE_DT_GET(DT_NODELABEL(spi_fiu0_cs1));
#else
	shd_dev = NULL;
#endif
	if (!shd_dev) {
		LOG_WRN("SPI producer: spi_fiu0_cs1 node not available");
	} else if (!device_is_ready(shd_dev)) {
		LOG_WRN("SPI producer: spi_fiu0_cs1 not ready");
		shd_dev = NULL;
	}

	spi_flash_worker_init();
}

/* =====================================================================
 * SPI Channel - PRODUCER ROLE (OBMF_ICP_SPI_CHANNEL)
 * The BMC sends us SPI flash requests targeting our local shd_dev
 * (FIU0 CS1) flash device; we execute them and respond.
 * ===================================================================== */

/* JEDEC SPI NOR opcodes we recognise */
#define SPI_OPCODE_WREN              0x06
#define SPI_OPCODE_RDSR              0x05
#define SPI_OPCODE_READ              0x03
#define SPI_OPCODE_PAGE_PROG         0x02
#define SPI_OPCODE_READ_ID           0x9F  /* JEDEC READ_ID */
#define SPI_OPCODE_SECTOR_ERASE_4K   0x20
#define SPI_OPCODE_BLOCK_ERASE_32K   0x52
#define SPI_OPCODE_BLOCK_ERASE_64K   0xD8
#define SPI_OPCODE_ADDR_HDR_LEN      5  /* opcode + 4-byte address */

/*
 * Two-phase transaction cache.
 * Some BMC clients split a single logical SPI op into two OBMF requests:
 *   Phase 1: WRITE  (opcode [+ params], read_data_size=0)  -> executes op
 *   Phase 2: READ   (write_data_size=0, read_data_size=N)  -> fetch result
 * We cache the result of phase 1 here and serve it on phase 2.
 */
#define SPI_PENDING_RD_MAX  3  /* largest cached result = JEDEC ID (3 bytes) */
static uint8_t spi_pending_phased_buf[SPI_PENDING_RD_MAX];
static uint8_t spi_pending_phased_len;  /* 0 = no pending data */

/*
 * Erase/write worker thread.
 *
 * flash_erase() and flash_write() on shd_dev block for a while (NOR
 * sector/block erase time, or page-program time) while the driver polls
 * the flash status register. Calling either synchronously from
 * handle_spi_request() blocks the OBMF RX dispatch thread, which runs at
 * K_PRIO_PREEMPT(1) - higher priority than the UART bridge threads in
 * plat_uart_bridge.c (priority 8/9). A busy-poll or even a k_yield() inside
 * the wait cannot hand the CPU to those threads, since k_yield() only
 * rotates threads at the *same* priority.
 *
 * Offload both operations to a single dedicated worker thread whose
 * priority is numerically greater (=lower) than the UART bridge threads,
 * so it never competes with them. handle_spi_request() hands off the job
 * and blocks on a semaphore - a real sleep, not a spin - so while the
 * erase/write is in progress the scheduler is free to run any other ready
 * thread, including the UART bridge, regardless of its priority.
 */
#define SPI_FLASH_WORKER_THREAD_STACK_SIZE 1024
/* Must be > the UART bridge thread priorities (8/9 in plat_uart_bridge.c). */
#define SPI_FLASH_WORKER_THREAD_PRIORITY   10

K_THREAD_STACK_DEFINE(spi_flash_worker_stack, SPI_FLASH_WORKER_THREAD_STACK_SIZE);
static struct k_thread spi_flash_worker_thread_data;
static k_tid_t spi_flash_worker_thread_id;

K_SEM_DEFINE(spi_flash_op_req_sem, 0, 1);
K_SEM_DEFINE(spi_flash_op_done_sem, 0, 1);

enum spi_flash_op {
	SPI_FLASH_OP_ERASE,
	SPI_FLASH_OP_WRITE,
};

static struct {
	enum spi_flash_op op;
	uint32_t addr;
	const uint8_t *data;  /* write only; unused for erase */
	uint32_t len;         /* write length, or erase size  */
	int rc;
} spi_flash_job;

static void spi_flash_worker_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		k_sem_take(&spi_flash_op_req_sem, K_FOREVER);
		if (spi_flash_job.op == SPI_FLASH_OP_ERASE) {
			spi_flash_job.rc = flash_erase(shd_dev, spi_flash_job.addr,
					spi_flash_job.len);
		} else {
			spi_flash_job.rc = flash_write(shd_dev, spi_flash_job.addr,
					spi_flash_job.data,
					spi_flash_job.len);
		}
		k_sem_give(&spi_flash_op_done_sem);
	}
}

static void spi_flash_worker_init(void)
{
	if (spi_flash_worker_thread_id) {
		return;
	}
	spi_flash_worker_thread_id = k_thread_create(
			&spi_flash_worker_thread_data, spi_flash_worker_stack,
			K_THREAD_STACK_SIZEOF(spi_flash_worker_stack),
			spi_flash_worker_thread, NULL, NULL, NULL,
			SPI_FLASH_WORKER_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(spi_flash_worker_thread_id, "obmf_spi_flash");
}

/*
 * Block the calling (RX dispatch) thread until the worker finishes the
 * erase/write. Uses a semaphore (genuine sleep) rather than a spin/yield
 * loop, so the scheduler can run any other ready thread - at any priority -
 * while we wait.
 */
static int spi_flash_op_offload(enum spi_flash_op op, uint32_t addr,
		const uint8_t *data, uint32_t len)
{
	spi_flash_job.op = op;
	spi_flash_job.addr = addr;
	spi_flash_job.data = data;
	spi_flash_job.len = len;
	k_sem_give(&spi_flash_op_req_sem);
	k_sem_take(&spi_flash_op_done_sem, K_FOREVER);
	return spi_flash_job.rc;
}

static int spi_flash_erase_offload(uint32_t addr, uint32_t erase_sz)
{
	return spi_flash_op_offload(SPI_FLASH_OP_ERASE, addr, NULL, erase_sz);
}

static int spi_flash_write_offload(uint32_t addr, const uint8_t *data, uint16_t len)
{
	return spi_flash_op_offload(SPI_FLASH_OP_WRITE, addr, data, len);
}

static void build_spi_response(uint8_t status, uint8_t spi_cmd,
				const uint8_t *read_data, uint16_t read_len,
				uint8_t *rsp_buf, uint32_t *rsp_len)
{
	obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;

	OBMF_ICP_SET_CHANNEL(rsp_hdr, OBMF_ICP_SPI_CHANNEL);
	OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr, 1, status);
	OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_SPI_RSP_HDR_SIZE + read_len);

	obmf_spi_response_t *rsp = (obmf_spi_response_t *)(rsp_buf + OBMF_HEADER_SIZE);

	rsp->command = spi_cmd;
	rsp->read_data_size = read_len;

	if (read_len > 0 && read_data) {
		memcpy(rsp_buf + OBMF_HEADER_SIZE + OBMF_SPI_RSP_HDR_SIZE,
		       read_data, read_len);
	}

	*rsp_len = OBMF_HEADER_SIZE + OBMF_SPI_RSP_HDR_SIZE + read_len;
}

void handle_spi_request(obmf_icp_header_t *req_hdr,
			uint8_t *payload, uint32_t payload_len,
			uint8_t *rsp_buf, uint32_t *rsp_len)
{
	ARG_UNUSED(req_hdr);

	if (payload_len < OBMF_SPI_REQ_HDR_SIZE) {
		LOG_WRN("SPI producer: request too short (%u)", payload_len);
		build_spi_response(OBMF_ICP_STATUS_INVALID_CMD, 0, NULL, 0,
				   rsp_buf, rsp_len);
		return;
	}

	if (!shd_dev) {
		LOG_WRN("SPI producer: shd_dev not ready");
		build_spi_response(OBMF_ICP_STATUS_NOT_READY, 0, NULL, 0,
				   rsp_buf, rsp_len);
		return;
	}

	obmf_spi_request_t *req = (obmf_spi_request_t *)payload;
	uint8_t spi_cmd = OBMF_SPI_CMD_GET_COMMAND(req->command);
	uint16_t wr_sz = req->write_data_size;
	uint16_t rd_sz = req->read_data_size;
	const uint8_t *wr_data = payload + OBMF_SPI_REQ_HDR_SIZE;

	if (wr_sz > 0 && (OBMF_SPI_REQ_HDR_SIZE + wr_sz) > payload_len) {
		LOG_WRN("SPI producer: write_data_size %u exceeds payload", wr_sz);
		build_spi_response(OBMF_ICP_STATUS_INVALID_CMD, spi_cmd, NULL, 0,
				   rsp_buf, rsp_len);
		return;
	}

	if (wr_sz == 0) {
		/* Phase 2 of a two-phase transaction: serve cached result */
		if (spi_cmd == OBMF_SPI_CMD_READ && spi_pending_phased_len > 0) {
			uint16_t len = MIN(spi_pending_phased_len, rd_sz);

			build_spi_response(OBMF_ICP_STATUS_SUCCESS, spi_cmd,
					   spi_pending_phased_buf, len,
					   rsp_buf, rsp_len);
			spi_pending_phased_len = 0;
			return;
		}
		LOG_WRN("SPI producer: zero write_data_size");
		build_spi_response(OBMF_ICP_STATUS_INVALID_CMD, spi_cmd, NULL, 0,
				   rsp_buf, rsp_len);
		return;
	}

	uint8_t opcode = wr_data[0];
	int rc = 0;

	switch (opcode) {
	case SPI_OPCODE_WREN:
		/* Write-enable: acknowledge, no-op on this driver */
		break;
	case SPI_OPCODE_RDSR: {
		/* Report flash always ready (status register = 0x00) */
		uint8_t sr = 0x00;

		build_spi_response(OBMF_ICP_STATUS_SUCCESS, spi_cmd, &sr,
				   MIN(1, rd_sz), rsp_buf, rsp_len);
		return;
	}
	case SPI_OPCODE_READ_ID: {
		uint8_t id[3];

		rc = flash_read_jedec_id(shd_dev, id);
		if (rc != 0) {
			LOG_ERR("SPI producer: flash_read_jedec_id failed (%d)", rc);
			build_spi_response(OBMF_ICP_STATUS_PERMANENT_ERROR, spi_cmd,
					   NULL, 0, rsp_buf, rsp_len);
			return;
		}
		if (rd_sz == 0) {
			/* Phase 1: cache result for the follow-up READ request */
			spi_pending_phased_len = sizeof(id);
			memcpy(spi_pending_phased_buf, id, sizeof(id));
			build_spi_response(OBMF_ICP_STATUS_SUCCESS, spi_cmd, NULL, 0,
					   rsp_buf, rsp_len);
		} else {
			build_spi_response(OBMF_ICP_STATUS_SUCCESS, spi_cmd, id,
					   MIN(sizeof(id), rd_sz), rsp_buf, rsp_len);
		}
		return;
	}
	case SPI_OPCODE_READ: {
		if (wr_sz < SPI_OPCODE_ADDR_HDR_LEN) {
			LOG_WRN("SPI producer: READ missing address");
			build_spi_response(OBMF_ICP_STATUS_INVALID_CMD, spi_cmd,
					   NULL, 0, rsp_buf, rsp_len);
			return;
		}

		uint32_t addr = ((uint32_t)wr_data[1] << 24) |
				((uint32_t)wr_data[2] << 16) |
				((uint32_t)wr_data[3] << 8) |
				wr_data[4];
		uint8_t *rd_dst = rsp_buf + OBMF_HEADER_SIZE + OBMF_SPI_RSP_HDR_SIZE;

		rc = flash_read(shd_dev, addr, rd_dst, rd_sz);
		if (rc != 0) {
			LOG_ERR("SPI producer: flash_read failed (%d)", rc);
			build_spi_response(OBMF_ICP_STATUS_PERMANENT_ERROR, spi_cmd,
					   NULL, 0, rsp_buf, rsp_len);
			return;
		}

		obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;

		OBMF_ICP_SET_CHANNEL(rsp_hdr, OBMF_ICP_SPI_CHANNEL);
		OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr, 1, OBMF_ICP_STATUS_SUCCESS);
		OBMF_ICP_SET_SIZE(rsp_hdr, OBMF_SPI_RSP_HDR_SIZE + rd_sz);

		obmf_spi_response_t *rsp =
			(obmf_spi_response_t *)(rsp_buf + OBMF_HEADER_SIZE);
		rsp->command = spi_cmd;
		rsp->read_data_size = rd_sz;

		*rsp_len = OBMF_HEADER_SIZE + OBMF_SPI_RSP_HDR_SIZE + rd_sz;
		return;
	}
	case SPI_OPCODE_PAGE_PROG: {
		if (wr_sz < SPI_OPCODE_ADDR_HDR_LEN) {
			LOG_WRN("SPI producer: PAGE_PROG missing address");
			build_spi_response(OBMF_ICP_STATUS_INVALID_CMD, spi_cmd,
					   NULL, 0, rsp_buf, rsp_len);
			return;
		}

		uint32_t addr = ((uint32_t)wr_data[1] << 24) |
				((uint32_t)wr_data[2] << 16) |
				((uint32_t)wr_data[3] << 8) |
				wr_data[4];
		const uint8_t *prog_data = wr_data + SPI_OPCODE_ADDR_HDR_LEN;
		uint16_t prog_len = wr_sz - SPI_OPCODE_ADDR_HDR_LEN;

		rc = spi_flash_write_offload(addr, prog_data, prog_len);
		if (rc != 0) {
			LOG_ERR("SPI producer: flash_write failed (%d)", rc);
		}
		break;
	}
	case SPI_OPCODE_SECTOR_ERASE_4K:
	case SPI_OPCODE_BLOCK_ERASE_32K:
	case SPI_OPCODE_BLOCK_ERASE_64K: {
		if (wr_sz < SPI_OPCODE_ADDR_HDR_LEN) {
			LOG_WRN("SPI producer: ERASE missing address");
			build_spi_response(OBMF_ICP_STATUS_INVALID_CMD, spi_cmd,
					   NULL, 0, rsp_buf, rsp_len);
			return;
		}

		uint32_t addr = ((uint32_t)wr_data[1] << 24) |
				((uint32_t)wr_data[2] << 16) |
				((uint32_t)wr_data[3] << 8) |
				wr_data[4];
		uint32_t erase_sz = (opcode == SPI_OPCODE_SECTOR_ERASE_4K) ? 0x1000 :
				    (opcode == SPI_OPCODE_BLOCK_ERASE_32K) ? 0x8000 :
									      0x10000;

		rc = spi_flash_erase_offload(addr, erase_sz);
		if (rc != 0) {
			LOG_ERR("SPI producer: flash_erase failed (%d)", rc);
		}
		break;
	}
	default:
		LOG_WRN("SPI producer: unknown opcode 0x%02x", opcode);
		build_spi_response(OBMF_ICP_STATUS_INVALID_CMD, spi_cmd, NULL, 0,
				   rsp_buf, rsp_len);
		return;
	}

	uint8_t final_status = (rc == 0) ? OBMF_ICP_STATUS_SUCCESS :
					    OBMF_ICP_STATUS_PERMANENT_ERROR;
	build_spi_response(final_status, spi_cmd, NULL, 0, rsp_buf, rsp_len);
}
