/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OBMF_H
#define OBMF_H

#include <stdint.h>

/* OBMF-ICP Revisions from OBMF-ICP 0.7 spec */
#define OBMF_ICP_REV_0 0x00

/* OBMF-ICP Transaction Types (spec page 17) */
#define OBMF_ICP_TRANS_SHORT_READ   0x00
#define OBMF_ICP_TRANS_SHORT_WRITE  0x01
#define OBMF_ICP_TRANS_LONG_READ    0x02
#define OBMF_ICP_TRANS_LONG_WRITE   0x03
#define OBMF_ICP_TRANS_SHORT_NOTIFY 0x04
#define OBMF_ICP_TRANS_LONG_NOTIFY  0x05

/* RqResp bit (spec page 17) */
#define OBMF_ICP_MSG_TYPE_REQUEST  0
#define OBMF_ICP_MSG_TYPE_RESPONSE 1

/**
 * @brief OBMF-ICP Message Header (spec page 17)
 */
typedef struct __attribute__((packed)) {
	uint8_t rev_rsvd;
	uint8_t channel;
	uint8_t tag_rsvd_trans_rqresp;
	/* Payload follows */
} obmf_icp_header_t;

/* Helper macros for header fields */
#define OBMF_ICP_GET_REV(h)      ((h)->rev_rsvd & 0x03)
#define OBMF_ICP_GET_CHANNEL(h)  ((h)->channel)
#define OBMF_ICP_GET_TAG(h)      (((h)->tag_rsvd_trans_rqresp >> 7) & 0x01)
#define OBMF_ICP_GET_TRANS(h)    (((h)->tag_rsvd_trans_rqresp >> 1) & 0x07)
#define OBMF_ICP_GET_RQRESP(h)   ((h)->tag_rsvd_trans_rqresp & 0x01)

#define OBMF_ICP_SET_REV(h, val)      ((h)->rev_rsvd = ((h)->rev_rsvd & ~0x03) | ((val) & 0x03))
#define OBMF_ICP_SET_CHANNEL(h, val)  ((h)->channel = (val))
#define OBMF_ICP_SET_TAG(h, val)      ((h)->tag_rsvd_trans_rqresp = ((h)->tag_rsvd_trans_rqresp & ~0x80) | (((val) & 0x01) << 7))
#define OBMF_ICP_SET_TRANS(h, val)    ((h)->tag_rsvd_trans_rqresp = ((h)->tag_rsvd_trans_rqresp & ~0x0E) | (((val) & 0x07) << 1))
#define OBMF_ICP_SET_RQRESP(h, val)   ((h)->tag_rsvd_trans_rqresp = ((h)->tag_rsvd_trans_rqresp & ~0x01) | ((val) & 0x01))


/* Completion Codes (spec page 20) */
#define OBMF_ICP_SUCCESS                    0x00
#define OBMF_ICP_ERR_UNKNOWN_CHANNEL        0x01
#define OBMF_ICP_ERR_CMD_NOT_SUPPORTED      0x02
#define OBMF_ICP_ERR_PERMANENT_ERROR        0x03
#define OBMF_ICP_ERR_NOT_READY              0x04
#define OBMF_ICP_ERR_INSUFFICIENT_PRIVILEGE 0x05
#define OBMF_ICP_ERR_ADDR_OUT_OF_RANGE      0x06
#define OBMF_ICP_ERR_OTHER                  0x07


/* Channel 0: Control Channel Definitions (spec page 25) */
#define OBMF_ICP_CONTROL_CHANNEL 0

/* Control Channel Register Offsets */
#define OBMF_VER_OFFSET           0x000
#define READ_SIZE_OFFSET          0x008
#define WRITE_SIZE_OFFSET         0x010
#define MAX_CHANNEL_NO_OFFSET     0x018
#define CHANNEL_1_OFFSET          0x100
#define CHANNEL_2_OFFSET          0x200
#define CHANNEL_3_OFFSET          0x300
#define CHANNEL_4_OFFSET          0x400
#define CHANNEL_5_OFFSET          0x500

/**
 * @brief OBMF_VER register (spec page 26)
 */
typedef struct __attribute__((packed)) {
	uint32_t obmf_ver;
} obmf_ver_reg_t;

/**
 * @brief READ_SIZE register (spec page 26)
 */
typedef struct __attribute__((packed)) {
	uint32_t read_size_sec;
	uint32_t read_size_pri;
} read_size_reg_t;

/**
 * @brief WRITE_SIZE register (spec page 26)
 */
typedef struct __attribute__((packed)) {
	uint32_t write_size_sec;
	uint32_t write_size_pri;
} write_size_reg_t;

/**
 * @brief MAX_CHANNEL_NO register (spec page 27)
 */
typedef struct __attribute__((packed)) {
	uint32_t max_channel_no;
} max_channel_no_reg_t;

/**
 * @brief Channel Configuration register (spec page 28)
 * This is a 4-byte register.
 */
typedef uint32_t channel_cfg_reg_t;

#define CHANNEL_CFG_GET_NO(reg)        ((reg) & 0xFF)
#define CHANNEL_CFG_GET_MANDATORY(reg) (((reg) >> 8) & 0x01)
#define CHANNEL_CFG_GET_ENABLED(reg)   (((reg) >> 9) & 0x01)

#define CHANNEL_CFG_SET_ENABLED(reg_ptr, val) \
    do { \
        if (val) { \
            *(reg_ptr) |= (1 << 9); \
        } else { \
            *(reg_ptr) &= ~(1 << 9); \
        } \
    } while (0)

/**
 * @brief Channel Register File structure (e.g., for Channel 1, spec page 27)
 */
