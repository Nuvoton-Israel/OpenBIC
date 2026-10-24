/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OBMF_H
#define OBMF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ipmi.h"

/* OBMF-ICP Channel Types (v0.9 spec section 9.1.1) */
#define OBMF_ICP_CHANNEL_TYPE_CONFIG         0x00
#define OBMF_ICP_CHANNEL_TYPE_MMIO           0x01
#define OBMF_ICP_CHANNEL_TYPE_GPIO           0x02
#define OBMF_ICP_CHANNEL_TYPE_SERIAL         0x03
#define OBMF_ICP_CHANNEL_TYPE_I2C_CONTROLLER 0x04
#define OBMF_ICP_CHANNEL_TYPE_I2C_TARGET     0x05
#define OBMF_ICP_CHANNEL_TYPE_I3C_CONTROLLER 0x06
#define OBMF_ICP_CHANNEL_TYPE_IPMI           0x07
#define OBMF_ICP_CHANNEL_TYPE_SPI_CONTROLLER 0x08
#define OBMF_ICP_CHANNEL_TYPE_IO             0x09
#define OBMF_ICP_CHANNEL_TYPE_MEM_DEVICE      0x0A

/* Backward-compat aliases */
#define OBMF_ICP_CHANNEL_TYPE_I2C  OBMF_ICP_CHANNEL_TYPE_I2C_CONTROLLER
#define OBMF_ICP_CHANNEL_TYPE_I3C  OBMF_ICP_CHANNEL_TYPE_I3C_CONTROLLER
#define OBMF_ICP_CHANNEL_TYPE_SPI  OBMF_ICP_CHANNEL_TYPE_SPI_CONTROLLER

/* OBMF-ICP Transaction Types (MMIO Sub-Header, spec section 9.1.2) */
#define OBMF_ICP_TRANS_SHORT_READ   0x00
#define OBMF_ICP_TRANS_SHORT_WRITE  0x01
#define OBMF_ICP_TRANS_LONG_READ    0x02
#define OBMF_ICP_TRANS_LONG_WRITE   0x03

/* I/O Channel Transaction Types (spec section 4.31) */
#define OBMF_ICP_IO_TRANS_SEQ_READ_8      0
#define OBMF_ICP_IO_TRANS_SEQ_WRITE_8     1
#define OBMF_ICP_IO_TRANS_SEQ_READ_16     2
#define OBMF_ICP_IO_TRANS_SEQ_WRITE_16    3
#define OBMF_ICP_IO_TRANS_SEQ_READ_32     4
#define OBMF_ICP_IO_TRANS_SEQ_WRITE_32    5
#define OBMF_ICP_IO_TRANS_FIXED_READ_8    6
#define OBMF_ICP_IO_TRANS_FIXED_WRITE_8   7
#define OBMF_ICP_IO_TRANS_FIXED_READ_16   8
#define OBMF_ICP_IO_TRANS_FIXED_WRITE_16  9
#define OBMF_ICP_IO_TRANS_FIXED_READ_32   10
#define OBMF_ICP_IO_TRANS_FIXED_WRITE_32  11

/* RqResp bit */
#define OBMF_ICP_MSG_TYPE_REQUEST  0
#define OBMF_ICP_MSG_TYPE_RESPONSE 1

/**
 * @brief OBMF-ICP Common Header (v1.0.0 RC1, 4 bytes)
 *
 * Byte 0: Channel Number [7:0]
 * Byte 1: Status [6:0], RqResp [7]
 * Byte 2-3: Size (uint16_t LE) - payload size in bytes
 */
typedef struct __attribute__((packed)) {
	uint8_t channel;        /* Byte 0: Channel Number */
	uint8_t rqresp_status;  /* Byte 1: [7]=RqResp, [6:0]=Status */
	uint16_t size;          /* Byte 2-3: Payload size (LE) */
} obmf_icp_header_t;

/**
 * @brief OBMF-ICP MMIO Channel header (OBMF-ICP 1.0.0 Table 5, 2 bytes)
 *
 * Byte 0: Transaction Type [2:0], Reserved [7:3]
 * Byte 1: Reserved, set to 0
 *
 * Also used by Channel 0, which is accessed with MMIO Short Read/Write.
 */
typedef struct __attribute__((packed)) {
	uint8_t trans_rsvd;     /* Byte 0: [2:0]=Transaction, [7:3]=Reserved */
	uint8_t reserved;       /* Byte 1: Reserved, set to 0 */
} obmf_icp_mmio_sub_header_t;

/**
 * @brief OBMF-ICP I/O Channel Sub-Header (v0.9.2, 2 bytes)
 *
 * Byte 0: Transaction Type [3:0], Reserved [7:4]
 * Byte 1: Tag [7:0]
 */
typedef struct __attribute__((packed)) {
	uint8_t trans_rsvd;     /* Byte 0: [3:0]=Transaction, [7:4]=Reserved */
	uint8_t tag;            /* Byte 1: Tag */
} obmf_icp_io_sub_header_t;

/* I/O Sub-Header macros */
#define OBMF_ICP_IO_GET_TRANS(sh)  ((sh)->trans_rsvd & 0x0F)
#define OBMF_ICP_IO_GET_TAG(sh)    ((sh)->tag)
#define OBMF_ICP_IO_SET_TRANS(sh, val) \
	do { \
		(sh)->trans_rsvd &= ~0x0F; \
		(sh)->trans_rsvd |= ((val) & 0x0F); \
	} while (0)
#define OBMF_ICP_IO_SET_TAG(sh, val)   ((sh)->tag = (val))

/* Common Header macros */
#define OBMF_ICP_GET_CHANNEL(h)       ((h)->channel)
#define OBMF_ICP_GET_RQRESP(h)        (((h)->rqresp_status >> 7) & 0x01)
#define OBMF_ICP_GET_STATUS(h)        ((h)->rqresp_status & 0x7F)
#define OBMF_ICP_GET_SIZE(h)          ((h)->size)

#define OBMF_ICP_SET_CHANNEL(h, val)       ((h)->channel = (val))
#define OBMF_ICP_SET_RQRESP(h, val) \
	((h)->rqresp_status = ((h)->rqresp_status & 0x7F) | (((val) & 0x01) << 7))
#define OBMF_ICP_SET_STATUS(h, val) \
	((h)->rqresp_status = ((h)->rqresp_status & 0x80) | ((val) & 0x7F))
#define OBMF_ICP_SET_RQRESP_STATUS(h, rq, st) \
	((h)->rqresp_status = (((rq) & 0x01) << 7) | ((st) & 0x7F))
#define OBMF_ICP_SET_SIZE(h, val)          ((h)->size = (val))

/* MMIO Sub-Header macros */
#define OBMF_ICP_MMIO_GET_TRANS(sh)  ((sh)->trans_rsvd & 0x07)

#define OBMF_ICP_MMIO_SET_TRANS(sh, val) \
	do { \
		(sh)->trans_rsvd &= ~0x07; \
		(sh)->trans_rsvd |= ((val) & 0x07); \
	} while (0)

/*
 * Generic Status Codes (v0.9 spec section 9.1.1)
 * Range: 0x00 - 0x3F
 */
#define OBMF_ICP_STATUS_SUCCESS                0x00
#define OBMF_ICP_STATUS_INVALID_CMD            0x01
#define OBMF_ICP_STATUS_TIMEOUT                0x02
#define OBMF_ICP_STATUS_NOT_READY              0x03
#define OBMF_ICP_STATUS_PERMANENT_ERROR        0x04
#define OBMF_ICP_STATUS_UNKNOWN_CHANNEL        0x05
#define OBMF_ICP_STATUS_SIZE_NOT_SUPPORTED     0x06

/*
 * MMIO Channel-specific Status Codes (v0.9 spec section 9.1.2)
 * Range: 0x40 - 0x7F
 */
#define OBMF_ICP_MMIO_STATUS_ADDR_OUT_OF_RANGE    0x40
#define OBMF_ICP_MMIO_STATUS_ACCESS_DENIED        0x41

/*
 * I/O Channel-specific Status Codes (v0.9.2 spec section 4.31)
 * Range: 0x40 - 0x7F
 */
#define OBMF_IO_STATUS_ADDR_OUT_OF_RANGE          0x40
#define OBMF_IO_STATUS_ACCESS_DENIED              0x41

/* Backward-compat aliases (map old names to new generic status codes) */
#define OBMF_ICP_SUCCESS                    OBMF_ICP_STATUS_SUCCESS
#define OBMF_ICP_ERR_UNKNOWN_CHANNEL        OBMF_ICP_STATUS_UNKNOWN_CHANNEL
#define OBMF_ICP_ERR_CMD_NOT_SUPPORTED      OBMF_ICP_STATUS_INVALID_CMD
#define OBMF_ICP_ERR_PERMANENT_ERROR        OBMF_ICP_STATUS_PERMANENT_ERROR
#define OBMF_ICP_ERR_NOT_READY              OBMF_ICP_STATUS_NOT_READY
#define OBMF_ICP_ERR_INSUFFICIENT_PRIVILEGE OBMF_ICP_MMIO_STATUS_ACCESS_DENIED
#define OBMF_ICP_ERR_ADDR_OUT_OF_RANGE      OBMF_ICP_MMIO_STATUS_ADDR_OUT_OF_RANGE
#define OBMF_ICP_ERR_OTHER                  0x07

/* Channel 0: Control Channel */
#define OBMF_ICP_CONTROL_CHANNEL 0


/**
 * @brief Check if the given channel type uses MMIO sub-header format.
 */
static inline bool obmf_icp_uses_mmio_subhdr(uint8_t channel_type)
{
	return (channel_type == OBMF_ICP_CHANNEL_TYPE_CONFIG ||
		channel_type == OBMF_ICP_CHANNEL_TYPE_MMIO);
}

/* =====================================================================
 * Channel 0 Discovery Structure (v0.9 spec section 9.2)
 * ===================================================================== */

/* OBMF Device Role */
#define OBMF_DEVICE_ROLE_UNSPECIFIED 0
#define OBMF_DEVICE_ROLE_HOST       1
#define OBMF_DEVICE_ROLE_DEVICE     2

/* Device name max length (32 bytes including NUL) */
#define OBMF_DEVICE_NAME_SIZE 32

/* Channel Configuration Space offsets */
#define CHAN_CFG_TYPE_OFFSET              0x00
#define CHAN_CFG_NUMBER_OFFSET            0x01
#define CHAN_CFG_NAME_OFFSET              0x02
#define CHAN_CFG_DEVICE_ROLE_OFFSET       0x12
#define CHAN_CFG_STATUS_OFFSET            0x13
#define CHAN_CFG_CONTROL_OFFSET           0x14
#define CHAN_CFG_SPECIFIC_STATUS_OFFSET   0x15
#define CHAN_CFG_SPECIFIC_CONTROL_OFFSET  0x16
#define CHAN_CFG_MAX_REQ_PAYLOAD_OFFSET   0x17
#define CHAN_CFG_MAX_RSP_PAYLOAD_OFFSET   0x19
#define CHAN_CFG_CONFIG_SIZE_OFFSET       0x1C
#define CHAN_CFG_CONFIG_DATA_OFFSET       0x20

/* Channel Configuration Header DEVICE_ROLE values (spec 4.13) */
#define OBMF_CH_DEVICE_ROLE_PRODUCER 0
#define OBMF_CH_DEVICE_ROLE_CONSUMER 1

/* CHANNEL_STATUS bits (section 9.2) */
#define CHAN_STATUS_HEALTH_MASK        0x0F
#define CHAN_STATUS_RESET_DONE         (1 << 4)
#define CHAN_STATUS_CONFIG_VALID       (1 << 5)
#define CHAN_STATUS_CONFIG_LOCKED      (1 << 6)
#define CHAN_STATUS_ERROR_INDICATOR    (1 << 7)

