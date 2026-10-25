/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * OBMF Memory Device Channel (v1.0.0 RC1 spec section 4.12).
 *  - Consumer role (ch4 / OBMF_ICP_MEM_DEV_CHANNEL): we send byte-addressable
 *    read/write/erase requests to the BMC's exposed storage device (eSPI
 *    flash access path). Includes an LRU flash page cache for improved
 *    read performance.
 */

#include "obmf_internal.h"
#include <sys/slist.h>
#include <kernel.h>

LOG_MODULE_DECLARE(obmf);

/* =====================================================================
 * Memory Device Page Cache
 * Front of list = MRU (most recently used), back = LRU (least recently used).
 * On read miss  : evict LRU tail, fill from device, prepend to MRU front.
 * On write/erase: invalidate overlapping entries (reset, move to LRU tail).
 * ===================================================================== */

#define OBMF_MEM_DEV_PAGE_SIZE      960U
#define OBMF_MEM_DEV_PAGE_LIST_LEN  5
/* Matches the device_size advertised in mem_dev_channel_config (obmf.c) */
#define OBMF_MEM_DEV_MAX_ADDR       0x4000000UL

struct obmf_mem_dev_page {
	sys_snode_t node;
	uint32_t offset;  /* byte offset of this page in the device */
	uint16_t len;     /* valid bytes in buf (0 = unused)        */
	uint8_t *buf;     /* OBMF_MEM_DEV_PAGE_SIZE bytes            */
};

static struct obmf_mem_dev_page mem_dev_pages[OBMF_MEM_DEV_PAGE_LIST_LEN];
static sys_slist_t mem_dev_page_list;
static bool mem_dev_cache_inited;

/* =====================================================================
 * Pending read state - set by caller, consumed by handle_mem_dev_response.
 *   mem_dev_pending_page_read == true  : page cache path
 *   mem_dev_pending_read_buf   != NULL : direct-copy-to-caller path
 * Only one path is active at a time.
 * ===================================================================== */
static bool     mem_dev_pending_page_read;
static uint32_t mem_dev_pending_page_offset;

static uint8_t  *mem_dev_pending_read_buf;
static uint16_t  mem_dev_pending_read_max;

/* =====================================================================
 * Response synchronisation message queue (depth 1 - one outstanding req).
 * ===================================================================== */
struct obmf_mem_dev_rsp_entry {
	uint8_t  status;
	uint16_t data_len;
};

K_MSGQ_DEFINE(obmf_mem_dev_msgq, sizeof(struct obmf_mem_dev_rsp_entry), 1, 4);

/* =====================================================================
 * Page cache management
 * ===================================================================== */

void obmf_mem_dev_init(void)
{
	if (mem_dev_cache_inited) {
		return;
	}
	sys_slist_init(&mem_dev_page_list);
	for (int i = 0; i < OBMF_MEM_DEV_PAGE_LIST_LEN; i++) {
		struct obmf_mem_dev_page *page = &mem_dev_pages[i];

		page->buf = (uint8_t *)malloc(OBMF_MEM_DEV_PAGE_SIZE);
		if (!page->buf) {
			LOG_ERR("Mem Dev cache: failed to alloc page %d", i);
			continue;
		}
		page->offset = 0;
		page->len = 0;
		sys_slist_append(&mem_dev_page_list, &page->node);
	}
	mem_dev_cache_inited = true;
	LOG_INF("Mem Dev cache initialized (%d pages x %u bytes)",
		OBMF_MEM_DEV_PAGE_LIST_LEN, OBMF_MEM_DEV_PAGE_SIZE);
}

static void drop_mem_dev_cache(uint32_t start, uint32_t end)
{
	struct obmf_mem_dev_page *page, *next;

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&mem_dev_page_list, page,
					  next, node) {
		if (page->len == 0) {
			continue;
		}
		/* Skip non-overlapping pages */
		if (start >= (page->offset + page->len) || end < page->offset) {
			continue;
		}
		LOG_DBG("Mem Dev cache drop [0x%x : 0x%x]",
			page->offset, page->offset + page->len - 1);
		page->len = 0;
		page->offset = 0;
		/* Move to LRU tail so it gets reused first on next miss */
		sys_slist_find_and_remove(&mem_dev_page_list, &page->node);
		sys_slist_append(&mem_dev_page_list, &page->node);
	}
}

/* =====================================================================
 * Internal: build + send OBMF Memory Device request, then block for
 * response. Caller must configure mem_dev_pending_page_read /
 * mem_dev_pending_read_buf before calling.
 * ===================================================================== */
