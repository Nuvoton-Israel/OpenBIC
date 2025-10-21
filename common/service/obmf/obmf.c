/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "obmf.h"
#include "../mctp/mctp.h"
#include <logging/log.h>
#include <string.h>
#include <stdio.h>

#ifdef CONFIG_USB_DEVICE_OBMF
#include <usb/class/usb_obmf.h>
#include <device.h>
#endif

LOG_MODULE_REGISTER(obmf, LOG_LEVEL_INF);

#ifdef CONFIG_USB_DEVICE_OBMF
static void obmf_out_cb(const struct device *dev, uint32_t len, uint8_t *data);

static struct obmf_ops obmf_usb_ops = {
	.read = obmf_out_cb,
};

static void obmf_out_cb(const struct device *dev, uint32_t len, uint8_t *data)
{
	uint8_t rsp_buf[256];
	uint32_t rsp_len = 0;

	if (len > 0) {
		obmf_get_response(data, len, rsp_buf, &rsp_len);

		if (rsp_len > 0) {
			obmf_transport_send(OBMF_PRIMARY_ENDPOINT, rsp_buf, rsp_len);
		}
	}
}
#endif

#define OBMF_MAX_CHANNELS 5
#define OBMF_DEFAULT_MAX_TRANS_SIZE 64 // Default 64-byte payload
#define OBMF_PRIMARY_ENDPOINT 0 // Placeholder for Primary's endpoint address

// --- OBMF Transport Layer --- 
// This is a placeholder. A real implementation would use a proper transport like MCTP.
int obmf_transport_send(uint8_t dest_endpoint, uint8_t *msg, uint32_t len)
{
#if CONFIG_USB_DEVICE_OBMF
	const struct device *dev = device_get_binding(CONFIG_USB_OBMF_DEVICE_NAME "_0");
	uint32_t bytes_written = 0;

	if (!dev) {
		LOG_ERR("Failed to get OBMF USB device");
		return -1;
	}

	if (obmf_usb_ep_write(dev, msg, len, &bytes_written) != 0) {
		LOG_ERR("obmf_usb_ep_write failed");
		return -1;
	}

	if (bytes_written != len) {
		LOG_ERR("Incomplete write: %d vs %d", bytes_written, len);
		return -1;
	}

	return 0;
#else
	mctp *mctp_inst = NULL;
	mctp_ext_params ext_params = {0};

	if (len > 256) { // Safety check for stack allocation
		LOG_ERR("OBMF message too large for MCTP transport buffer: %u", len);
		return -1;
	}

	if (get_mctp_info(dest_endpoint, &mctp_inst, &ext_params) != MCTP_SUCCESS) {
		LOG_ERR("Failed to get MCTP info for endpoint 0x%x", dest_endpoint);
		return -1;
	}

	// The MCTP payload for a tunneled protocol is typically [message type, message payload].
	// The official OBMF-ICP over MCTP binding specification (DSP0295) is not yet
	// available. We assume the MCTP message type for OBMF is the IANA-defined
	// vendor type 0x7F, and that it should be prepended to the OBMF message.
	uint8_t mctp_buf[len + 1];
	mctp_buf[0] = MCTP_MSG_TYPE_VEN_DEF_IANA;
	memcpy(&mctp_buf[1], msg, len);

	if (mctp_send_msg(mctp_inst, mctp_buf, len + 1, ext_params) != MCTP_SUCCESS) {
		LOG_ERR("mctp_send_msg failed");
		return -1;
	}

	return 0;
#endif
}


// --- Channel 0 (Producer) Data ---
typedef struct __attribute__((packed)) {
	obmf_ver_reg_t obmf_ver;
	uint8_t _pad1[READ_SIZE_OFFSET - sizeof(obmf_ver_reg_t)];
	read_size_reg_t read_size;
	uint8_t _pad2[WRITE_SIZE_OFFSET - (READ_SIZE_OFFSET + sizeof(read_size_reg_t))];
	write_size_reg_t write_size;
	uint8_t _pad3[MAX_CHANNEL_NO_OFFSET - (WRITE_SIZE_OFFSET + sizeof(write_size_reg_t))];
	max_channel_no_reg_t max_channel_no;
	uint8_t _pad4[CHANNEL_1_OFFSET - (MAX_CHANNEL_NO_OFFSET + sizeof(max_channel_no_reg_t))];
	channel_reg_file_t channels[OBMF_MAX_CHANNELS];
} obmf_discovery_structure_t;