/* CHANNEL_STATUS health values (Table 65; 4-14 are reserved) */
#define CHAN_HEALTH_UNKNOWN            0
#define CHAN_HEALTH_READY              1
#define CHAN_HEALTH_DISABLED           2
#define CHAN_HEALTH_NOT_READY          3
#define CHAN_HEALTH_ERROR              15

/* CHANNEL_CONTROL bits (section 9.2) */
#define CHAN_CONTROL_ENABLE            (1 << 0)
#define CHAN_CONTROL_RESET             (1 << 1)
#define CHAN_CONTROL_APPLY_CONFIG      (1 << 2)
#define CHAN_CONTROL_MANUF_LOCK_REQ    (1 << 3)
#define CHAN_CONTROL_CLEAR_ERROR       (1 << 4)

/* CONFIG_STATUS bits (Table 64) */
#define CH0_CONFIG_STATUS_MANUF_LOCK_SUPPORTED (1 << 0)
#define CH0_CONFIG_STATUS_MANUF_LOCKED         (1 << 1)
#define CH0_CONFIG_STATUS_MANUF_LOCK_REQ       (1 << 2)
#define CH0_CONFIG_STATUS_MANUF_VALID          (1 << 3)
#define CH0_CONFIG_STATUS_RUNTIME_APPLIED      (1 << 4)

/**
 * @brief Channel 0 base registers (v1.0.0 RC1 spec section 4.13)
 */
typedef struct __attribute__((packed)) {
	uint8_t version_minor_bcd;  /* 0x00: Minor version (BCD) */
	uint8_t version_major_bcd;  /* 0x01: Major version (BCD) */
	uint16_t vendor_id;         /* 0x02: PCI-SIG Vendor ID */
	uint16_t device_id;         /* 0x04: Device ID */
	char device_name[OBMF_DEVICE_NAME_SIZE]; /* 0x06-0x25: UTF-8 NUL-padded */
	uint8_t num_of_channels;    /* 0x26: Number of channels (excluding Ch0) */
	uint8_t config_status;      /* 0x27: Configuration status */
	uint32_t vendor_config_offset; /* 0x28: Offset to vendor config region */
	/* Channel offsets follow at 0x2C, 4 bytes each */
} obmf_ch0_base_t;

/* Channels currently allocated up to ch19 (includes Serial1, SPI1, I3C x2). */
#define OBMF_MAX_CHANNELS 19

/**
 * @brief Channel Configuration Space Common Header (v1.0.0 RC1 spec section 4.13)
 */
typedef struct __attribute__((packed)) {
	uint8_t channel_type;            /* 0x00: Channel type */
	uint8_t channel_number;          /* 0x01: Channel number */
	char channel_name[16];           /* 0x02: UTF-8/ASCII NUL-padded name */
	uint8_t device_role;             /* 0x12: 0=Producer, 1=Consumer */
	uint8_t channel_status;          /* 0x13: Health status */
	uint8_t channel_control;         /* 0x14: Channel control */
	uint8_t channel_specific_status; /* 0x15: Channel-specific status */
	uint8_t channel_specific_control;/* 0x16: Channel-specific control */
	uint16_t max_request_payload_size;  /* 0x17: Max request size (bytes) */
	uint16_t max_response_payload_size; /* 0x19: Max response size (bytes) */
	uint8_t reserved;                /* 0x1B: Reserved */
	uint32_t config_size;            /* 0x1C: Size of config data */
	/* CONFIGURATION_DATA follows at 0x20 */
} obmf_channel_config_header_t;

/**
 * @brief MMIO Channel Configuration Data (v0.9 section 9.2.1)
 */
typedef struct __attribute__((packed)) {
	uint16_t max_mmio_read_size;    /* 0x00: Max read transfer size (bytes) */
	uint16_t max_mmio_write_size;   /* 0x02: Max write transfer size (bytes) */
} obmf_mmio_config_data_t;

/**
 * @brief Full Channel Configuration Space for MMIO channel
 */
typedef struct __attribute__((packed)) {
	obmf_channel_config_header_t header;
	obmf_mmio_config_data_t config;
} obmf_mmio_channel_config_t;

/* Helper - old discovery struct definition removed; new one below includes all channels */

/* =====================================================================
 * MMIO Channel Structures (unchanged from previous version)
 * ===================================================================== */

/* Channel 1: MMIO Channel Definitions */
#define OBMF_ICP_MMIO_CHANNEL 1

#define MMIO_SPACE_OFFSET 0x0
#define OBMF_MMIO_SPACE_SIZE 128

/* FLASH region mapped within MMIO space */
#define OBMF_FLASH_SPACE_SIZE              128
#define OBMF_FLASH_ERASE_START_ADDR_OFFSET 0x1000
#define OBMF_FLASH_ERASE_SIZE_OFFSET       0x1004
#define OBMF_MMIO_FLASH_BASE_OFFSET         0x4000
#define OBMF_MMIO_FLASH_ERASE_START_ADDR    (OBMF_MMIO_FLASH_BASE_OFFSET + OBMF_FLASH_ERASE_START_ADDR_OFFSET)
#define OBMF_MMIO_FLASH_ERASE_SIZE          (OBMF_MMIO_FLASH_BASE_OFFSET + OBMF_FLASH_ERASE_SIZE_OFFSET)

typedef struct __attribute__((packed)) {
	uint32_t erase_start_address;
	uint32_t erase_size;
} obmf_flash_erase_regs_t;

/* VW region */
#define OBMF_VW_MAX_COUNT 4
#define OBMF_MMIO_VW_BASE_OFFSET        0x8000
#define OBMF_VW_STATE_OFFSET            0x04
#define OBMF_VW_DIRECTION_OFFSET        0x08

typedef union {
	uint32_t raw;
	struct {
		uint32_t vw_no          : 8;
		uint32_t vw_notification: 1;
		uint32_t reserved       : 23;
	} __attribute__((packed)) bits;
} obmf_vw_cfg_reg_t;

typedef union {
	uint8_t raw;
	struct {
		uint8_t vw_state : 1;
		uint8_t reserved : 7;
	} __attribute__((packed)) bits;
} obmf_vw_state_reg_t;

typedef union {
	uint8_t raw;
	struct {
		uint8_t vw_direction : 2;
		uint8_t reserved     : 6;
	} __attribute__((packed)) bits;
} obmf_vw_direction_reg_t;

typedef struct __attribute__((packed)) {
	obmf_vw_cfg_reg_t       vw_cfg;
	obmf_vw_state_reg_t     vw_state[OBMF_VW_MAX_COUNT];
	obmf_vw_direction_reg_t vw_direction[OBMF_VW_MAX_COUNT];
} obmf_vw_structure_t;

/* RTC Channel Structure */
typedef union {
	uint8_t raw;
	struct {
		uint8_t reserved : 4;
		uint8_t dv       : 3;
		uint8_t uip      : 1;
	} __attribute__((packed)) bits;
} obmf_rtc_rega_reg_t;

typedef union {
	uint8_t raw;
	struct {
		uint8_t dse  : 1;
		uint8_t hf   : 1;
		uint8_t dm   : 1;
		uint8_t sqwe : 1;
		uint8_t uie  : 1;
		uint8_t aie  : 1;
		uint8_t pie  : 1;
		uint8_t set  : 1;
	} __attribute__((packed)) bits;
} obmf_rtc_regb_reg_t;

typedef union {
	uint8_t raw;
	struct {
		uint8_t reserved : 4;
		uint8_t uf       : 1;
		uint8_t af       : 1;
		uint8_t pf       : 1;
		uint8_t irqf     : 1;
	} __attribute__((packed)) bits;
} obmf_rtc_regc_reg_t;

typedef union {
	uint8_t raw;
	struct {
		uint8_t da       : 6;
		uint8_t reserved : 1;
		uint8_t vrt      : 1;
	} __attribute__((packed)) bits;
} obmf_rtc_regd_reg_t;

typedef struct __attribute__((packed)) {
	uint8_t              sec;
	uint8_t              sec_alarm;
	uint8_t              minutes;
	uint8_t              minutes_alarm;
	uint8_t              hours;
	uint8_t              hours_alarm;
	uint8_t              day_of_week;
	uint8_t              day_of_month;
	uint8_t              month;
	uint8_t              year;
	obmf_rtc_rega_reg_t  reg_a;
	obmf_rtc_regb_reg_t  reg_b;
	obmf_rtc_regc_reg_t  reg_c;
	obmf_rtc_regd_reg_t  reg_d;
	uint8_t              reg_e;
} obmf_rtc_structure_t;

/* UART Channel Structure */
typedef union {
	uint8_t raw;
	struct {
		uint8_t wls   : 2;
		uint8_t stb   : 1;
		uint8_t pen   : 1;
		uint8_t eps   : 1;
		uint8_t stkyp : 1;
		uint8_t sb    : 1;
		uint8_t dlab  : 1;
	} __attribute__((packed)) bits;
} obmf_uart_lcr_reg_t;

typedef union {
	uint8_t raw;
	struct {
		uint8_t dtr      : 1;
		uint8_t rts      : 1;
		uint8_t out1     : 1;
		uint8_t out2     : 1;
		uint8_t loop     : 1;
		uint8_t reserved : 3;
	} __attribute__((packed)) bits;
} obmf_uart_mcr_reg_t;

typedef union {
	uint8_t raw;
	struct {
		uint8_t dr    : 1;
		uint8_t oe    : 1;
		uint8_t pe    : 1;
		uint8_t fe    : 1;
		uint8_t bi    : 1;
		uint8_t tdrq  : 1;
		uint8_t temt  : 1;
		uint8_t fifoe : 1;
	} __attribute__((packed)) bits;
} obmf_uart_lsr_reg_t;

typedef union {
	uint8_t raw;
	struct {
		uint8_t dcts : 1;
		uint8_t ddsr : 1;
		uint8_t teri : 1;
		uint8_t ddcd : 1;
		uint8_t cts  : 1;
		uint8_t dsr  : 1;
		uint8_t ri   : 1;
		uint8_t dcd  : 1;
	} __attribute__((packed)) bits;
} obmf_uart_msr_reg_t;

typedef struct __attribute__((packed)) {
	uint8_t             rbr_thr_dll;
	uint8_t             ier_dlh;
	uint8_t             iir_fcr;
	obmf_uart_lcr_reg_t lcr;
	obmf_uart_mcr_reg_t mcr;
	obmf_uart_lsr_reg_t lsr;
	obmf_uart_msr_reg_t msr;
	uint8_t             scr;
} obmf_uart_structure_t;

/* UART region mapped within MMIO space */
#define OBMF_MMIO_UART_BASE_OFFSET      0x10000
#define OBMF_MMIO_UART_RBR_THR          OBMF_MMIO_UART_BASE_OFFSET
#define OBMF_MMIO_UART_LCR              (OBMF_MMIO_UART_BASE_OFFSET + 0x03)
#define OBMF_MMIO_UART_LSR              (OBMF_MMIO_UART_BASE_OFFSET + 0x05)

/* POST_CODE region mapped within MMIO space */
#define OBMF_POST_CODE_REG_COUNT 2
#define OBMF_MMIO_POST_CODE_BASE_OFFSET 0x1C000
#define OBMF_MMIO_POST_CODE_PORT0       OBMF_MMIO_POST_CODE_BASE_OFFSET

typedef struct __attribute__((packed)) {
	uint8_t post_code[OBMF_POST_CODE_REG_COUNT];
} obmf_post_code_structure_t;

