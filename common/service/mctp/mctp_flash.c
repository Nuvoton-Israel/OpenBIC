#include "mctp.h"
#include <usb/class/usb_mctp.h>
#include <logging/log.h>
#include <stdlib.h>
#include <zephyr.h>
#include <shell/shell.h>
#include "libutil.h"
#include "plat_def.h"
#include "plat_mctp.h"

#ifdef ENABLE_EDAF_OVER_MCTP
LOG_MODULE_REGISTER(mctp_flash, LOG_LEVEL_DBG);

/* MCTP flash command */
#define MCTP_FLASH_READ		0
#define MCTP_FLASH_WRITE	1
#define MCTP_FLASH_ERASE	2
#define MCTP_FLASH_CMD_MASK	0x7F
#define MCTP_FLASH_CMD_RESP	0x80

/* MCTP flash return code */
#define MCTP_FLASH_RC_OK	0
#define MCTP_FLASH_RC_FAIL	1

#define PAGE_SIZE		960
#define MAX_FLASH_ADDR		0x4000000

struct mctp_flash_msg {
	uint8_t msg_type : 7;
	uint8_t ic : 1;
	uint8_t cmd;
	uint32_t offset;
	uint16_t len;
	uint8_t data[];
} __attribute__((packed));

struct mctp_flash_comp {
	uint8_t cmd;
	uint8_t status;
};

char comp_buffer[2];
void mctp_flash_prefetch(void *mctp_p, uint32_t flash_addr)
{
	mctp *mctp_inst = (mctp *)mctp_p;
	struct flash_page *page, *next;
	struct mctp_flash_msg req = { 0 };
	mctp_ext_params ext_params;
	uint32_t prefetch_addr = ((flash_addr / PAGE_SIZE) + 1) * PAGE_SIZE;
	struct mctp_flash_comp comp = { 0 };

	if (mctp_ringbuf_space() < (MCTP_USB_BTU * 2)) {
		LOG_DBG("Ringbuf no space\n");
		return;
	}
	if (prefetch_addr >= MAX_FLASH_ADDR)
		return;

	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&mctp_inst->flash_page_list,
				  page, next, node) {
		if ((prefetch_addr >= page->offset) && prefetch_addr < (page->offset + page->len)) {
			LOG_DBG("prefetch addr already in page %d", page->index);
			return;
		}
	}

	ext_params.tag_owner = 1;
	ext_params.type = MCTP_MEDIUM_TYPE_USB;
	ext_params.ep = 0;
	req.msg_type = MCTP_MSG_TYPE_FLASH;
	req.cmd = MCTP_FLASH_READ;
	req.len = PAGE_SIZE;
	req.offset = prefetch_addr;
	LOG_DBG("Flash Prefetch {0x%x, %zd}", prefetch_addr, PAGE_SIZE);
	mctp_send_msg(mctp_inst, (uint8_t *)&req, sizeof(req), ext_params);
	k_msgq_get(mctp_inst->mctp_flash_msgq, &comp, K_FOREVER);
}

static void drop_flash_cache(mctp *mctp_inst, uint32_t start, uint32_t end)
{
	struct flash_page *page, *next;

	/* Drop the matched cache */
	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&mctp_inst->flash_page_list,
					  page, next, node) {
		if (start >= (page->offset + page->len))
			continue;
		if (end < page->offset)
			continue;

		LOG_DBG("Drop cache [0x%x : 0x%x]", page->offset, page->offset + page->len - 1);
		page->offset = 0;
		page->len = 0;
		sys_slist_find_and_remove(&mctp_inst->flash_page_list, &page->node);
		sys_slist_append(&mctp_inst->flash_page_list, &page->node);
	}
}