static int obmf_mem_dev_send_and_wait(uint8_t command, uint32_t addr,
				      uint16_t data_size,
				      const uint8_t *write_data,
				      uint16_t write_len)
{
	uint32_t total_len =
		OBMF_HEADER_SIZE + OBMF_MEM_DEV_REQ_HDR_SIZE + write_len;
	uint8_t *tx_buf = (uint8_t *)malloc(total_len);

	if (!tx_buf) {
		LOG_ERR("Mem Dev: failed to alloc tx buffer (%d bytes)", total_len);
		mem_dev_pending_read_buf = NULL;
		mem_dev_pending_page_read = false;
		return -1;
	}
	memset(tx_buf, 0, total_len);

	obmf_mem_dev_request_t *req =
		(obmf_mem_dev_request_t *)(tx_buf + OBMF_HEADER_SIZE);
	req->command = OBMF_MEM_DEV_GET_CMD(command);
	req->address = addr;
	req->data_size = data_size;

	if (write_data && write_len > 0) {
		memcpy(tx_buf + OBMF_HEADER_SIZE + OBMF_MEM_DEV_REQ_HDR_SIZE,
		       write_data, write_len);
	}

	/* A response that arrived after an earlier timeout is not ours */
	k_msgq_purge(&obmf_mem_dev_msgq);

	int rc = obmf_send_request(OBMF_ICP_MEM_DEV_CHANNEL, tx_buf, total_len);
	if (rc != 0) {
		mem_dev_pending_read_buf = NULL;
		mem_dev_pending_page_read = false;
		free(tx_buf);
		return rc;
	}

	/* Block until handle_mem_dev_response puts an entry in the msgq */
	struct obmf_mem_dev_rsp_entry entry;

	rc = k_msgq_get(&obmf_mem_dev_msgq, &entry, K_MSEC(10000));
	if (rc != 0) {
		LOG_HEXDUMP_ERR(tx_buf, total_len, "Mem Dev transfer timeout:");
		channel_tags[OBMF_ICP_MEM_DEV_CHANNEL].is_outstanding = false;
		mem_dev_pending_read_buf = NULL;
		mem_dev_pending_page_read = false;
		free(tx_buf);
		return -1;
	}

	free(tx_buf);
	return (entry.status == OBMF_ICP_SUCCESS) ? 0 : -1;
}

/* =====================================================================
 * handle_mem_dev_response - called from obmf.c RX thread when a Memory
 * Device response arrives. Copies read data into the page cache or
 * caller buffer (whichever pending state is active), then unblocks the
 * waiting caller via obmf_mem_dev_msgq. The payload buffer is valid only
 * for this call.
 * ===================================================================== */
void handle_mem_dev_response(obmf_icp_header_t *rsp_hdr,
			    uint8_t *payload, uint32_t payload_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(rsp_hdr);
	uint8_t status_hdr = OBMF_ICP_GET_STATUS(rsp_hdr);
	struct obmf_mem_dev_rsp_entry entry = {
		.status = OBMF_ICP_ERR_PERMANENT_ERROR,
		.data_len = 0
	};

	if (status_hdr != OBMF_ICP_SUCCESS) {
		LOG_ERR("Mem Dev response error: status=0x%x", status_hdr);
		channel_tags[channel].is_outstanding = false;
		mem_dev_pending_read_buf = NULL;
		mem_dev_pending_page_read = false;
		k_msgq_put(&obmf_mem_dev_msgq, &entry, K_NO_WAIT);
		return;
	}

	/* The status is in the common header: the payload is the read data */
	uint16_t read_len = (uint16_t)payload_len;
	const uint8_t *read_data = (read_len > 0) ? payload : NULL;

	entry.status = status_hdr;
	entry.data_len = 0;

	if (read_data && read_len > 0) {
		if (mem_dev_pending_page_read) {
			/* Page cache path: evict LRU, fill with new data */
			sys_snode_t *lru =
				sys_slist_peek_tail(&mem_dev_page_list);
			if (lru) {
				sys_slist_find_and_remove(&mem_dev_page_list,
							  lru);
				struct obmf_mem_dev_page *page =
					CONTAINER_OF(lru,
						     struct obmf_mem_dev_page,
						     node);
				uint16_t copy_len =
					MIN(read_len, OBMF_MEM_DEV_PAGE_SIZE);
				memcpy(page->buf, read_data, copy_len);
				page->offset = mem_dev_pending_page_offset;
				page->len = copy_len;
				/* Promote to MRU front */
				sys_slist_prepend(&mem_dev_page_list,
						  &page->node);
				entry.data_len = copy_len;
				LOG_DBG("Mem Dev cache fill [0x%x : 0x%x]",
					page->offset,
					page->offset + page->len - 1);
			}
			mem_dev_pending_page_read = false;
		} else if (mem_dev_pending_read_buf) {
			/* Direct transfer path: copy to caller's buffer */
			uint16_t copy_len =
				MIN(read_len, mem_dev_pending_read_max);
			memcpy(mem_dev_pending_read_buf, read_data, copy_len);
			entry.data_len = copy_len;
			mem_dev_pending_read_buf = NULL;
		}
	} else {
		mem_dev_pending_page_read = false;
		mem_dev_pending_read_buf = NULL;
	}

	channel_tags[channel].is_outstanding = false;
	if (k_msgq_put(&obmf_mem_dev_msgq, &entry, K_NO_WAIT) != 0) {
		LOG_ERR("Mem Dev: failed to enqueue response");
	}
}