/* TPM */
#define OBMF_TPM_SPACE_SIZE 64

/* =====================================================================
 * Interface-Optimized Channel Definitions (v0.9 spec section 9.1.12-9.1.17)
 * ===================================================================== */

/* --- Channel Numbers for interface-optimized channels --- */
#define OBMF_ICP_SERIAL_CHANNEL_0    2
#define OBMF_ICP_IPMI_CHANNEL_NUM    3
/* Second Serial channel (e.g. FPGA console). Placed at 16, past the last
 * base channel (ch15 = I2C Target 1), since ch1-ch15 are all otherwise
 * allocated. */
#define OBMF_ICP_SERIAL_CHANNEL_1    16
#define OBMF_ICP_MEM_DEV_CHANNEL     4  /* Consumer: SMC→BMC (EDAF/eSPI path) */
#define OBMF_ICP_SPI_CHANNEL        17  /* Producer: BMC→SMC (local shd_dev flash) */
#define OBMF_ICP_SMC_GPIO_CHANNEL    5  /* Platform GPIO channel */
#define OBMF_ICP_SGPIO0_CHANNEL      6  /* SGPIO0 GPIO channel */
#define OBMF_ICP_SGPIO1_CHANNEL      7  /* SGPIO1 GPIO channel */
#define OBMF_ICP_VW_GPIO_CHANNEL    8  /* Virtual Wire GPIO channel */
#define OBMF_ICP_IO_CHANNEL         9  /* I/O Port channel */
#define OBMF_ICP_I2C_CONTROLLER_CHANNEL_0 10 /* I2C Controller channel 0 */
#define OBMF_ICP_I2C_CONTROLLER_CHANNEL_1 11 /* I2C Controller channel 1 */
#define OBMF_ICP_I2C_CONTROLLER_CHANNEL_2 12 /* I2C Controller channel 2 */
#define OBMF_ICP_I2C_CONTROLLER_CHANNEL_3 13 /* I2C Controller channel 3 */
#define OBMF_ICP_I2C_TARGET_CHANNEL_0     14 /* I2C Target channel 0 (bus1) */
#define OBMF_ICP_I2C_TARGET_CHANNEL_1     15 /* I2C Target channel 1 (bus2) */

#define OBMF_ICP_I3C_CONTROLLER_CHANNEL   18 /* I3C Controller channel */
#define OBMF_ICP_I3C_CONTROLLER_CHANNEL_1 19 /* Second I3C Controller channel */
#define OBMF_MAX_I3C_CONTROLLER_CHANNELS  2
/* Map an I3C Controller channel number to its 0-based index */
#define OBMF_I3C_CTRL_CHAN_IDX(ch) \
	((ch) == OBMF_ICP_I3C_CONTROLLER_CHANNEL_1 ? 1 : 0)
#define OBMF_MAX_I2C_TARGET_CHANNELS      2  /* Number of I2C Target channels */
/* Map target channel index (0-based) ↔ channel number */
#define OBMF_I2C_TGT_CHAN_NUM(idx) (OBMF_ICP_I2C_TARGET_CHANNEL_0 + (idx))
#define OBMF_I2C_TGT_CHAN_IDX(ch)  ((ch) - OBMF_ICP_I2C_TARGET_CHANNEL_0)

/* Maximum simultaneous Serial channels (ch2, ch16) */
#define OBMF_MAX_SERIAL_CHANNELS  2

/* Map a Serial channel number to its serial_channel_configs[] index. */
#define OBMF_SERIAL_CHAN_IDX(ch) \
	((ch) == OBMF_ICP_SERIAL_CHANNEL_0 ? 0 : \
	 (ch) == OBMF_ICP_SERIAL_CHANNEL_1 ? 1 : 0)

/* Map an I2C Controller channel number to its i2c_channel_configs[] index. */
#define OBMF_I2C_CTRL_CHAN_IDX(ch) \
	((ch) == OBMF_ICP_I2C_CONTROLLER_CHANNEL_0 ? 0 : \
	 (ch) == OBMF_ICP_I2C_CONTROLLER_CHANNEL_1 ? 1 : \
	 (ch) == OBMF_ICP_I2C_CONTROLLER_CHANNEL_2 ? 2 : \
	 (ch) == OBMF_ICP_I2C_CONTROLLER_CHANNEL_3 ? 3 : 0)

/* Maximum simultaneous I2C Controller channels (ch10-ch13) */
#define OBMF_MAX_I2C_CONTROLLER_CHANNELS  4

/* True if 'ch' is any I2C Controller channel */
#define OBMF_IS_I2C_CONTROLLER_CHANNEL(ch) \
	((ch) == OBMF_ICP_I2C_CONTROLLER_CHANNEL_0 || \
	 (ch) == OBMF_ICP_I2C_CONTROLLER_CHANNEL_1 || \
	 (ch) == OBMF_ICP_I2C_CONTROLLER_CHANNEL_2 || \
	 (ch) == OBMF_ICP_I2C_CONTROLLER_CHANNEL_3)

/* SGPIO channels are independent logical chips (each channel index is 0~127). */

/* =====================================================================
 * GPIO Channel (v0.98 spec section 9.1.11)
 * ===================================================================== */

/* GPIO Channel Commands */
#define OBMF_GPIO_CMD_GET_VALUES         0
#define OBMF_GPIO_CMD_SET_VALUES         1
#define OBMF_GPIO_CMD_GET_INT_CONFIG     2
#define OBMF_GPIO_CMD_SET_INT_CONFIG     3
#define OBMF_GPIO_CMD_INT_NOTIFY         4
#define OBMF_GPIO_CMD_INT_NOTIFY_DGRAM   5

/* GPIO Data values (bits [15:12] of each GPIO entry), spec Table 18/19 */
#define OBMF_GPIO_VALUE_LOW     0
#define OBMF_GPIO_VALUE_HIGH    1

/* GPIO Interrupt Config values */
#define OBMF_GPIO_INT_DISABLE        0
#define OBMF_GPIO_INT_LEVEL_LOW      1
#define OBMF_GPIO_INT_LEVEL_HIGH     2
#define OBMF_GPIO_INT_RISING_EDGE    3
#define OBMF_GPIO_INT_FALLING_EDGE   4
#define OBMF_GPIO_INT_BOTH_EDGE      5

/* GPIO indices below this limit can have an interrupt configured */
#define OBMF_GPIO_MAX_INT_INDEX      128

/* GPIO Channel-specific Status Codes */
#define OBMF_GPIO_STATUS_INDEX_NOT_SUPPORTED   0x40
#define OBMF_GPIO_STATUS_INT_NOT_SUPPORTED     0x41
#define OBMF_GPIO_STATUS_INVALID_OPERATION     0x42

/* GPIO_STATUS bits (CHANNEL_SPECIFIC_STATUS of a GPIO channel, Table 67) */
#define OBMF_GPIO_STS_INDEX_ERROR        (1 << 0)
#define OBMF_GPIO_STS_INTERRUPT_PENDING  (1 << 1)

/* GPIO entry: 2 bytes packed as [11:0]=Index, [15:12]=Data */
typedef struct __attribute__((packed)) {
	uint16_t raw;
} obmf_gpio_entry_t;

#define OBMF_GPIO_ENTRY_GET_INDEX(e)  ((e)->raw & 0x0FFF)
#define OBMF_GPIO_ENTRY_GET_DATA(e)   (((e)->raw >> 12) & 0x0F)
#define OBMF_GPIO_ENTRY_SET(e, idx, data) \
	((e)->raw = ((idx) & 0x0FFF) | (((data) & 0x0F) << 12))

/* GPIO Channel Request (spec section 9.1.11.1) */
typedef struct __attribute__((packed)) {
	uint8_t command;           /* Byte 0: Command */
	/* Followed by array of obmf_gpio_entry_t */
} obmf_gpio_request_t;

#define OBMF_GPIO_REQ_HDR_SIZE  sizeof(obmf_gpio_request_t)

/* GPIO Channel Response (spec section 9.1.11.2) */
typedef struct __attribute__((packed)) {
	uint8_t command;           /* Byte 0: Command (echoed) */
	/* For Get Values / Get Interrupt Config: GPIO entry array follows */
} obmf_gpio_response_t;

#define OBMF_GPIO_RSP_HDR_SIZE  sizeof(obmf_gpio_response_t)

/* GPIO Channel Configuration Data (v0.98 spec section 9.2.1) */

/* GPIO Direction values */
#define OBMF_GPIO_DIR_OUTPUT    0x00
#define OBMF_GPIO_DIR_INPUT     0x01

/* GPIO Default Output values */
#define OBMF_GPIO_DEFAULT_LOW   0x00
#define OBMF_GPIO_DEFAULT_HIGH  0x01

/* GPIO Output Drive Config values */
#define OBMF_GPIO_DRIVE_PUSH_PULL    0x00
#define OBMF_GPIO_DRIVE_OPEN_DRAIN   0x01
#define OBMF_GPIO_DRIVE_OPEN_SOURCE  0x02

/* GPIO Output Persist State values */
#define OBMF_GPIO_PERSIST_FALSE  0x00
#define OBMF_GPIO_PERSIST_TRUE   0x01

/* GPIO Bias Pull Config values */
#define OBMF_GPIO_BIAS_NO_PULL    0x00
#define OBMF_GPIO_BIAS_PULL_UP    0x01
#define OBMF_GPIO_BIAS_PULL_DOWN  0x02

/* GPIO Configuration Entry (50 bytes / 0x32 per entry, spec section 9.2.1) */
typedef struct __attribute__((packed)) {
	uint16_t gpio_index;            /* 0x00: Programmable GPIO index (0-4095) */
	char gpio_name[32];             /* 0x02: UTF-8/ASCII NUL-padded name */
	uint8_t gpio_direction;         /* 0x22: 0=Output, 1=Input */
	uint8_t gpio_default_output;    /* 0x23: 0=Low, 1=High */
	uint8_t gpio_output_drive_cfg;  /* 0x24: Push/Pull, Open Drain, Open Source */
	uint8_t gpio_output_persist;    /* 0x25: Persist state across reset */
	uint8_t gpio_bias_pull_cfg;     /* 0x26: No Pull, Pull Up, Pull Down */
	uint16_t gpio_bias_pull_strength; /* 0x27: Units: 16 ohm (0=unspecified) */
	uint16_t gpio_output_drive_strength; /* 0x29: Units: mA (0=unspecified) */
	uint16_t gpio_slew_rate;        /* 0x2B: Units: 0.1ns (0=unspecified) */
	uint16_t gpio_input_debounce;   /* 0x2D: Units: us (0=unspecified) */
	uint8_t reserved[3];            /* 0x2F: Reserved, must be 0 */
} obmf_gpio_config_entry_t;

#define OBMF_GPIO_CONFIG_ENTRY_SIZE  sizeof(obmf_gpio_config_entry_t) /* 50 bytes */
#define OBMF_GPIO_MAX_ENTRIES        128

typedef struct __attribute__((packed)) {
	uint16_t gpio_count;       /* 0x00: Total number of GPIO entries */
	obmf_gpio_config_entry_t entries[OBMF_GPIO_MAX_ENTRIES]; /* 0x02: Array */
} obmf_gpio_config_data_t;

/* Full GPIO Channel Configuration Space */
typedef struct __attribute__((packed)) {
	obmf_channel_config_header_t header;
	obmf_gpio_config_data_t config;
} obmf_gpio_channel_config_t;

/* =====================================================================
 * Serial Channel (v0.9 spec section 9.1.12)
 * ===================================================================== */