int mctp_flash_read(void *mctp_p, uint8_t *buf, uint32_t offset, uint16_t len)
{
	mctp *mctp_inst = (mctp *)mctp_p;
	struct flash_page *page, *next;
	struct mctp_flash_msg req = { 0 };
	mctp_ext_params ext_params;
	bool req_sent = false;
	uint32_t flash_addr;
	int retry = 10;
	int order = 0;
	struct mctp_flash_comp comp = { 0 };

	if (len > PAGE_SIZE) {
		LOG_ERR("flash read size(%d) > PAGE_SIZE(%d)", len, PAGE_SIZE);
		return 0;
	}

	LOG_DBG("Read flash: offset 0x%x, len %d", offset, len);
copy_page:
	/* Lookup the page cache */
	SYS_SLIST_FOR_EACH_CONTAINER_SAFE(&mctp_inst->flash_page_list,
					  page, next, node) {
		if ((offset >= page->offset) && (offset + len) <= (page->offset + page->len)) {
			memcpy(buf, &page->buf[offset - page->offset], len);
			if (order) {
				sys_slist_find_and_remove(&mctp_inst->flash_page_list, &page->node);
				sys_slist_prepend(&mctp_inst->flash_page_list, &page->node);
			}
			return MCTP_FLASH_RC_OK;
		}
		order++;
	}
	if (!req_sent) {
		if (((offset % PAGE_SIZE) + len) > PAGE_SIZE)
			flash_addr = offset;
		else
			flash_addr = (offset / PAGE_SIZE) * PAGE_SIZE;
		ext_params.tag_owner = 1;
		ext_params.type = MCTP_MEDIUM_TYPE_USB;
		ext_params.ep = 0;
		req.msg_type = MCTP_MSG_TYPE_FLASH;
		req.cmd = MCTP_FLASH_READ;
		req.len = PAGE_SIZE;
		req.offset = flash_addr;
		LOG_DBG("Flash Read {0x%x, %zd}", flash_addr, PAGE_SIZE);
		mctp_send_msg(mctp_inst, (uint8_t *)&req, sizeof(req), ext_params);
		req_sent = true;
	}

	k_msgq_get(mctp_inst->mctp_flash_msgq, &comp, K_FOREVER);
	if (retry--)
		goto copy_page;

	LOG_ERR("Fail to read flash page offset 0x%x, len %d", offset, len);
	return MCTP_FLASH_RC_FAIL;
}

int mctp_flash_write(void *mctp_p, uint8_t *buf, uint32_t offset, uint16_t len)
{
	mctp *mctp_inst = (mctp *)mctp_p;
	mctp_ext_params ext_params = { 0 };
	struct mctp_flash_comp comp = { 0 };
	uint8_t *req_buf;
	struct mctp_flash_msg *req;

	req_buf = malloc(sizeof(struct mctp_flash_msg) + len);
	if (!req_buf)
		return MCTP_ERROR;
	req = (struct mctp_flash_msg *)req_buf;

	LOG_DBG("Write flash: offset 0x%x, len %d", offset, len);
	ext_params.tag_owner = 1;
	ext_params.type = MCTP_MEDIUM_TYPE_USB;
	ext_params.ep = 0;
	req->msg_type = MCTP_MSG_TYPE_FLASH;
	req->cmd = MCTP_FLASH_WRITE;
	req->len = len;
	req->offset = offset;
	memcpy(req->data, buf, len);
	mctp_send_msg(mctp_inst, req_buf, sizeof(struct mctp_flash_msg) + len, ext_params);
	SAFE_FREE(req_buf);

	drop_flash_cache(mctp_inst, offset, offset + len);
	/* Wait for complete */
	k_msgq_get(mctp_inst->mctp_flash_msgq, &comp, K_FOREVER);

	return MCTP_FLASH_RC_OK;
}

int mctp_flash_erase(void *mctp_p, uint32_t offset, uint16_t len)
{
	mctp *mctp_inst = (mctp *)mctp_p;
	struct mctp_flash_msg req = { 0 };
	mctp_ext_params ext_params = { 0 };
	struct mctp_flash_comp comp = { 0 };
	int erase_sz;

	LOG_DBG("Erase flash: offset 0x%x, len %d", offset, len);

	if (len > 3) {
		LOG_ERR("invalid erase len %d\n", len);
		return MCTP_FLASH_RC_FAIL;
	}
	if (len)
		erase_sz = 4096 * 8 * len;
	else
		erase_sz = 4096;

	ext_params.tag_owner = 1;
	ext_params.type = MCTP_MEDIUM_TYPE_USB;
	ext_params.ep = 0;
	req.msg_type = MCTP_MSG_TYPE_FLASH;
	req.cmd = MCTP_FLASH_ERASE;
	req.len = len;
	req.offset = offset;
	mctp_send_msg(mctp_inst, (uint8_t *)&req, sizeof(req), ext_params);

	drop_flash_cache(mctp_inst, offset, offset + erase_sz);
	/* Wait for complete */
	k_msgq_get(mctp_inst->mctp_flash_msgq, &comp, K_FOREVER);

	return MCTP_FLASH_RC_OK;
}