/* =====================================================================
 * Public API
 * ===================================================================== */

int obmf_mem_dev_read(uint32_t addr, uint32_t len, uint8_t *buf)
{
	struct obmf_mem_dev_page *page, *next;
	int order;
	int retry = 10;
	bool req_sent = false;

	if (!buf || len == 0 || len > OBMF_MEM_DEV_PAGE_SIZE ||
	    addr >= OBMF_MEM_DEV_MAX_ADDR) {
		LOG_ERR("Mem Dev read: invalid args (addr=0x%x len=%d)",
			addr, len);
		return -1;
	}

copy_page:
	order = 0;
	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&mem_dev_page_list, page,
					  next, node) {
		if (page->len > 0 &&
		    addr >= page->offset &&
		    (addr + len) <= (page->offset + page->len)) {
			memcpy(buf, &page->buf[addr - page->offset], len);
			if (order > 0) {
				/* Promote cache hit to MRU front */
				sys_slist_find_and_remove(&mem_dev_page_list,
							  &page->node);
				sys_slist_prepend(&mem_dev_page_list,
						  &page->node);
			}
			LOG_DBG("Mem Dev cache hit: 0x%x+%d", addr, len);
			return 0;
		}
		order++;
	}

	if (!req_sent) {
		/* Align to page boundary */
		uint32_t page_addr =
			(addr / OBMF_MEM_DEV_PAGE_SIZE) *
			OBMF_MEM_DEV_PAGE_SIZE;
		/* If request crosses page boundary, start at addr itself */
		if (((addr % OBMF_MEM_DEV_PAGE_SIZE) + len) >
		    OBMF_MEM_DEV_PAGE_SIZE) {
			page_addr = addr;
		}
		uint32_t page_end = page_addr + OBMF_MEM_DEV_PAGE_SIZE;

		if (page_end > OBMF_MEM_DEV_MAX_ADDR) {
			page_end = OBMF_MEM_DEV_MAX_ADDR;
		}
		uint16_t fetch_len = (uint16_t)(page_end - page_addr);

		mem_dev_pending_page_read = true;
		mem_dev_pending_page_offset = page_addr;
		mem_dev_pending_read_buf = NULL;

		int rc;
		int retries = OBMF_REQUEST_MAX_RETRY;

		do {
			LOG_DBG("Mem Dev fetch: 0x%x+%u", page_addr, fetch_len);
			rc = obmf_mem_dev_send_and_wait(OBMF_MEM_DEV_CMD_READ,
							page_addr, fetch_len,
							NULL, 0);
			if (rc == -ENETDOWN) {
				return rc;
			}
			if (rc != 0) {
				LOG_ERR("Mem Dev fetch retry (%d left): addr=0x%x",
					retries - 1, page_addr);
				k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
			}
		} while (rc != 0 && --retries > 0);

		if (rc != 0) {
			LOG_ERR("Mem Dev fetch failed after %d retries: addr=0x%x",
				OBMF_REQUEST_MAX_RETRY, page_addr);
			return rc;
		}
		req_sent = true;
	}

	if (--retry > 0) {
		goto copy_page;
	}

	LOG_ERR("Mem Dev read failed after retries (addr=0x%x len=%d)",
		addr, len);
	return -1;
}

int obmf_mem_dev_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
	if (!data || len == 0 || addr >= OBMF_MEM_DEV_MAX_ADDR) {
		LOG_ERR("Mem Dev write: invalid args");
		return -1;
	}

	mem_dev_pending_read_buf = NULL;
	mem_dev_pending_page_read = false;

	int rc;
	int retries = OBMF_REQUEST_MAX_RETRY;

	do {
		rc = obmf_mem_dev_send_and_wait(OBMF_MEM_DEV_CMD_WRITE, addr,
						(uint16_t)len, data,
						(uint16_t)len);
		if (rc == -ENETDOWN) {
			break;
		}
		if (rc != 0) {
			LOG_ERR("Mem Dev write retry (%d left): addr=0x%x",
				retries - 1, addr);
			k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
		}
	} while (rc != 0 && --retries > 0);

	if (rc == 0) {
		drop_mem_dev_cache(addr, addr + len);
	} else if (rc != -ENETDOWN) {
		LOG_ERR("Mem Dev write failed after %d retries: addr=0x%x",
			OBMF_REQUEST_MAX_RETRY, addr);
	}
	return rc;
}