/* Serial Channel-specific Status Codes */
#define OBMF_SERIAL_STATUS_LINE_TIMEOUT  0x40

/* Serial Channel Request: Operation/Event bits */
#define OBMF_SERIAL_OP_GENERATE_BREAK    (1 << 0)
#define OBMF_SERIAL_OP_BREAK_DETECTED    (1 << 1)
#define OBMF_SERIAL_OP_TX_OVERRUN        (1 << 4)
#define OBMF_SERIAL_OP_CARRIER_DOWN      (1 << 5)

/* Serial Channel Request (spec section 9.1.12.1) */
typedef struct __attribute__((packed)) {
	uint8_t operation_event;   /* Byte 0: Operation/Event flags */
	uint16_t char_count;       /* Bytes 1-2: Number of characters */
	/* Character Data follows (variable length) */
} obmf_serial_request_t;

#define OBMF_SERIAL_REQ_HDR_SIZE  sizeof(obmf_serial_request_t)

/*
 * An OBMF message carries at most 256 payload bytes, which bounds the
 * characters a single Serial request can carry.
 */
#define OBMF_SERIAL_MAX_CHARS  (256 - OBMF_SERIAL_REQ_HDR_SIZE)

/* Serial Channel Response (spec section 9.1.12.2) */
typedef struct __attribute__((packed)) {
	uint8_t ack_nack;          /* Byte 0: [0]=ACK(0)/NACK(1), [7:1]=Reserved */
	uint16_t accepted_count;   /* Bytes 1-2: Number of characters accepted */
} obmf_serial_response_t;

#define OBMF_SERIAL_RSP_SIZE  sizeof(obmf_serial_response_t)

/* Serial Channel Configuration Data (v0.9 spec section 9.2.1) */
typedef struct __attribute__((packed)) {
	uint32_t uart_speed_hz;    /* 0x00: Baud rate in Hz */
	uint8_t uart_data_bits;    /* 0x04: Number of data bits */
	uint8_t uart_stop_bits;    /* 0x05: Number of stop bits */
	uint8_t uart_line_control; /* 0x06: [3:0]=Parity, [7:4]=Flow control */
} obmf_serial_config_data_t;

/* UART Parity (uart_line_control[3:0]) */
#define OBMF_SERIAL_PARITY_NONE   0
#define OBMF_SERIAL_PARITY_ODD    1
#define OBMF_SERIAL_PARITY_EVEN   2
#define OBMF_SERIAL_PARITY_MARK   3
#define OBMF_SERIAL_PARITY_SPACE  4

/* UART Flow Control (uart_line_control[7:4]) */
#define OBMF_SERIAL_FLOW_NONE     0
#define OBMF_SERIAL_FLOW_XONXOFF  1
#define OBMF_SERIAL_FLOW_RTSCTS   2
#define OBMF_SERIAL_FLOW_DSRDTR   3

/* Full Serial Channel Configuration Space */
typedef struct __attribute__((packed)) {
	obmf_channel_config_header_t header;
	obmf_serial_config_data_t config;
} obmf_serial_channel_config_t;

/* =====================================================================
 * IPMI Channel (v0.9 spec section 9.1.16)
 * ===================================================================== */

/* IPMI Channel Commands */
#define OBMF_IPMI_CMD_SEND_MESSAGE  0x00

/* IPMI Channel Request (spec section 9.1.16.1) */
typedef struct __attribute__((packed)) {
	uint8_t command;           /* Byte 0: Command (0=Send IPMI Message) */
	uint8_t netfn_lun;         /* Byte 1: NetFn/LUN per IPMI spec */
	/* IPMI Command + Payload/Completion Code follows */
} obmf_ipmi_request_t;

#define OBMF_IPMI_REQ_HDR_SIZE  sizeof(obmf_ipmi_request_t)

/* IPMI Channel Response (spec section 9.1.16.2) */
typedef struct __attribute__((packed)) {
	uint8_t command;           /* Byte 0: Command */
	uint8_t netfn_lun;         /* Byte 1: NetFn/LUN */
	/* IPMI Command + Payload/Completion Code follows */
} obmf_ipmi_response_t;

#define OBMF_IPMI_RSP_HDR_SIZE  sizeof(obmf_ipmi_response_t)

/* Full IPMI Channel Configuration Space (no channel-specific config data) */
typedef struct __attribute__((packed)) {
	obmf_channel_config_header_t header;
	/* No CONFIGURATION_DATA for IPMI */
} obmf_ipmi_channel_config_t;

/* =====================================================================
 * SPI Controller Channel (v0.9 spec section 9.1.17)
 * ===================================================================== */

/* SPI Commands [3:0] */
#define OBMF_SPI_CMD_READ        1
#define OBMF_SPI_CMD_WRITE       2
#define OBMF_SPI_CMD_WRITE_READ  3
#define OBMF_SPI_CMD_POSTED_WRITE 4

/* SPI CS Control bits */
#define OBMF_SPI_CS_ASSERT_BEFORE  (1 << 5)
#define OBMF_SPI_CS_DEASSERT_AFTER (1 << 4)

/* SPI Command byte macros */
#define OBMF_SPI_CMD_GET_CS(cmd)        (((cmd) >> 6) & 0x03)
#define OBMF_SPI_CMD_GET_CS_CTRL(cmd)   (((cmd) >> 4) & 0x03)
#define OBMF_SPI_CMD_GET_COMMAND(cmd)   ((cmd) & 0x0F)
#define OBMF_SPI_CMD_BUILD(cs, assert, deassert, spi_cmd) \
	((((cs) & 0x03) << 6) | \
	 (((assert) ? 1 : 0) << 5) | \
	 (((deassert) ? 1 : 0) << 4) | \
	 ((spi_cmd) & 0x0F))

/* SPI Controller Channel Request (spec section 9.1.17.1) */
typedef struct __attribute__((packed)) {
	uint8_t command;           /* Byte 0: [7:6]=CS, [5:4]=CS ctrl, [3:0]=cmd */
	uint16_t write_data_size;  /* Bytes 1-2: Size of write data */
	uint16_t read_data_size;   /* Bytes 3-4: Size of read data */
	/* Write Data follows (variable length) */
} obmf_spi_request_t;

#define OBMF_SPI_REQ_HDR_SIZE  sizeof(obmf_spi_request_t)

/* SPI Controller Channel Response (spec section 4.10.2, no status in payload) */
typedef struct __attribute__((packed)) {
	uint8_t command;           /* Byte 0: [7:4]=Reserved, [3:0]=SPI cmd */
	uint16_t read_data_size;   /* Bytes 1-2: Size of read data */
	/* Read Data follows (variable length) */
} obmf_spi_response_t;

#define OBMF_SPI_RSP_HDR_SIZE  sizeof(obmf_spi_response_t)

/* SPI Supported Config Flags (section 9.2.1 - SPI Controller Channel) */
#define OBMF_SPI_FLAG_CPHA           (1 << 0)
#define OBMF_SPI_FLAG_CPOL           (1 << 1)
#define OBMF_SPI_FLAG_CS_HIGH        (1 << 2)
#define OBMF_SPI_FLAG_LSB_FIRST      (1 << 3)
#define OBMF_SPI_FLAG_3WIRE          (1 << 4)
#define OBMF_SPI_FLAG_LOOP           (1 << 5)
#define OBMF_SPI_FLAG_NO_CS          (1 << 6)
#define OBMF_SPI_FLAG_READY          (1 << 7)
#define OBMF_SPI_FLAG_TX_DUAL        (1 << 8)
#define OBMF_SPI_FLAG_TX_QUAD        (1 << 9)
#define OBMF_SPI_FLAG_RX_DUAL        (1 << 10)
#define OBMF_SPI_FLAG_RX_QUAD        (1 << 11)

/* SPI Controller Channel Configuration Data */
typedef struct __attribute__((packed)) {
	uint32_t supported_config_flags; /* 0x00: Supported flags (read-only) */
	uint32_t config_flags;           /* 0x04: Active config flags (rw) */
	uint8_t num_devices;             /* 0x08: Number of SPI devices/CS */
	uint8_t reserved[3];             /* 0x09: Reserved */
	uint32_t rd_max_speed_hz;        /* 0x0C: Max SPI read clock (Hz) */
	uint32_t wr_max_speed_hz;        /* 0x10: Max SPI write clock (Hz) */
} obmf_spi_config_data_t;

/* Full SPI Channel Configuration Space */
typedef struct __attribute__((packed)) {
	obmf_channel_config_header_t header;
	obmf_spi_config_data_t config;
} obmf_spi_channel_config_t;

/* =====================================================================
 * Memory Device Channel (v1.0.0 RC1 spec section 4.12)
 *
 * Consumer role (ch4 / OBMF_ICP_MEM_DEV_CHANNEL): we send byte-addressable
 * read/write/erase requests to the BMC's exposed storage device (eSPI
 * flash access path).
 * ===================================================================== */

/* Memory Device Channel Commands (Command byte [1:0]) */
#define OBMF_MEM_DEV_CMD_READ   0
#define OBMF_MEM_DEV_CMD_WRITE  1
#define OBMF_MEM_DEV_CMD_ERASE  2

#define OBMF_MEM_DEV_GET_CMD(b)  ((b) & 0x03)

/* Memory Device Channel-specific Status Codes (spec section 4.12) */
#define OBMF_MEM_DEV_STATUS_ACCESS_DENIED       0x40
#define OBMF_MEM_DEV_STATUS_SIZE_UNSUPPORTED    0x41
#define OBMF_MEM_DEV_STATUS_ALIGNMENT           0x42
#define OBMF_MEM_DEV_STATUS_WRITE_PROTECTED     0x43

/* MEM_DEV_STATUS bits (channel-specific status register) */
#define OBMF_MEM_DEV_STS_WRITE_PROTECTED  (1 << 0)
#define OBMF_MEM_DEV_STS_MEDIA_ERROR      (1 << 1)

/* MEM_DEV_CONTROL bits (channel-specific control register) */
#define OBMF_MEM_DEV_CTRL_CLEAR_STATUS    (1 << 0)

/* MEM_DEV_CFG CAPABILITIES bits */
#define OBMF_MEM_DEV_CAP_WRITEABLE  (1 << 0)
#define OBMF_MEM_DEV_CAP_NO_ERASE   (1 << 1)

/* Memory Device Channel Request (spec section 4.12.1) */
typedef struct __attribute__((packed)) {
	uint8_t command;      /* Byte 0: [1:0]=command, [7:2]=Reserved */
	uint32_t address;     /* Bytes 1-4: 32-bit byte offset (LE) */
	uint16_t data_size;   /* Bytes 5-6: Read/Write=bytes, Erase=KB */
	/* Write Data follows (variable length, Memory Write only) */
} obmf_mem_dev_request_t;

#define OBMF_MEM_DEV_REQ_HDR_SIZE  sizeof(obmf_mem_dev_request_t)

/* Memory Device Channel Response (spec section 4.12.2) */
typedef struct __attribute__((packed)) {
	uint8_t status;        /* Byte 0: Completion status */
	/* Read Data follows (variable length, Memory Read success only) */
} obmf_mem_dev_response_t;

#define OBMF_MEM_DEV_RSP_HDR_SIZE  sizeof(obmf_mem_dev_response_t)

/* Memory Device Channel Configuration Data (spec "MEM_DEV_CFG") */
typedef struct __attribute__((packed)) {
	uint32_t capabilities;  /* 0x00: [0]=WRITEABLE, [1]=NO_ERASE */
	uint32_t device_size;   /* 0x04: Total addressable size in bytes */
	uint16_t erase_size;    /* 0x08: Erase block size in KB (power of two) */
	uint16_t read_size;     /* 0x0A: Max single Read request size in bytes */
	uint16_t write_size;    /* 0x0C: Max single Write request size in bytes */
	uint16_t reserved;      /* 0x0E: Reserved */
} obmf_mem_dev_config_data_t;

