/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal shared definitions for OBMF channel implementations.
 */

#ifndef OBMF_INTERNAL_H
#define OBMF_INTERNAL_H

#include "obmf.h"
#include <logging/log.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <kernel.h>

/* Internal constants shared across channel files */
#define OBMF_REQUEST_MAX_RETRY 2
#define OBMF_REQUEST_RETRY_DELAY_MS 10
/*
 * A request still unanswered after this long is abandoned, so that a lost
 * response cannot block its channel for good (matches the BMC driver).
 */
/* Must exceed the longest caller wait (mem-dev, 10 s) */
#define OBMF_REQUEST_TIMEOUT_MS 12000

#define OBMF_HEADER_SIZE sizeof(obmf_icp_header_t)
/* Size of the buffer every channel handler builds its response in */
#define OBMF_RSP_BUF_SIZE 256
#define OBMF_MMIO_SUB_HDR_SIZE sizeof(obmf_icp_mmio_sub_header_t)
#define OBMF_FULL_MMIO_HDR_SIZE (OBMF_HEADER_SIZE + OBMF_MMIO_SUB_HDR_SIZE)

/* Build a response MMIO header: same transaction, reserved bits zero */
static inline void obmf_mmio_rsp_sub_header(obmf_icp_mmio_sub_header_t *rsp,
					    const obmf_icp_mmio_sub_header_t *req)
{
	rsp->trans_rsvd = OBMF_ICP_MMIO_GET_TRANS(req);
	rsp->reserved = 0;
}
#define OBMF_ICP_IO_SUB_HDR_SIZE sizeof(obmf_icp_io_sub_header_t)
#define OBMF_FULL_IO_HDR_SIZE (OBMF_HEADER_SIZE + OBMF_ICP_IO_SUB_HDR_SIZE)

/*
 * Per-channel state. is_outstanding applies to every channel; producer_tag
 * and consumer_tag are only read/written for the I/O channel - v1.0.0 RC1
 * removed the Tag from the MMIO sub-header, so no other channel uses them.
 */
typedef struct {
	bool is_outstanding;
	uint8_t producer_tag; /* Only used for I/O channel */
	uint8_t consumer_tag; /* Only used for I/O channel */
	int64_t sent_at;	/* k_uptime_get() when the request was queued */
} OBMF_Channel_Tags;

extern OBMF_Channel_Tags channel_tags[];

/* Internal channel handler declarations (called from obmf.c dispatch) */
void handle_mmio_read_req(obmf_icp_header_t *req_hdr,
			  uint8_t *req_payload, uint32_t req_len,
			  uint8_t *rsp_buf, uint32_t *rsp_len);
void handle_mmio_write_req(obmf_icp_header_t *req_hdr,
			   uint8_t *req_payload, uint32_t req_len,
			   uint8_t *rsp_buf, uint32_t *rsp_len);
void handle_mmio_response(obmf_icp_header_t *rsp_hdr,
			  uint8_t *payload, uint32_t payload_len);
void handle_ipmi_response(obmf_icp_header_t *rsp_hdr,
			  uint8_t *payload, uint32_t payload_len);
void handle_mem_dev_response(obmf_icp_header_t *rsp_hdr,
			     uint8_t *payload, uint32_t payload_len);
void handle_spi_request(obmf_icp_header_t *req_hdr,
			uint8_t *payload, uint32_t payload_len,
			uint8_t *rsp_buf, uint32_t *rsp_len);
void handle_gpio_request(obmf_icp_header_t *req_hdr,
			 uint8_t *payload, uint32_t payload_len,
			 uint8_t *rsp_buf, uint32_t *rsp_len);
void handle_gpio_response(obmf_icp_header_t *rsp_hdr,
			  uint8_t *payload, uint32_t payload_len);
void handle_serial_request(obmf_icp_header_t *req_hdr,
			   uint8_t *payload, uint32_t payload_len,
			   uint8_t *rsp_buf, uint32_t *rsp_len);
void handle_serial_response(obmf_icp_header_t *rsp_hdr,
			    uint8_t *payload, uint32_t payload_len);
void handle_io_request(obmf_icp_header_t *req_hdr,
		       uint8_t *req_payload, uint32_t req_len,
		       uint8_t *rsp_buf, uint32_t *rsp_len);
void handle_io_response(obmf_icp_header_t *rsp_hdr,
			uint8_t *payload, uint32_t payload_len);
void handle_i2c_request(obmf_icp_header_t *req_hdr,
			uint8_t *req_payload, uint32_t req_len,
			uint8_t *rsp_buf, uint32_t *rsp_len);
void handle_i2c_target_response(obmf_icp_header_t *rsp_hdr,
				uint8_t *payload, uint32_t payload_len);
void handle_i3c_request(obmf_icp_header_t *req_hdr,
			uint8_t *req_payload, uint32_t req_len,
			uint8_t *rsp_buf, uint32_t *rsp_len);
void handle_i3c_response(obmf_icp_header_t *rsp_hdr,
			 uint8_t *payload, uint32_t payload_len);
void obmf_i2c_target_channels_init(void);
void obmf_serial_channels_init(void);

/* Error response builder (defined in obmf.c) */
void obmf_build_err_response(obmf_icp_header_t *req_hdr,
			     uint8_t error_code,
			     uint8_t *rsp_buf, uint32_t *rsp_len,
			     bool alt);

#define OBMF_BUILD_ERR_AND_RETURN(hdr, err, buf, len, alt) \
	do { \
		obmf_build_err_response(hdr, err, buf, len, alt); \
		return; \
	} while (0)

#endif /* OBMF_INTERNAL_H */