int obmf_mem_dev_erase(uint32_t addr, uint32_t erase_sz)
{
	if (erase_sz == 0 || (erase_sz % 1024) != 0 ||
	    (erase_sz / 1024) > UINT16_MAX ||
	    (addr % erase_sz) != 0 || addr >= OBMF_MEM_DEV_MAX_ADDR) {
		LOG_ERR("Mem Dev erase: unsupported size/alignment 0x%x @0x%x",
			erase_sz, addr);
		return -1;
	}

	mem_dev_pending_read_buf = NULL;
	mem_dev_pending_page_read = false;

	int rc;
	int retries = OBMF_REQUEST_MAX_RETRY;

	do {
		rc = obmf_mem_dev_send_and_wait(OBMF_MEM_DEV_CMD_ERASE, addr,
						(uint16_t)(erase_sz / 1024),
						NULL, 0);
		if (rc == -ENETDOWN) {
			break;
		}
		if (rc != 0) {
			LOG_ERR("Mem Dev erase retry (%d left): addr=0x%x",
				retries - 1, addr);
			k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
		}
	} while (rc != 0 && --retries > 0);

	if (rc == 0) {
		drop_mem_dev_cache(addr, addr + erase_sz);
	} else if (rc != -ENETDOWN) {
		LOG_ERR("Mem Dev erase failed after %d retries: addr=0x%x",
			OBMF_REQUEST_MAX_RETRY, addr);
	}
	return rc;
}

#ifdef CONFIG_SHELL
#include <shell/shell.h>

/*
 * Debug commands that issue Memory Device requests to the BMC, which are
 * otherwise only sent on behalf of eSPI flash cycles from the host:
 *   obmf_memdev read  <addr> <len>
 *   obmf_memdev write <addr> <byte> <len>   (fills len bytes with byte)
 *   obmf_memdev erase <addr> <bytes>
 */
static int cmd_memdev_read(const struct shell *sh, size_t argc, char **argv)
{
	static uint8_t buf[OBMF_MEM_DEV_PAGE_SIZE];
	uint32_t addr = strtoul(argv[1], NULL, 0);
	uint32_t len = strtoul(argv[2], NULL, 0);
	int rc;

	if (len == 0 || len > sizeof(buf)) {
		shell_error(sh, "len must be 1-%u", (unsigned int)sizeof(buf));
		return -EINVAL;
	}
	rc = obmf_mem_dev_read(addr, len, buf);
	if (rc) {
		shell_error(sh, "read 0x%x+%u failed: %d", addr, len, rc);
		return rc;
	}
	shell_hexdump(sh, buf, len);
	return 0;
}

static int cmd_memdev_write(const struct shell *sh, size_t argc, char **argv)
{
	static uint8_t buf[OBMF_MEM_DEV_PAGE_SIZE];
	uint32_t addr = strtoul(argv[1], NULL, 0);
	uint8_t val = strtoul(argv[2], NULL, 0);
	uint32_t len = strtoul(argv[3], NULL, 0);
	int rc;

	if (len == 0 || len > sizeof(buf)) {
		shell_error(sh, "len must be 1-%u", (unsigned int)sizeof(buf));
		return -EINVAL;
	}
	memset(buf, val, len);
	rc = obmf_mem_dev_write(addr, buf, len);
	if (rc) {
		shell_error(sh, "write 0x%x+%u failed: %d", addr, len, rc);
		return rc;
	}
	shell_print(sh, "wrote %u bytes of 0x%02x at 0x%x", len, val, addr);
	return 0;
}

static int cmd_memdev_erase(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t addr = strtoul(argv[1], NULL, 0);
	uint32_t size = strtoul(argv[2], NULL, 0);
	int rc = obmf_mem_dev_erase(addr, size);

	if (rc) {
		shell_error(sh, "erase 0x%x+0x%x failed: %d", addr, size, rc);
		return rc;
	}
	shell_print(sh, "erased 0x%x bytes at 0x%x", size, addr);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_memdev_cmds,
	SHELL_CMD_ARG(read, NULL, "read <addr> <len>", cmd_memdev_read, 3, 0),
	SHELL_CMD_ARG(write, NULL, "write <addr> <byte> <len>",
		      cmd_memdev_write, 4, 0),
	SHELL_CMD_ARG(erase, NULL, "erase <addr> <bytes>", cmd_memdev_erase,
		      3, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(obmf_memdev, &sub_memdev_cmds,
		   "OBMF Memory Device channel requests", NULL);
#endif /* CONFIG_SHELL */