/* Full Memory Device Channel Configuration Space */
typedef struct __attribute__((packed)) {
	obmf_channel_config_header_t header;
	obmf_mem_dev_config_data_t config;
} obmf_mem_dev_channel_config_t;

/* =====================================================================
 * I/O Port Channel (v0.9.2 spec section 4.31)
 * ===================================================================== */

/* I/O Range Service Types */
#define OBMF_IO_SERVICE_RESERVED    0
#define OBMF_IO_SERVICE_KCS         1
#define OBMF_IO_SERVICE_BT          2
#define OBMF_IO_SERVICE_RTC         3
#define OBMF_IO_SERVICE_POST_CODE   4
#define OBMF_IO_SERVICE_SUPER_IO    5
#define OBMF_IO_SERVICE_COMA        6
#define OBMF_IO_SERVICE_COMB        7
#define OBMF_IO_SERVICE_I8042       8
#define OBMF_IO_SERVICE_MCU1        9
#define OBMF_IO_SERVICE_MCU2        10

/* I/O POST Code port and capture size */
#define OBMF_IO_POST_CODE_PORT   0x0080
#define OBMF_IO_POST_CODE_SIZE   4

/* I/O Range flags */
#define OBMF_IO_RANGE_ENABLE  (1 << 0)

/* Maximum number of I/O ranges to advertise in config */
#define OBMF_IO_MAX_RANGES  4

/**
 * @brief I/O Channel Configuration Data header (spec IO Channel Config Data)
 *
 * TRANSACTION_TYPES_SUPPORTED: 16-bit bitmap, bit N = transaction type N supported.
 * IO_RANGE_COUNT: number of IO_RANGE_CFG entries that follow.
 */
typedef struct __attribute__((packed)) {
	uint16_t trans_types_supported; /* 0x00: Bitmap of supported transaction types */
	uint16_t io_range_count;        /* 0x02: Number of IO_RANGE_CFG entries */
} obmf_io_config_hdr_t;

/**
 * @brief I/O Range Configuration Entry (10 bytes per spec)
 */
typedef struct __attribute__((packed)) {
	uint8_t  range_flags;   /* 0x00: [0]=RANGE_ENABLE */
	uint8_t  reserved;      /* 0x01: Reserved */
	uint16_t range_start;   /* 0x02: First port number in range */
	uint16_t range_end;     /* 0x04: Last port number in range */
	uint16_t range_mask;    /* 0x06: Address mask (0=exact match) */
	uint16_t service_type;  /* 0x08: Service type (POST_CODE, KCS, etc.) */
} obmf_io_range_cfg_t;

/**
 * @brief I/O Channel Configuration Data (header + range array)
 */
typedef struct __attribute__((packed)) {
	obmf_io_config_hdr_t  hdr;
	obmf_io_range_cfg_t   ranges[OBMF_IO_MAX_RANGES];
} obmf_io_config_data_t;

/* Full I/O Channel Configuration Space */
typedef struct __attribute__((packed)) {
	obmf_channel_config_header_t header;
	obmf_io_config_data_t config;
} obmf_io_channel_config_t;

/* =====================================================================
 * I2C Controller Channel (v1.0.0 RC1 spec section 4.6)
 *
 * Producer side (this device): the OBMF Consumer (host/BMC) sends I2C
 * read/write/SMBus requests, we drive the physical I2C bus (hal_i2c) and
 * return a response.
 * ===================================================================== */

/* I2C Controller Channel Commands (Command byte [6:0]) */
#define OBMF_I2C_CMD_READ               0
#define OBMF_I2C_CMD_WRITE              1
#define OBMF_I2C_CMD_SMBUS_BLOCK_READ   2
#define OBMF_I2C_CMD_SMBUS_WRITE_READ   3
#define OBMF_I2C_CMD_SMBUS_HOST_NOTIFY  4

/* Send STOP control (Command byte [7]) */
#define OBMF_I2C_STOP_SEND      0  /* Send STOP between transactions */
#define OBMF_I2C_STOP_NONE      1  /* Do not send STOP (hold the bus) */

/* I2C Command byte helpers: [6:0]=command, [7]=Send STOP control */
#define OBMF_I2C_CMD_GET_COMMAND(b)  ((b) & 0x7F)
#define OBMF_I2C_CMD_GET_STOP(b)     (((b) >> 7) & 0x01)
#define OBMF_I2C_CMD_BUILD(cmd, no_stop) \
	(((cmd) & 0x7F) | (((no_stop) ? 1 : 0) << 7))

/* I2C Controller Channel-specific Status Codes (spec section 4.18) */
#define OBMF_I2C_STATUS_TIMEOUT          0x40
#define OBMF_I2C_STATUS_ARBITRATION_LOST 0x41
#define OBMF_I2C_STATUS_BUS_BUSY         0x42
#define OBMF_I2C_STATUS_NACK             0x43

/* I2C Controller Channel Request (spec section 4.6.1) */
typedef struct __attribute__((packed)) {
	uint8_t command;       /* Byte 0: [6:0]=command, [7]=Send STOP */
	uint8_t address;       /* Byte 1: I2C/SMBus target address */
	uint16_t read_length;  /* Bytes 2-3: Size of read data (0 for write) */
	/* Write Data follows (variable length) */
} obmf_i2c_request_t;

#define OBMF_I2C_REQ_HDR_SIZE  sizeof(obmf_i2c_request_t)

/* I2C Controller Channel Response (spec section 4.6.2) */
typedef struct __attribute__((packed)) {
	uint8_t command;       /* Byte 0: [6:0]=command, [7]=Reserved=0 */
	uint16_t read_length;  /* Bytes 1-2: Size of read data */
	uint8_t reserved;      /* Byte 3: Reserved */
	/* Read Data follows at Byte 4 (variable length) */
} obmf_i2c_response_t;

#define OBMF_I2C_RSP_HDR_SIZE  sizeof(obmf_i2c_response_t)

/* I2C bus speed enum (used by Controller/Target Configuration Data) */
#define OBMF_I2C_SPEED_100KHZ   0
#define OBMF_I2C_SPEED_400KHZ   1
#define OBMF_I2C_SPEED_1MHZ     2
#define OBMF_I2C_SPEED_3P4MHZ   3

/* I2C_CONTROLLER_OPTIONS bits (config data offset 0x01) */
#define OBMF_I2C_OPT_BUS_RECOVERY        (1 << 0)
#define OBMF_I2C_OPT_AUTO_BUS_RECOVERY   (1 << 1)
#define OBMF_I2C_OPT_BUS_OVERRIDE        (1 << 2)

/* I2C Controller Channel Configuration Data (spec "Channel Configuration Data") */
typedef struct __attribute__((packed)) {
	uint8_t controller_speed;   /* 0x00: I2C_CONTROLLER_SPEED enum */
	uint8_t controller_options; /* 0x01: I2C_CONTROLLER_OPTIONS bits */
} obmf_i2c_config_data_t;

/* Full I2C Controller Channel Configuration Space */
typedef struct __attribute__((packed)) {
	obmf_channel_config_header_t header;
	obmf_i2c_config_data_t config;
} obmf_i2c_channel_config_t;

/* =====================================================================
 * I2C Target Channel (v1.0.0 RC1 spec section 4.7)
 *
 * Consumer side (this device): when this device is addressed as an I2C
 * target (e.g. SSIF) we forward the received write to the OBMF Producer
 * (host/BMC) and wait for its acknowledging response.  Read is not
 * supported in this version of the spec.
 * ===================================================================== */

/* I2C Target Channel Commands (Command byte [6:0]) */
#define OBMF_I2C_TARGET_CMD_WRITE  1

/* I2C Target Channel-specific Status Codes (spec section 4.21) */
#define OBMF_I2C_TARGET_STATUS_TRANSACTION  0x40

/* I2C Target Channel Request (spec section 4.22) */
typedef struct __attribute__((packed)) {
	uint8_t command;       /* Byte 0: [6:0]=command (1=I2C Write), [7]=Rsvd */
	uint8_t address;       /* Byte 1: I2C/SMBus target address */
	uint16_t reserved;     /* Bytes 2-3: Reserved */
	/* Write Data follows (variable length) */
} obmf_i2c_target_request_t;

#define OBMF_I2C_TARGET_REQ_HDR_SIZE  sizeof(obmf_i2c_target_request_t)

/* I2C Target Channel Response (spec section 4.23) */
typedef struct __attribute__((packed)) {
	uint8_t command;       /* Byte 0: [6:0]=command, [7]=Reserved */
} obmf_i2c_target_response_t;

#define OBMF_I2C_TARGET_RSP_SIZE  sizeof(obmf_i2c_target_response_t)

/* Maximum configurable I2C target address entries advertised in config */
#define OBMF_I2C_TARGET_MAX_ADDRS  4

/* I2C Target Channel Configuration Data (spec "Channel Configuration Data") */
typedef struct __attribute__((packed)) {
	uint8_t target_speed;       /* 0x00: I2C_TARGET_SPEED enum */
	uint8_t reserved;           /* 0x01: RESERVED */
	uint8_t target_address_count; /* 0x02: I2C_TARGET_ADDRESS_COUNT */
	uint8_t target_address_list[OBMF_I2C_TARGET_MAX_ADDRS]; /* 0x03: list */
} obmf_i2c_target_config_data_t;

/* Full I2C Target Channel Configuration Space */
typedef struct __attribute__((packed)) {
	obmf_channel_config_header_t header;
	obmf_i2c_target_config_data_t config;
} obmf_i2c_target_channel_config_t;

/* =====================================================================
 * I3C Controller Channel (v1.0.0 RC1 spec section 4.8)
 * ===================================================================== */

/* I3C Controller Channel Commands (Consumer-initiated) */
#define OBMF_I3C_CMD_EXECUTE_SEQUENCE    0 /* Chained SDR/CCC operations */
#define OBMF_I3C_CMD_DO_DAA              1 /* ENTDAA sequence */
#define OBMF_I3C_CMD_BUS_RECOVERY        2 /* Reset/recovery pattern */
#define OBMF_I3C_CMD_SET_ASSOCIATED_I2C  3 /* Associate I2C channel */
#define OBMF_I3C_CMD_GET_STATS           4 /* Retrieve error/event counters */

/* I3C Producer-initiated Events */
#define OBMF_I3C_EVT_IBI_NOTIFICATION    5
#define OBMF_I3C_EVT_HOTJOIN_NOTIFICATION 6
#define OBMF_I3C_EVT_BUS_ERROR           7

/* I3C Controller Channel-specific Status Codes (0x40-0x7F) */
#define OBMF_I3C_STATUS_BUS_TIMEOUT       0x40
#define OBMF_I3C_STATUS_ARB_LOST          0x41
#define OBMF_I3C_STATUS_BUS_BUSY          0x42
#define OBMF_I3C_STATUS_NACK              0x43
#define OBMF_I3C_STATUS_BUS_FRAMING       0x44
#define OBMF_I3C_STATUS_PARITY            0x45
#define OBMF_I3C_STATUS_ILLEGAL_CCC_REPLY 0x46

/* I3C Execute Sequence operation types */
#define OBMF_I3C_OP_CCC           0 /* CCC transfer */
#define OBMF_I3C_OP_CCC_W_DB      1 /* CCC with defining byte */
#define OBMF_I3C_OP_PRIVATE_SDR   2 /* Private SDR transfer */