int mctp_flash_msg_handler(void *mctp_p, uint8_t *buf, uint32_t len, mctp_ext_params ext_params)
{
	struct mctp_flash_msg *msg = (struct mctp_flash_msg *)buf;
	struct flash_page *page;
	struct mctp_flash_comp comp = { 0 };

	CHECK_NULL_ARG_WITH_RETURN(mctp_p, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(buf, MCTP_ERROR);

	mctp *mctp_inst = (mctp *)mctp_p;
	if (len < sizeof(struct mctp_flash_msg))
		return MCTP_ERROR;

	if (len > PAGE_SIZE + sizeof(struct mctp_flash_msg))
		return MCTP_ERROR;

	comp.cmd = msg->cmd;
	LOG_DBG("mctp_flash message (cmd %d)", msg->cmd);
	if (msg->cmd == (MCTP_FLASH_READ | MCTP_FLASH_CMD_RESP)) {
		sys_snode_t *last = sys_slist_peek_tail(&mctp_inst->flash_page_list);
		sys_slist_find_and_remove(&mctp_inst->flash_page_list, last);
		page = CONTAINER_OF(last, struct flash_page, node);

		memcpy(page->buf, buf + sizeof(struct mctp_flash_msg), len - sizeof(struct mctp_flash_msg));
		page->offset = msg->offset;
		page->len = msg->len;

		sys_slist_prepend(&mctp_inst->flash_page_list, &page->node);
		LOG_DBG("New cache [0x%x : 0x%x]", page->offset, (page->offset + page->len - 1));
	}

	k_msgq_put(mctp_inst->mctp_flash_msgq, &comp, K_NO_WAIT);

	return 0;
}

void mctp_flash_init(mctp *mctp_inst)
{
	sys_slist_init(&mctp_inst->flash_page_list);
	mctp_inst->mctp_flash_msgq = (struct k_msgq *)malloc(sizeof(struct k_msgq));
	k_msgq_init(mctp_inst->mctp_flash_msgq, comp_buffer, sizeof(struct mctp_flash_comp), 1);

	for (int i = 0; i < FLASH_PAGE_LIST_LEN; i++) {
		struct flash_page *page = &mctp_inst->page[i];

		page->buf = malloc(PAGE_SIZE);
		page->offset = 0;
		page->len = 0;
		page->index = i;
		sys_slist_append(&mctp_inst->flash_page_list, &page->node);
	}
}

void mctp_flash_free(mctp *mctp_inst)
{
	struct flash_page *page;
	sys_snode_t *next;

	SAFE_FREE(mctp_inst->mctp_flash_msgq);

	while (true) {
		next = sys_slist_get(&mctp_inst->flash_page_list);
		if (!next)
			break;
		page = CONTAINER_OF(next, struct flash_page, node);
		SAFE_FREE(page->buf);
	}
}

static int cmd_mctp(const struct shell *shell, size_t argc, char **argv)
{
	mctp_ext_params ext_params;
	struct mctp_flash_msg req = { 0 };
	struct mctp_flash_comp comp = { 0 };
	mctp *mctp_inst = find_mctp_by_medium_type(MCTP_MEDIUM_TYPE_USB);
	uint32_t start, end;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ext_params.tag_owner = 1;
	ext_params.type = MCTP_MEDIUM_TYPE_USB;
	ext_params.ep = 0;
	req.msg_type = MCTP_MSG_TYPE_FLASH;
	req.cmd = MCTP_FLASH_READ;
	start = k_uptime_get_32();
	for (int addr = 0; addr < 0x4000000; addr += PAGE_SIZE) {
		req.len = PAGE_SIZE;
		req.offset = addr;
		LOG_DBG("Flash Read {0x%x, %zd}", addr, PAGE_SIZE);
		mctp_send_msg(mctp_inst, (uint8_t *)&req, sizeof(req), ext_params);
		k_msgq_get(mctp_inst->mctp_flash_msgq, &comp, K_FOREVER);
	}
	end = k_uptime_get_32();
	LOG_INF("mctp test spends %u seconds", (end - start)/1000);

	return 0;
}
SHELL_CMD_ARG_REGISTER(mctp, NULL, "mctp test", cmd_mctp, 1, 0);
#endif