typedef struct __attribute__((packed)) {
	uint8_t channel_guid[16]; /* Offset 0x00 */
	channel_cfg_reg_t cfg;    /* Offset 0x10 */
} channel_reg_file_t;


/* Channel 2: Virtual Wires Channel Definitions (spec page 34) */
#define OBMF_ICP_VIRTUAL_WIRES_CHANNEL 1

/* Channel 3: UART Channel Definitions (spec page 45) */
#define OBMF_ICP_UART_CHANNEL 2
#define UART_RBR_THR_DLL_OFFSET 0x0

/* Channel 4: Legacy I/O Channel Definitions (spec page 55) */
#define OBMF_ICP_LEGACY_IO_CHANNEL 4
#define LEGACY_IO_POST_CODE_OFFSET 0x80

/* Channel 5: Flash Channel Definitions (spec page 33) */
#define OBMF_ICP_FLASH_CHANNEL 5
#define FLASH_SPACE_OFFSET           0x0000
#define FLASH_ERASE_START_ADDR_OFFSET 0x1000
#define FLASH_ERASE_SIZE_OFFSET       0x1004


/* Virtual Wires Register Offsets */
#define VW_CFG_OFFSET         0x0
#define VW_0_STATE_OFFSET     0x4
#define VW_1_STATE_OFFSET     0x5
#define VW_2_STATE_OFFSET     0x6
#define VW_3_STATE_OFFSET     0x7
#define VW_0_DIRECTION_OFFSET 0x8
#define VW_1_DIRECTION_OFFSET 0x9
#define VW_2_DIRECTION_OFFSET 0xA
#define VW_3_DIRECTION_OFFSET 0xB

/**
 * @brief Virtual Wires Register file
 */

typedef struct __attribute__((packed)) {
	uint32_t vw_cfg;
	uint8_t vw_state[4];
	uint8_t vw_direction[4];
} obmf_vw_reg_file_t;


/**
 * @brief Function to initialize the OBMF service.
 */
void obmf_service_init(void);

/**
 * @brief Handler for incoming OBMF messages.
 *
 * @param msg Pointer to the message buffer.
 * @param len Length of the message.
 */
void obmf_message_handler(uint8_t *msg, uint32_t len);

/**
 * @brief Function to get the response for a given OBMF request.
 *
 * This function is the main entry point for the OBMF secondary service. It takes a request
 * message, processes it, and populates a response buffer.
 *
 * @param req_buf Pointer to the request message buffer.
 * @param req_len Length of the request message.
 * @param rsp_buf Pointer to the buffer where the response will be written.
 * @param rsp_len Pointer to a variable that will hold the length of the response.
 * @return 0 on success, or a negative error code.
 */
int obmf_get_response(uint8_t *req_buf, uint32_t req_len, uint8_t *rsp_buf, uint32_t *rsp_len);

/**
 * @brief Send an OBMF request message to the Primary.
 *
 * @param channel The channel number for the request.
 * @param req_buf Pointer to the request message buffer.
 * @param req_len Length of the request message.
 * @return 0 on success, negative on error.
 */
int obmf_send_request(uint8_t channel, uint8_t *req_buf, uint32_t req_len);

/**
 * @brief Read the state of a virtual wire from the Primary.
 *
 * @param wire_index The index of the virtual wire to read.
 * @param state Pointer to a variable to store the state.
 * @return 0 on success, negative on error.
 */
int obmf_vw_read_state(uint8_t wire_index, uint8_t *state);

/**
 * @brief Write the state of a virtual wire to the Primary.
 *
 * @param wire_index The index of the virtual wire to write.
 * @param state The state to write.
 * @return 0 on success, negative on error.
 */
int obmf_vw_write_state(uint8_t wire_index, uint8_t state);

/**
 * @brief Read the direction of a virtual wire from the Primary.
 *
 * @param wire_index The index of the virtual wire to read.
 * @param direction Pointer to a variable to store the direction.
 * @return 0 on success, negative on error.
 */
int obmf_vw_read_direction(uint8_t wire_index, uint8_t *direction);

/**
 * @brief Write the direction of a virtual wire to the Primary.
 *
 * @param wire_index The index of the virtual wire to write.
 * @param direction The direction to set.
 * @return 0 on success, negative on error.
 */
int obmf_vw_write_direction(uint8_t wire_index, uint8_t direction);

/**
 * @brief Write data to the UART channel.
 *
 * @param buf Pointer to the data buffer to write.
 * @param len Length of the data.
 * @return 0 on success, negative on error.
 */
int obmf_uart_write(uint8_t *buf, uint32_t len);

/**
 * @brief Write a POST code to the Legacy I/O channel.
 *
 * @param code The 8-bit POST code to write.
 * @return 0 on success, negative on error.
 */
int obmf_legacy_io_write_post_code(uint8_t code);

/**
 * @brief Read data from the flash channel.
 *
 * @param offset The offset to read from.
 * @param len The number of bytes to read.
 * @param buf Pointer to the buffer to store the read data.
 * @return 0 on success, negative on error.
 */
int obmf_flash_read(uint32_t offset, uint32_t len, uint8_t *buf);

/**
 * @brief Write data to the flash channel.
 *
 * @param offset The offset to write to.
 * @param len The number of bytes to write.
 * @param buf Pointer to the data to write.
 * @return 0 on success, negative on error.
 */
int obmf_flash_write(uint32_t offset, uint32_t len, uint8_t *buf);

/**
 * @brief Erase a region of the flash.
 *
 * @param offset The starting offset of the region to erase.
 * @param len The size of the region to erase.
 * @return 0 on success, negative on error.
 */
int obmf_flash_erase(uint32_t offset, uint32_t len);

#endif /* OBMF_H */