/* I3C Bus Recovery action types */
#define OBMF_I3C_RECOVERY_SCL_LOW       0
#define OBMF_I3C_RECOVERY_TARGET_RESET  1
#define OBMF_I3C_RECOVERY_RSTACT_RESET  2
#define OBMF_I3C_RECOVERY_HDR_EXIT      3

/* I3C per-operation result status codes */
#define OBMF_I3C_OP_SUCCESS    0
#define OBMF_I3C_OP_NACK       1
#define OBMF_I3C_OP_ARB_LOST   2
#define OBMF_I3C_OP_TIMEOUT    3
#define OBMF_I3C_OP_BUS_ERR    4

/* I3C Feature Flags (config data offset 0x02) */
#define OBMF_I3C_FEATURE_IBI_PRIV_READ  (1 << 0) /* IBI private read support */

/* I3C broadcast address */
#define OBMF_I3C_BROADCAST_ADDR  0x7E

/**
 * @brief I3C Controller Channel request fixed header (6 bytes).
 *
 * For Execute Sequence the operations array starts immediately after
 * parameter_1 (byte 2).  parameter_2 and parameter_3 are used only by
 * DAA, Bus Recovery, Set Associated I2C, IBI/HotJoin/BusError commands.
 */
typedef struct __attribute__((packed)) {
	uint8_t  command;     /* Byte 0: Command type */
	uint8_t  parameter_1; /* Byte 1: num_ops / excluded_addrs / action_type */
	uint16_t parameter_2; /* Bytes 2-3: scl_od_khz / i2c_channel / payload_len */
	uint16_t parameter_3; /* Bytes 4-5: scl_pp_khz / scl_low_us / pending_read */
} obmf_i3c_request_hdr_t;

#define OBMF_I3C_REQ_HDR_SIZE  sizeof(obmf_i3c_request_hdr_t) /* 6 bytes */

/**
 * @brief I3C Controller Channel response fixed header (2 bytes).
 */
typedef struct __attribute__((packed)) {
	uint8_t command;      /* Byte 0: Command (echoed) */
	uint8_t result_count; /* Byte 1: num ops done / discovered devs / counters */
} obmf_i3c_response_t;

#define OBMF_I3C_RSP_HDR_SIZE  sizeof(obmf_i3c_response_t) /* 2 bytes */

/**
 * @brief Execute Sequence per-operation header (8 bytes).
 */
typedef struct __attribute__((packed)) {
	uint8_t  op_type;           /* 0=CCC, 1=CCC+DefByte, 2=Private SDR */
	uint8_t  send_stop;         /* 1=STOP after transfer, 0=Repeated-Start */
	uint16_t target_scl_od_khz; /* Open-Drain SCL in kHz (0=default) */
	uint16_t target_scl_pp_khz; /* Push-Pull SCL in kHz (0=default) */
	uint16_t descriptor_len;    /* Bytes of descriptor payload that follow */
} obmf_i3c_op_hdr_t;

#define OBMF_I3C_OP_HDR_SIZE  sizeof(obmf_i3c_op_hdr_t) /* 8 bytes */

/**
 * @brief CCC operation descriptor header (5 bytes + variable write_data).
 */
typedef struct __attribute__((packed)) {
	uint8_t  ccc_id;       /* CCC ID (e.g. 0x06=RSTACT, 0x87=SETDASA) */
	uint8_t  dynamic_addr; /* Target addr (OBMF_I3C_BROADCAST_ADDR for broadcast) */
	uint8_t  is_write;     /* 1=Write, 0=Read */
	uint16_t data_len;     /* Bytes to transfer */
	/* write_data[data_len] follows if is_write=1 */
} obmf_i3c_ccc_op_hdr_t;

#define OBMF_I3C_CCC_OP_HDR_SIZE  sizeof(obmf_i3c_ccc_op_hdr_t) /* 5 bytes */

/**
 * @brief CCC with Defining Byte operation descriptor header (6 bytes + variable).
 */
typedef struct __attribute__((packed)) {
	uint8_t  ccc_id;        /* CCC ID */
	uint8_t  defining_byte; /* CCC sub-command defining byte */
	uint8_t  dynamic_addr;  /* Target addr */
	uint8_t  is_write;      /* 1=Write, 0=Read */
	uint16_t data_len;      /* Bytes to transfer */
	/* write_data[data_len] follows if is_write=1 */
} obmf_i3c_ccc_w_db_op_hdr_t;

#define OBMF_I3C_CCC_W_DB_OP_HDR_SIZE  sizeof(obmf_i3c_ccc_w_db_op_hdr_t) /* 6 bytes */

/**
 * @brief Private SDR operation descriptor header (4 bytes + variable write_data).
 */
typedef struct __attribute__((packed)) {
	uint8_t  dynamic_addr; /* Target 7-bit dynamic address */
	uint8_t  is_write;     /* 1=Write, 0=Read */
	uint16_t data_len;     /* Bytes to transfer */
	/* write_data[data_len] follows if is_write=1 */
} obmf_i3c_sdr_op_hdr_t;

#define OBMF_I3C_SDR_OP_HDR_SIZE  sizeof(obmf_i3c_sdr_op_hdr_t) /* 4 bytes */

/**
 * @brief Execute Sequence per-operation result descriptor (3 bytes + read_data).
 */
typedef struct __attribute__((packed)) {
	uint8_t  op_status;  /* OBMF_I3C_OP_* result code */
	uint16_t actual_len; /* Actual bytes transferred */
	/* read_data[actual_len] follows for read ops with actual_len > 0 */
} obmf_i3c_op_result_t;

#define OBMF_I3C_OP_RESULT_SIZE  sizeof(obmf_i3c_op_result_t) /* 3 bytes */

/**
 * @brief DAA discovered device structure (9 bytes per device).
 */
typedef struct __attribute__((packed)) {
	uint8_t dynamic_addr; /* Assigned 7-bit dynamic address */
	uint8_t pid[6];       /* 48-bit Provisioned ID (MSB first) */
	uint8_t bcr;          /* Bus Characteristics Register */
	uint8_t dcr;          /* Device Characteristics Register */
} obmf_i3c_daa_dev_t;

/**
 * @brief Stats counter entry (3 bytes).
 */
typedef struct __attribute__((packed)) {
	uint8_t  counter_type;  /* Counter type (see spec section 4.8.2) */
	uint16_t counter_value; /* 16-bit unsigned counter value */
} obmf_i3c_stats_entry_t;

/**
 * @brief I3C Controller Channel Configuration Data (spec section 9.2.1).
 *
 * Uses the existing v0.9 obmf_channel_config_header_t unchanged.
 */
typedef struct __attribute__((packed)) {
	uint16_t dev_nack_retry_count;  /* 0x00: Configured NACK retry count (rw) */
	uint8_t  i3c_feature_flags;     /* 0x02: Feature flags (r) */
	uint8_t  max_chained_ops;       /* 0x03: Max ops per Execute Sequence (r) */
	uint16_t max_scl_pp_khz;        /* 0x04: Max push-pull clock in kHz (r) */
	uint16_t max_scl_od_khz;        /* 0x06: Max open-drain clock in kHz (r) */
	uint16_t max_read_turnaround;   /* 0x08: Max read turnaround in µs (r) */
	uint16_t max_dev_nack_retry;    /* 0x0A: Max NACK retry count (r) */
	uint16_t max_payload_size;      /* 0x0C: Max payload per command in bytes (r) */
	uint16_t max_ibi_payload_size;  /* 0x0E: Max IBI payload in bytes (r) */
} obmf_i3c_config_data_t;

/* Full I3C Controller Channel Configuration Space */
typedef struct __attribute__((packed)) {
	obmf_channel_config_header_t header;
	obmf_i3c_config_data_t config;
} obmf_i3c_channel_config_t;

typedef struct __attribute__((packed)) {
	obmf_ch0_base_t base;
	uint32_t channel_offsets[OBMF_MAX_CHANNELS];
	/* Channel configuration spaces */
	obmf_mmio_channel_config_t mmio_channel_config;
	obmf_serial_channel_config_t serial_channel_configs[OBMF_MAX_SERIAL_CHANNELS];
	obmf_ipmi_channel_config_t ipmi_channel_config;
	obmf_spi_channel_config_t spi_channel_config;       /* ch17: SPI Controller */
	obmf_gpio_channel_config_t gpio_channel_config;     /* ch8: VW GPIO */
	obmf_gpio_channel_config_t sgpio1_channel_config;   /* ch7: SGPIO1 */
	obmf_gpio_channel_config_t smc_gpio_channel_config; /* ch5: Platform GPIO */
	obmf_gpio_channel_config_t sgpio0_channel_config;   /* ch6: SGPIO0 */
	obmf_io_channel_config_t   io_channel_config;       /* ch9: I/O Port */
	/* ch10-ch13: I2C Controller 0-3 (index = slot in OBMF_IS_I2C_CONTROLLER_CHANNEL order) */
	obmf_i2c_channel_config_t        i2c_channel_configs[OBMF_MAX_I2C_CONTROLLER_CHANNELS];
	obmf_i2c_target_channel_config_t i2c_target_channel_configs[OBMF_MAX_I2C_TARGET_CHANNELS]; /* ch14-ch15: I2C Target */
	obmf_i3c_channel_config_t        i3c_channel_config;        /* ch17: I3C Controller */
	obmf_mem_dev_channel_config_t    mem_dev_channel_config;    /* ch4: Memory Device */
	obmf_i3c_channel_config_t        i3c1_channel_config;       /* ch19: I3C Controller 1 */
} obmf_discovery_structure_t;

/* Offsets within discovery struct for each channel config */
#define CH0_MMIO_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, mmio_channel_config)
#define CH0_SERIAL_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, serial_channel_configs[0])
#define CH0_IPMI_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, ipmi_channel_config)
#define CH0_SPI_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, spi_channel_config)
#define CH0_GPIO_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, gpio_channel_config)
#define CH0_SGPIO1_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, sgpio1_channel_config)
#define CH0_SMC_GPIO_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, smc_gpio_channel_config)
#define CH0_SGPIO0_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, sgpio0_channel_config)
#define CH0_IO_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, io_channel_config)
#define CH0_I2C_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, i2c_channel_configs[0])
#define CH0_I2C1_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, i2c_channel_configs[1])
#define CH0_I2C2_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, i2c_channel_configs[2])
#define CH0_I2C3_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, i2c_channel_configs[3])
#define CH0_I2C_TARGET_CHAN_CONFIG_OFFSET(idx) \
	offsetof(obmf_discovery_structure_t, i2c_target_channel_configs[(idx)])
#define CH0_SERIAL_CHAN_1_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, serial_channel_configs[1])
#define CH0_I3C_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, i3c_channel_config)
#define CH0_MEM_DEV_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, mem_dev_channel_config)
#define CH0_I3C1_CHAN_CONFIG_OFFSET \
	offsetof(obmf_discovery_structure_t, i3c1_channel_config)

/* =====================================================================
 * Callback types for interface-optimized channels
 * ===================================================================== 
 */

/**
 * @brief Serial channel response callback - called when serial response arrives.
 * @param ack_nack 0x00 = ACK (all accepted), 0x01 = NACK
 * @param accepted_count Number of characters accepted by remote
 * @param ctx User context
 */
typedef void (*obmf_serial_rsp_cb_t)(uint8_t ack_nack, uint16_t accepted_count,
				     void *ctx);

/**
 * @brief IPMI channel response callback - called when IPMI response arrives.
 * @param netfn_lun Response NetFn/LUN byte
 * @param cmd IPMI command byte (echoed)
 * @param cc IPMI completion code
 * @param data Response data (after completion code)
 * @param data_len Length of response data
 * @param ctx User context
 */