static obmf_discovery_structure_t discovery_struct;
static uint8_t channel_tags[OBMF_MAX_CHANNELS + 1] = {0}; // To track next tag for requests

// --- Forward Declarations ---
static void handle_ch0_read_req(obmf_icp_header_t *req_hdr, uint8_t *req_payload, uint32_t req_len, uint8_t *rsp_buf, uint32_t *rsp_len);
static void handle_ch0_write_req(obmf_icp_header_t *req_hdr, uint8_t *req_payload, uint32_t req_len, uint8_t *rsp_buf, uint32_t *rsp_len);
static void handle_vw_response(obmf_icp_header_t *rsp_hdr, uint8_t *rsp_payload, uint32_t rsp_len);
static void handle_uart_response(obmf_icp_header_t *rsp_hdr, uint8_t *rsp_payload, uint32_t rsp_len);
static void handle_legacy_io_response(obmf_icp_header_t *rsp_hdr, uint8_t *rsp_payload, uint32_t rsp_len);
static void handle_flash_response(obmf_icp_header_t *rsp_hdr, uint8_t *rsp_payload, uint32_t rsp_len);


// --- Public API --- 

void obmf_service_init(void)
{
#ifdef CONFIG_USB_DEVICE_OBMF
	const struct device *dev = device_get_binding(CONFIG_USB_OBMF_DEVICE_NAME "_0");
	if (dev) {
		usb_obmf_register_device(dev, &obmf_usb_ops);
	} else {
		LOG_ERR("Failed to get OBMF USB device for callback registration");
	}
#endif
	LOG_INF("OBMF Secondary Service Initialized");

	memset(&discovery_struct, 0, sizeof(discovery_struct));

	// --- Populate Channel 0 Discovery Structure ---
	discovery_struct.obmf_ver.obmf_ver = OBMF_ICP_REV_0;
	discovery_struct.read_size.read_size_sec = OBMF_DEFAULT_MAX_TRANS_SIZE;
	discovery_struct.read_size.read_size_pri = OBMF_DEFAULT_MAX_TRANS_SIZE;
	discovery_struct.write_size.write_size_sec = OBMF_DEFAULT_MAX_TRANS_SIZE;
	discovery_struct.write_size.write_size_pri = OBMF_DEFAULT_MAX_TRANS_SIZE;
	discovery_struct.max_channel_no.max_channel_no = OBMF_MAX_CHANNELS;

	// Channel :1 Virtual Wires (index 0)
	const uint8_t vw_guid[] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; 
	memcpy(discovery_struct.channels[1].channel_guid, vw_guid, 16);
	discovery_struct.channels[1].cfg = (0 << 9) | (0 << 8) | 1; // Enabled=0, Mandatory=0, Ch No=1

	// Channel 2: UART (index 1)
	const uint8_t uart_guid[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; 
	memcpy(discovery_struct.channels[2].channel_guid, uart_guid, 16);
	discovery_struct.channels[2].cfg = (0 << 9) | (0 << 8) | 2; // Enabled=0, Mandatory=0, Ch No=2

	// Channel 4: Legacy I/O (index 3)
	const uint8_t legacy_io_guid[] = {0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; 
	memcpy(discovery_struct.channels[3].channel_guid, legacy_io_guid, 16);
	discovery_struct.channels[3].cfg = (0 << 9) | (0 << 8) | 4; // Enabled=0, Mandatory=0, Ch No=4

	// Channel 5: Flash (index 4)
	const uint8_t flash_guid[] = {0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; 
	memcpy(discovery_struct.channels[4].channel_guid, flash_guid, 16);
	discovery_struct.channels[4].cfg = (0 << 9) | (1 << 8) | 5; // Enabled=0, Mandatory=1, Ch No=5
}

int obmf_get_response(uint8_t *msg, uint32_t len, uint8_t *rsp_buf, uint32_t *rsp_len)
{
	if (len < sizeof(obmf_icp_header_t)) {
		LOG_ERR("Invalid OBMF message length: %d", len);
		return -1;
	}

	obmf_icp_header_t *hdr = (obmf_icp_header_t *)msg;
	uint8_t *payload = msg + sizeof(obmf_icp_header_t);
	uint32_t payload_len = len - sizeof(obmf_icp_header_t);
	uint8_t channel = OBMF_ICP_GET_CHANNEL(hdr);

	if (OBMF_ICP_GET_RQRESP(hdr) == OBMF_ICP_MSG_TYPE_REQUEST) {
		// --- Handle incoming REQUESTS (we are the Producer) ---
		LOG_INF("Received OBMF request: Channel=%d", channel);
		switch (channel) {
		case OBMF_ICP_CONTROL_CHANNEL:
			if (OBMF_ICP_GET_TRANS(hdr) == OBMF_ICP_TRANS_SHORT_READ) {
				handle_ch0_read_req(hdr, payload, payload_len, rsp_buf, rsp_len);
			} else if (OBMF_ICP_GET_TRANS(hdr) == OBMF_ICP_TRANS_SHORT_WRITE) {
				handle_ch0_write_req(hdr, payload, payload_len, rsp_buf, rsp_len);
			} else {
				LOG_ERR("Unsupported transaction type %d for Channel 0", OBMF_ICP_GET_TRANS(hdr));
			}
			break;
		default:
			LOG_ERR("Request received for unsupported/consumer channel %d", channel);
			break;
		}
	} else { 
		// --- Handle incoming RESPONSES (we are the Consumer) ---
		LOG_INF("Received OBMF response: Channel=%d", channel);
		switch (channel) {
		case OBMF_ICP_VIRTUAL_WIRES_CHANNEL:
			handle_vw_response(hdr, payload, payload_len);
			break;
		case OBMF_ICP_UART_CHANNEL:
			handle_uart_response(hdr, payload, payload_len);
			break;
		case OBMF_ICP_LEGACY_IO_CHANNEL:
			handle_legacy_io_response(hdr, payload, payload_len);
			break;
		case OBMF_ICP_FLASH_CHANNEL:
			handle_flash_response(hdr, payload, payload_len);
			break;
		default:
			LOG_WRN("Response received for unhandled channel %d", channel);
			break;
		}
	}

	return 0;
}

int obmf_send_request(uint8_t channel, uint8_t *req_buf, uint32_t req_len)
{
    if (channel > OBMF_MAX_CHANNELS) {
        return -1; // Invalid channel
    }

	obmf_icp_header_t *hdr = (obmf_icp_header_t *)req_buf;
	OBMF_ICP_SET_REV(hdr, OBMF_ICP_REV_0);
	OBMF_ICP_SET_CHANNEL(hdr, channel);
	OBMF_ICP_SET_RQRESP(hdr, OBMF_ICP_MSG_TYPE_REQUEST);
	OBMF_ICP_SET_TAG(hdr, channel_tags[channel]);
	channel_tags[channel] = 1 - channel_tags[channel]; // Alternate tag for next request

	return obmf_transport_send(OBMF_PRIMARY_ENDPOINT, req_buf, req_len);
}

static void handle_ch0_read_req(obmf_icp_header_t *req_hdr, uint8_t *req_payload, uint32_t req_len, uint8_t *rsp_buf, uint32_t *rsp_len)
{
	if (req_len < 9) { // 8-byte address + 1-byte size
		LOG_ERR("Invalid CH0 read request payload length: %d", req_len);
		return;
	}

	uint64_t addr = *(uint64_t *)req_payload;
	uint8_t read_size = req_payload[8];

	LOG_INF("CH0 Read Request: addr=0x%llx, size=%d", addr, read_size);

	obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;
	uint8_t *rsp_payload = rsp_buf + sizeof(obmf_icp_header_t);
	
	*rsp_hdr = *req_hdr;
	OBMF_ICP_SET_RQRESP(rsp_hdr, OBMF_ICP_MSG_TYPE_RESPONSE);

	if (addr + read_size > sizeof(discovery_struct)) {
		LOG_ERR("CH0 read out of bounds: addr=0x%llx, size=%d", addr, read_size);
		rsp_payload[0] = OBMF_ICP_ERR_ADDR_OUT_OF_RANGE;
		*rsp_len = sizeof(obmf_icp_header_t) + 1;
		return;
	}

	rsp_payload[0] = OBMF_ICP_SUCCESS;
	memcpy(rsp_payload + 1, (uint8_t *)&discovery_struct + addr, read_size);
	
	*rsp_len = sizeof(obmf_icp_header_t) + 1 + read_size;
}

static void handle_ch0_write_req(obmf_icp_header_t *req_hdr, uint8_t *req_payload, uint32_t req_len, uint8_t *rsp_buf, uint32_t *rsp_len)
{
	if (req_len < 10) { // 8-byte address + 1-byte size + at least 1 byte of data
		LOG_ERR("Invalid CH0 write request payload length: %d", req_len);
		return;
	}

	uint64_t addr = *(uint64_t *)req_payload;
	uint8_t write_size = req_payload[8];
	uint8_t *write_data = req_payload + 9;

	if (req_len != 9 + write_size) {
		LOG_ERR("CH0 write request size mismatch: payload_len=%d, write_size=%d", req_len, write_size);
		return;
	}

	LOG_INF("CH0 Write Request: addr=0x%llx, size=%d", addr, write_size);

	obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;
	uint8_t *rsp_payload = rsp_buf + sizeof(obmf_icp_header_t);

	*rsp_hdr = *req_hdr;
	OBMF_ICP_SET_RQRESP(rsp_hdr, OBMF_ICP_MSG_TYPE_RESPONSE);

	if (addr + write_size > sizeof(discovery_struct)) {
		LOG_ERR("CH0 write out of bounds: addr=0x%llx, size=%d", addr, write_size);
		rsp_payload[0] = OBMF_ICP_ERR_ADDR_OUT_OF_RANGE;
		*rsp_len = sizeof(obmf_icp_header_t) + 1;
		return;
	}

	// A real implementation would need more robust, atomic, and secure access control.
	// For now, we allow writes to the discovery structure.
	memcpy((uint8_t *)&discovery_struct + addr, write_data, write_size);

	rsp_payload[0] = OBMF_ICP_SUCCESS;
	*rsp_len = sizeof(obmf_icp_header_t) + 1;
}


// --- Virtual Wires (Consumer) Logic ---

static void handle_vw_response(obmf_icp_header_t *rsp_hdr, uint8_t *rsp_payload, uint32_t rsp_len)
{
    if (rsp_len > 0 && rsp_payload[0] == OBMF_ICP_SUCCESS) {
        LOG_INF("VW Operation Success");
        if (rsp_len > 1) {
            // A real implementation would pass this data back to the caller
            // For now, we just log it.
            LOG_HEXDUMP_INF(rsp_payload + 1, rsp_len - 1, "VW Response Data:");
        }
    } else {
        LOG_ERR("VW Operation Failed: code=0x%x", rsp_len > 0 ? rsp_payload[0] : -1);
    }
}

int obmf_vw_read_state(uint8_t wire_index, uint8_t *state)
{
	uint8_t tx_buf[16];
	obmf_icp_header_t *hdr = (obmf_icp_header_t *)tx_buf;
	OBMF_ICP_SET_TRANS(hdr, OBMF_ICP_TRANS_SHORT_READ);

	uint64_t *addr = (uint64_t *)(tx_buf + sizeof(obmf_icp_header_t));
	uint8_t *size = (uint8_t *)(tx_buf + sizeof(obmf_icp_header_t) + sizeof(uint64_t));

	*addr = VW_0_STATE_OFFSET + wire_index;
	*size = 1;

	return obmf_send_request(OBMF_ICP_VIRTUAL_WIRES_CHANNEL, tx_buf, sizeof(obmf_icp_header_t) + 9);
}

int obmf_vw_write_state(uint8_t wire_index, uint8_t state)
{
	uint8_t tx_buf[16];
	obmf_icp_header_t *hdr = (obmf_icp_header_t *)tx_buf;
	OBMF_ICP_SET_TRANS(hdr, OBMF_ICP_TRANS_SHORT_WRITE);

	uint64_t *addr = (uint64_t *)(tx_buf + sizeof(obmf_icp_header_t));
	uint8_t *size = (uint8_t *)(tx_buf + sizeof(obmf_icp_header_t) + sizeof(uint64_t));
	uint8_t *data = (uint8_t *)(tx_buf + sizeof(obmf_icp_header_t) + sizeof(uint64_t) + 1);

	*addr = VW_0_STATE_OFFSET + wire_index;
	*size = 1;
	*data = state;

	return obmf_send_request(OBMF_ICP_VIRTUAL_WIRES_CHANNEL, tx_buf, sizeof(obmf_icp_header_t) + 10);
}


// --- UART (Consumer) Logic ---

static void handle_uart_response(obmf_icp_header_t *rsp_hdr, uint8_t *rsp_payload, uint32_t rsp_len)
{
    if (rsp_len > 0 && rsp_payload[0] == OBMF_ICP_SUCCESS) {
        LOG_INF("UART Write Success");
    } else {
        LOG_ERR("UART Write Failed: code=0x%x", rsp_len > 0 ? rsp_payload[0] : -1);
    }
}

int obmf_uart_write(uint8_t *buf, uint32_t len)
{
	// Note: This sends the entire buffer as a single write request.
	// A real UART driver would likely send byte by byte.
	// This also uses a long write, which might not be optimal for single bytes.
	uint8_t tx_buf[sizeof(obmf_icp_header_t) + 10 + len]; // Header + Long Write Hdr + data
	obmf_icp_header_t *hdr = (obmf_icp_header_t *)tx_buf;
	OBMF_ICP_SET_TRANS(hdr, OBMF_ICP_TRANS_LONG_WRITE);

	uint64_t *addr = (uint64_t *)(tx_buf + sizeof(obmf_icp_header_t));
	uint16_t *write_len = (uint16_t *)(tx_buf + sizeof(obmf_icp_header_t) + sizeof(uint64_t));
	uint8_t *data = (uint8_t *)(tx_buf + sizeof(obmf_icp_header_t) + sizeof(uint64_t) + sizeof(uint16_t));

	*addr = UART_RBR_THR_DLL_OFFSET;
	*write_len = len;
	memcpy(data, buf, len);

	return obmf_send_request(OBMF_ICP_UART_CHANNEL, tx_buf, sizeof(obmf_icp_header_t) + 10 + len);
}


// --- Legacy I/O (Consumer) Logic ---

static void handle_legacy_io_response(obmf_icp_header_t *rsp_hdr, uint8_t *rsp_payload, uint32_t rsp_len)
{
    if (rsp_len > 0 && rsp_payload[0] == OBMF_ICP_SUCCESS) {
        LOG_INF("Legacy I/O Write Success");
    } else {
        LOG_ERR("Legacy I/O Write Failed: code=0x%x", rsp_len > 0 ? rsp_payload[0] : -1);
    }
}

int obmf_legacy_io_write_post_code(uint8_t code)
{
	uint8_t tx_buf[sizeof(obmf_icp_header_t) + 10]; // Header + Short Write Hdr
	obmf_icp_header_t *hdr = (obmf_icp_header_t *)tx_buf;
	OBMF_ICP_SET_TRANS(hdr, OBMF_ICP_TRANS_SHORT_WRITE);

	uint64_t *addr = (uint64_t *)(tx_buf + sizeof(obmf_icp_header_t));
	uint8_t *size = (uint8_t *)(tx_buf + sizeof(obmf_icp_header_t) + sizeof(uint64_t));
	uint8_t *data = (uint8_t *)(tx_buf + sizeof(obmf_icp_header_t) + sizeof(uint64_t) + 1);

	*addr = LEGACY_IO_POST_CODE_OFFSET;
	*size = 1;
	*data = code;

	return obmf_send_request(OBMF_ICP_LEGACY_IO_CHANNEL, tx_buf, sizeof(obmf_icp_header_t) + 10);
}


// --- Flash (Consumer) Logic ---

static void handle_flash_response(obmf_icp_header_t *rsp_hdr, uint8_t *rsp_payload, uint32_t rsp_len)
{
    if (rsp_len > 0 && rsp_payload[0] == OBMF_ICP_SUCCESS) {
        LOG_INF("Flash Operation Success");
        if (rsp_len > 1) {
            LOG_HEXDUMP_INF(rsp_payload + 1, rsp_len - 1, "Flash Response Data:");
        }
    } else {
        LOG_ERR("Flash Operation Failed: code=0x%x", rsp_len > 0 ? rsp_payload[0] : -1);
    }
}

int obmf_flash_read(uint32_t offset, uint32_t len, uint8_t *buf)
{
	// This implementation doesn't handle receiving the read data, it only sends the request.
	// A real implementation would need a mechanism (e.g. callbacks) to return the data.
	uint8_t tx_buf[sizeof(obmf_icp_header_t) + 10]; // Header + Long Read Hdr
	obmf_icp_header_t *hdr = (obmf_icp_header_t *)tx_buf;
	OBMF_ICP_SET_TRANS(hdr, OBMF_ICP_TRANS_LONG_READ);

	uint64_t *addr = (uint64_t *)(tx_buf + sizeof(obmf_icp_header_t));
	uint16_t *read_len = (uint16_t *)(tx_buf + sizeof(obmf_icp_header_t) + sizeof(uint64_t));

	*addr = FLASH_SPACE_OFFSET + offset;
	*read_len = len;

	return obmf_send_request(OBMF_ICP_FLASH_CHANNEL, tx_buf, sizeof(obmf_icp_header_t) + 10);
}

int obmf_flash_write(uint32_t offset, uint32_t len, uint8_t *buf)
{
	uint8_t tx_buf[sizeof(obmf_icp_header_t) + 10 + len];
	obmf_icp_header_t *hdr = (obmf_icp_header_t *)tx_buf;
	OBMF_ICP_SET_TRANS(hdr, OBMF_ICP_TRANS_LONG_WRITE);

	uint64_t *addr = (uint64_t *)(tx_buf + sizeof(obmf_icp_header_t));
	uint16_t *write_len = (uint16_t *)(tx_buf + sizeof(obmf_icp_header_t) + sizeof(uint64_t));
	uint8_t *data = (uint8_t *)(tx_buf + sizeof(obmf_icp_header_t) + sizeof(uint64_t) + sizeof(uint16_t));

	*addr = FLASH_SPACE_OFFSET + offset;
	*write_len = len;
	memcpy(data, buf, len);

	return obmf_send_request(OBMF_ICP_FLASH_CHANNEL, tx_buf, sizeof(obmf_icp_header_t) + 10 + len);
}

int obmf_flash_erase(uint32_t offset, uint32_t len)
{
	int ret = 0;
	uint8_t tx_buf[sizeof(obmf_icp_header_t) + 13]; // Header + Short Write Hdr for 4 bytes
	obmf_icp_header_t *hdr = (obmf_icp_header_t *)tx_buf;
	OBMF_ICP_SET_TRANS(hdr, OBMF_ICP_TRANS_SHORT_WRITE);

	uint64_t *addr = (uint64_t *)(tx_buf + sizeof(obmf_icp_header_t));
	uint8_t *size = (uint8_t *)(tx_buf + sizeof(obmf_icp_header_t) + sizeof(uint64_t));
	uint32_t *data = (uint32_t *)(tx_buf + sizeof(obmf_icp_header_t) + sizeof(uint64_t) + 1);

	// 1. Write erase start address
	*addr = FLASH_ERASE_START_ADDR_OFFSET;
	*size = sizeof(uint32_t);
	*data = offset;
	ret = obmf_send_request(OBMF_ICP_FLASH_CHANNEL, tx_buf, sizeof(obmf_icp_header_t) + 9 + sizeof(uint32_t));
	if (ret != 0) {
		return ret;
	}

	// 2. Write erase size (this is assumed to trigger the erase)
	*addr = FLASH_ERASE_SIZE_OFFSET;
	*size = sizeof(uint32_t);
	*data = len;
	ret = obmf_send_request(OBMF_ICP_FLASH_CHANNEL, tx_buf, sizeof(obmf_icp_header_t) + 9 + sizeof(uint32_t));

	return ret;
}