typedef void (*obmf_ipmi_rsp_cb_t)(uint8_t netfn_lun, uint8_t cmd,
				   uint8_t cc,
				   const uint8_t *data, uint16_t data_len,
				   void *ctx);

/**
 * @brief I2C target channel response callback (Consumer side).
 * @param status 0x00 = success, non-zero = error (channel status code)
 * @param command Echoed I2C command [6:0]
 * @param ctx User context
 */
typedef void (*obmf_i2c_target_rsp_cb_t)(uint8_t status, uint8_t command,
					 void *ctx);

/* =====================================================================
 * Platform channel configuration
 *
 * The common service implements the OBMF-ICP protocol; the platform decides
 * which channels exist, what they are called, how they are configured and
 * which local bus backs them.  A platform describes its channels in a table
 * and hands it to obmf_service_init():
 *
 *   static const struct obmf_plat_channel plat_obmf_channels[] = {
 *       OBMF_CHANNEL_MMIO("MMIO", 64, 64),
 *       OBMF_CHANNEL_SERIAL(OBMF_ICP_SERIAL_CHANNEL_0, "Serial", 115200),
 *       OBMF_CHANNEL_I2C(OBMF_ICP_I2C_CONTROLLER_CHANNEL_0, "I2C0",
 *                        I2C_BUS1, OBMF_I2C_SPEED_100KHZ),
 *       OBMF_CHANNEL_I3C(OBMF_ICP_I3C_CONTROLLER_CHANNEL, "I3C0", 4),
 *   };
 *
 *   static const struct obmf_plat_config plat_obmf_config =
 *       OBMF_PLAT_CONFIG("OpenBIC-OBMF", plat_obmf_channels);
 *
 *   obmf_service_init(&plat_obmf_config);
 *
 * The channel number selects the channel type (see OBMF_ICP_*_CHANNEL).
 * Channels that are not listed are not advertised to the host: their
 * Channel 0 offset is 0 and requests to them fail with UNKNOWN_CHANNEL.
 * ===================================================================== */

/** Maximum channel name length (the 16-byte field is NUL-padded). */
#define OBMF_CHANNEL_NAME_MAX 15

/**
 * @brief One channel provided by the platform.
 *
 * Only the member of @ref cfg matching the channel type is used.  Channel
 * types without configuration data (IPMI) ignore it.
 */
struct obmf_plat_channel {
	uint8_t channel;  /* OBMF_ICP_*_CHANNEL */
	const char *name; /* Advertised name, at most OBMF_CHANNEL_NAME_MAX chars */
	uint8_t bus;      /* I2C/I3C controller channels: local bus number */
	union {
		obmf_mmio_config_data_t mmio;
		obmf_serial_config_data_t serial;
		obmf_spi_config_data_t spi;
		obmf_mem_dev_config_data_t mem_dev;
		struct {
			uint16_t count; /* Number of GPIOs exposed */
		} gpio;
		obmf_io_config_data_t io; /* io_range_count 0 = count enabled ranges */
		obmf_i2c_config_data_t i2c;
		struct {
			uint8_t speed;  /* OBMF_I2C_SPEED_* */
		} i2c_target;
		obmf_i3c_config_data_t i3c;
	} cfg;
};

/** @brief Platform description passed to obmf_service_init(). */
struct obmf_plat_config {
	const char *device_name; /* At most OBMF_DEVICE_NAME_SIZE - 1 chars */
	uint16_t vendor_id;      /* PCI-SIG vendor ID, 0xFFFF = none */
	uint16_t device_id;
	const struct obmf_plat_channel *channels;
	uint8_t num_channels;
};

#define OBMF_PLAT_CONFIG(_name, _channels)                                     \
	{                                                                      \
		.device_name = (_name), .vendor_id = 0xFFFF, .device_id = 0x0001, \
		.channels = (_channels),                                       \
		.num_channels = sizeof(_channels) / sizeof((_channels)[0]),    \
	}

/* --- Helpers for the channel table, one per channel type --- */

/** MMIO channel (always channel 1); max read/write sizes in bytes. */
#define OBMF_CHANNEL_MMIO(_name, _max_rd, _max_wr)                             \
	{ .channel = OBMF_ICP_MMIO_CHANNEL, .name = (_name),                   \
	  .cfg.mmio = { .max_mmio_read_size = (_max_rd),                       \
			.max_mmio_write_size = (_max_wr) } }

/** Serial channel, 8 data bits, 1 stop bit, no parity. */
#define OBMF_CHANNEL_SERIAL(_ch, _name, _baud)                                 \
	{ .channel = (_ch), .name = (_name),                                   \
	  .cfg.serial = { .uart_speed_hz = (_baud), .uart_data_bits = 8,       \
			  .uart_stop_bits = 1,                                 \
			  .uart_line_control = OBMF_SERIAL_PARITY_NONE } }

/** IPMI channel (no configuration data). */
#define OBMF_CHANNEL_IPMI(_name)                                               \
	{ .channel = OBMF_ICP_IPMI_CHANNEL_NUM, .name = (_name) }

/** SPI controller channel; flags are OBMF_SPI_FLAG_*, clocks in Hz. */
#define OBMF_CHANNEL_SPI(_name, _flags, _num_dev, _rd_hz, _wr_hz)              \
	{ .channel = OBMF_ICP_SPI_CHANNEL, .name = (_name),                    \
	  .cfg.spi = { .supported_config_flags = (_flags),                     \
		       .num_devices = (_num_dev),                              \
		       .rd_max_speed_hz = (_rd_hz),                            \
		       .wr_max_speed_hz = (_wr_hz) } }

/**
 * Memory device channel; caps are OBMF_MEM_DEV_CAP_*, size in bytes,
 * erase block in KB, read/write sizes in bytes.
 */
#define OBMF_CHANNEL_MEM_DEV(_name, _caps, _size, _erase_kb, _rd, _wr)         \
	{ .channel = OBMF_ICP_MEM_DEV_CHANNEL, .name = (_name),                \
	  .cfg.mem_dev = { .capabilities = (_caps), .device_size = (_size),    \
			   .erase_size = (_erase_kb), .read_size = (_rd),      \
			   .write_size = (_wr) } }

/** GPIO channel exposing @p _count GPIOs. */
#define OBMF_CHANNEL_GPIO(_ch, _name, _count)                                  \
	{ .channel = (_ch), .name = (_name), .cfg.gpio = { .count = (_count) } }

/** One I/O port range for OBMF_CHANNEL_IO() (OBMF_IO_SERVICE_* type). */
#define OBMF_IO_RANGE(_start, _end, _service)                                  \
	{ .range_flags = OBMF_IO_RANGE_ENABLE, .range_start = (_start),        \
	  .range_end = (_end), .service_type = (_service) }

/**
 * I/O port channel; @p _types is a bitmap of OBMF_ICP_IO_TRANS_* bits and
 * the remaining arguments are up to OBMF_IO_MAX_RANGES OBMF_IO_RANGE()s.
 */
#define OBMF_CHANNEL_IO(_name, _types, ...)                                    \
	{ .channel = OBMF_ICP_IO_CHANNEL, .name = (_name),                     \
	  .cfg.io = { .hdr = { .trans_types_supported = (_types) },            \
		      .ranges = { __VA_ARGS__ } } }

/** I2C controller channel on local bus @p _bus; speed is OBMF_I2C_SPEED_*. */
#define OBMF_CHANNEL_I2C(_ch, _name, _bus, _speed)                             \
	{ .channel = (_ch), .name = (_name), .bus = (_bus),                    \
	  .cfg.i2c = { .controller_speed = (_speed) } }

/**
 * I2C target channel; speed is OBMF_I2C_SPEED_*.  Addresses are set at run
 * time with obmf_i2c_target_set_address().
 */
#define OBMF_CHANNEL_I2C_TARGET(_ch, _name, _speed)                            \
	{ .channel = (_ch), .name = (_name),                                   \
	  .cfg.i2c_target = { .speed = (_speed) } }

/** Default I3C controller capabilities (see obmf_i3c_config_data_t). */
#define OBMF_I3C_DEFAULT_CONFIG                                                \
	{ .i3c_feature_flags = OBMF_I3C_FEATURE_IBI_PRIV_READ,                 \
	  .max_chained_ops = 16, .max_scl_pp_khz = 12500,                      \
	  .max_scl_od_khz = 400, .max_read_turnaround = 1000,                  \
	  .max_dev_nack_retry = 3, .max_payload_size = 256,                    \
	  .max_ibi_payload_size = 128 }

/** I3C controller channel on local bus @p _bus with default capabilities. */
#define OBMF_CHANNEL_I3C(_ch, _name, _bus)                                     \
	{ .channel = (_ch), .name = (_name), .bus = (_bus),                    \
	  .cfg.i3c = OBMF_I3C_DEFAULT_CONFIG }

/* =====================================================================
 * Public API
 * ===================================================================== */

/**
 * @brief Build the discovery structure from @p cfg and start the service.
 *
 * @p cfg and its channel table must stay valid (normally static const).
 * Invalid or duplicate table entries are logged and skipped.
 *
 * @return 0 on success, negative errno if the service could not start.
 */
int obmf_service_init(const struct obmf_plat_config *cfg);

/** @brief True if the platform configured @p channel (Channel 0 is always). */
bool obmf_channel_is_present(uint8_t channel);
void obmf_message_handler(uint8_t *msg, uint32_t len);
int obmf_get_response(uint8_t *req_buf, uint32_t req_len,
		      uint8_t *rsp_buf, uint32_t *rsp_len);
int obmf_send_request(uint8_t channel, uint8_t *req_buf, uint32_t req_len);

/**
 * @brief Check whether a channel is currently enabled by the host.
 */
bool obmf_channel_is_enabled(uint8_t channel);

void obmf_mmio_local_update(uint32_t offset, uint32_t len, const uint8_t *data);

typedef void (*obmf_mmio_write_cb_t)(uint32_t offset, uint32_t len,
				     const uint8_t *data, void *ctx);
void obmf_register_mmio_write_cb(obmf_mmio_write_cb_t cb, void *ctx);

/* MMIO channel API (Consumer - legacy for UART/POST/VW) */
int obmf_mmio_read(uint32_t offset, uint32_t len, uint8_t *buf);
int obmf_mmio_write(uint32_t offset, uint32_t len, uint8_t *buf);

/* Serial channel API (Consumer - we send requests) */
/**
 * @brief Register the response callback for a given Serial channel.
 * @param channel OBMF_ICP_SERIAL_CHANNEL_0 or OBMF_ICP_SERIAL_CHANNEL_1.
 */
void obmf_register_serial_rsp_cb(uint8_t channel, obmf_serial_rsp_cb_t cb,
				 void *ctx);

/**
 * @brief Refill callback for obmf_serial_send().
 *
 * Called after a partial-NACK to append additional bytes to the retry packet
 * (spec §partial-accept: "additional bytes may be appended").
 *
 * @param buf   Buffer to write additional bytes into.
 * @param space Maximum bytes that may be written.
 * @param ctx   User context passed to obmf_serial_send().
 * @return Number of bytes written (0 = nothing to append).
 */
typedef uint16_t (*obmf_serial_refill_cb_t)(uint8_t *buf, uint16_t space,
					    void *ctx);

/**
 * @brief Send serial data with partial-accept retry (spec §partial-accept).
 *
 * Blocks until the data is fully acknowledged, a transport error occurs, or
 * OBMF_REQUEST_MAX_RETRY+1 attempts are exhausted.  On each partial-NACK the
 * unaccepted tail is retried from the first unaccepted byte; if @p refill is
 * non-NULL it appends queued bytes to the retry packet (saves an extra
 * round-trip per the spec "additional bytes may be appended" allowance).
 *
 * @param channel OBMF_ICP_SERIAL_CHANNEL_0 or OBMF_ICP_SERIAL_CHANNEL_1.
 * @param data   Initial payload.
 * @param len    Number of bytes, at most OBMF_SERIAL_MAX_CHARS.
 * @param refill Optional callback to append bytes on retry; NULL = none.
 * @param ctx    User context forwarded to @p refill.
 * @return 0 on full ACK, -EMSGSIZE if @p len is too large, negative errno
 *         on other errors.
 */
int obmf_serial_send(uint8_t channel, const uint8_t *data, uint16_t len,
		     obmf_serial_refill_cb_t refill, void *ctx);

/* Serial channel API (Producer - host sends serial data to us) */
typedef uint16_t (*obmf_serial_rx_cb_t)(const uint8_t *data, uint16_t len,
					void *ctx);
/**
 * @brief Register the RX callback for a given Serial channel.
 * @param channel OBMF_ICP_SERIAL_CHANNEL_0 or OBMF_ICP_SERIAL_CHANNEL_1.
 */
void obmf_register_serial_rx_cb(uint8_t channel, obmf_serial_rx_cb_t cb,
				 void *ctx);

/* IPMI channel API (Consumer - we send requests) */
void obmf_register_ipmi_rsp_cb(obmf_ipmi_rsp_cb_t cb, void *ctx);
int obmf_ipmi_send(uint8_t netfn_lun, uint8_t cmd,
		   const uint8_t *data, uint16_t data_len);

/**
 * Initialise the SPI Producer role (ch17). Binds the local shd_dev flash
 * device used to serve BMC-originated SPI requests.
 */
void obmf_spi_producer_init(void);

/* Memory Device channel API (Consumer - we send requests, ch4) */

/**
 * Initialise the Memory Device flash page cache pool.  Must be called once
 * before any obmf_mem_dev_* function is used (called from obmf_service_init).
 */
void obmf_mem_dev_init(void);

/**
 * Read @len bytes from the remote memory device at @addr into @buf.
 * Uses an LRU page cache: reads a full 960-byte page on a cache miss and
 * caches it for subsequent accesses.  @len must be <= 960.
 */
int obmf_mem_dev_read(uint32_t addr, uint32_t len, uint8_t *buf);

/**
 * Write @len bytes from @data to the remote memory device at @addr.
 * Invalidates any cache entries overlapping the written range.
 */
int obmf_mem_dev_write(uint32_t addr, const uint8_t *data, uint32_t len);

/**
 * Erase @erase_sz bytes of the remote memory device starting at @addr.
 * @erase_sz must be a non-zero multiple of the device's ERASE_SIZE.
 * Invalidates any cache entries overlapping the erased range.
 */
int obmf_mem_dev_erase(uint32_t addr, uint32_t erase_sz);

/* =====================================================================
 * I2C Controller channel API (Producer - host/BMC drives I2C through us)
 * =====================================================================
 *
 * The host (OBMF Consumer) sends I2C read/write/SMBus requests on this
 * channel; the OBMF service performs the transaction on the physical bus
 * via hal_i2c and returns the response.  The platform must bind the
 * channel to a physical I2C bus before enabling it.
 */

/**
 * @brief Bind the OBMF I2C Controller channel to a physical I2C bus.
 * @param bus Zephyr/OpenBIC I2C bus index used by hal_i2c.
 */
/**
 * @brief Bind an OBMF I2C Controller channel to a physical I2C bus.
 * @param channel OBMF channel number (OBMF_ICP_I2C_CONTROLLER_CHANNEL_0 .. _3).
 * @param bus     Physical I2C bus index used by hal_i2c.
 */
void obmf_i2c_set_controller_bus(uint8_t channel, uint8_t bus);

/**
 * @brief Bind an OBMF I3C Controller channel to a physical I3C bus.
 * @param channel OBMF_ICP_I3C_CONTROLLER_CHANNEL or
 *                OBMF_ICP_I3C_CONTROLLER_CHANNEL_1.
 * @param bus     Physical I3C bus index used by hal_i3c.
 */
void obmf_i3c_set_controller_bus(uint8_t channel, uint8_t bus);

/* =====================================================================
 * I2C Target channel API (Consumer - we forward target writes to host)
 * =====================================================================
 *
 * When this device is addressed as an I2C target, the platform forwards the
 * raw write payload received on the physical bus to the host (OBMF Producer)
 * with obmf_i2c_target_write(). The payload is passed through as is, e.g. an
 * MCTP over SMBus response from a device behind the OBMF I2C Controller
 * channel is not parsed by this device.
 */
/**
 * @brief Advertise the I2C target address of an I2C Target channel.
 * @param channel OBMF channel number (OBMF_ICP_I2C_TARGET_CHANNEL_0 .. _1).
 * @param address 7-bit I2C target address.
 * @return 0 on success, -EINVAL on an invalid channel.
 */
int obmf_i2c_target_set_address(uint8_t channel, uint8_t address);

/**
 * @brief Register a response callback for an I2C Target channel.
 * @param idx Channel index (0 = ch14/bus1, 1 = ch15/bus2).
 */
void obmf_register_i2c_target_rsp_cb(uint8_t idx,
				      obmf_i2c_target_rsp_cb_t cb, void *ctx);

/**
 * @brief Forward an I2C target write to the host (blocking until response).
 * @param channel OBMF channel number (OBMF_ICP_I2C_TARGET_CHANNEL_0 or _1).
 * @param address The I2C target address that was addressed.
 * @param write_data Received write payload.
 * @param write_len Length of @write_data.
 */
int obmf_i2c_target_write(uint8_t channel, uint8_t address,
			  const uint8_t *write_data, uint16_t write_len);

/* GPIO channel API (Producer - host sends GPIO requests to us) */
typedef void (*obmf_gpio_set_cb_t)(uint16_t index, uint8_t value, void *ctx);
typedef uint8_t (*obmf_gpio_get_cb_t)(uint16_t index, void *ctx);
/* Register callbacks for a specific GPIO channel (generic, channel-indexed) */
void obmf_register_gpio_channel_get_cb(uint8_t channel,
				       obmf_gpio_get_cb_t cb, void *ctx);
void obmf_register_gpio_channel_set_cb(uint8_t channel,
				       obmf_gpio_set_cb_t cb, void *ctx);
/* Backward-compat wrappers (default to channel 5, VW GPIO) */
void obmf_register_gpio_set_cb(obmf_gpio_set_cb_t cb, void *ctx);
void obmf_register_gpio_get_cb(obmf_gpio_get_cb_t cb, void *ctx);
int obmf_gpio_notify(const uint16_t *indices, const uint8_t *values,
		     uint16_t count, uint8_t channel);
/*
 * Report a new pin level; sends Interrupt Notify only when it matches the
 * interrupt configured by the Consumer for that index.
 */
int obmf_gpio_report_level(uint8_t channel, uint16_t index, bool high);

/* GPIO channel configuration (Platform populates entries at init) */
obmf_gpio_config_entry_t *obmf_gpio_get_config_entries(void);
uint16_t obmf_gpio_get_config_count(void);

/* SGPIO1 channel configuration (Platform populates entries at init) */
obmf_gpio_config_entry_t *obmf_sgpio1_get_config_entries(void);
uint16_t obmf_sgpio1_get_config_count(void);
void obmf_sgpio1_set_config_count(uint16_t count);

/* SGPIO (legacy name, equivalent to SGPIO1) */
obmf_gpio_config_entry_t *obmf_sgpio_get_config_entries(void);
uint16_t obmf_sgpio_get_config_count(void);

/* SGPIO0 channel configuration (Platform populates entries at init) */
obmf_gpio_config_entry_t *obmf_sgpio0_get_config_entries(void);
uint16_t obmf_sgpio0_get_config_count(void);
void obmf_sgpio0_set_config_count(uint16_t count);

/* Platform GPIO channel configuration (Platform populates entries at init) */
obmf_gpio_config_entry_t *obmf_smc_gpio_get_config_entries(void);
uint16_t obmf_smc_gpio_get_config_count(void);
void obmf_smc_gpio_set_config_count(uint16_t count);

/* GPIO channel API (Consumer - we send Get/Set requests) */
typedef void (*obmf_gpio_rsp_cb_t)(uint8_t command, const uint16_t *indices,
				   const uint8_t *values, uint16_t count,
				   void *ctx);
void obmf_register_gpio_rsp_cb(obmf_gpio_rsp_cb_t cb, void *ctx);
int obmf_gpio_get_values(const uint16_t *indices, uint16_t count, uint8_t channel);
int obmf_gpio_set_values(const uint16_t *indices, const uint8_t *values,
			 uint16_t count, uint8_t channel);

void obmf_stop(void);

/**
 * @brief Register IBI callbacks for an I3C device and enable IBI.
 *
 * On IBI the SMC performs an optional private SDR read (if the MDB indicates
 * a pending read) and sends an OBMF_I3C_EVT_IBI_NOTIFICATION event to the BMC.
 *
 * @param bus          Physical I3C bus index.
 * @param dynamic_addr 7-bit dynamic address of the target device.
 * @return 0 on success, negative errno on failure.
 */
int obmf_i3c_request_ibi(uint8_t bus, uint8_t dynamic_addr);

/* I/O channel API (Consumer – we send write/read requests) */
/**
 * @brief Write @len bytes to I/O port @port over the OBMF I/O channel.
 *
 * Transaction type is selected automatically from @len:
 *   len % 4 == 0  →  32-bit (Sequential or Fixed Port I/O)
 *   len % 2 == 0  →  16-bit
 *   otherwise     →   8-bit
 *
 * @param port   Starting I/O port address.
 * @param len    Number of bytes to write.
 * @param buf    Source data buffer.
 * @param fixed  true = all bytes target the same port (Fixed Port I/O),
 *               false = port incremented per byte (Sequential Port I/O).
 * @return 0 on success, negative errno on failure.
 */
int obmf_io_write(uint16_t port, uint16_t len, const uint8_t *buf, bool fixed);

/**
 * @brief Initiate a port I/O read of @len bytes from @port.
 *
 * Transaction type is selected by the same width rules as obmf_io_write.
 * The read data arrives asynchronously through handle_io_response.
 *
 * @param port   Starting I/O port address.
 * @param len    Number of bytes to read.
 * @param fixed  true = Fixed Port I/O, false = Sequential Port I/O.
 * @return 0 on success, negative errno on failure.
 */
int obmf_io_read(uint16_t port, uint16_t len, bool fixed);
/**
 * @brief I/O channel write callback – called on every port I/O write.
 * @param port_addr  Starting I/O port address.
 * @param size       Total write size in bytes.
 * @param data       Write data buffer.
 * @param is_fixed   true = Fixed Port I/O (all bytes to same port),
 *                   false = Sequential (port incremented per byte).
 * @param ctx        User context.
 */
typedef void (*obmf_io_write_cb_t)(uint16_t port_addr, uint16_t size,
				   const uint8_t *data, bool is_fixed,
				   void *ctx);
void obmf_register_io_write_cb(obmf_io_write_cb_t cb, void *ctx);

/**
 * @brief Copy the latest captured I/O POST code bytes (ports 0x80..0x83).
 * @param buf  Destination buffer.
 * @param len  Number of bytes to copy (capped to OBMF_IO_POST_CODE_SIZE).
 */
void obmf_io_get_post_code(uint8_t *buf, uint8_t len);

#endif /* OBMF_H */
