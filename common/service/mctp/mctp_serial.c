/*
 * Copyright (c) 2023 Nuvoton Technology Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mctp.h"
#include "hal_i2c_target.h"
#include <logging/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/crc.h>
#include <sys/printk.h>
#include <zephyr.h>
#include <sys/ring_buffer.h>
#include "libutil.h"

LOG_MODULE_DECLARE(mctp, LOG_LEVEL_DBG);

#define MCTP_SMBUS_PEC_SIZE 1
#define MCTP_SERIAL_MTU 68 /* base mtu (64) + mctp header */
#define MCTP_SERIAL_FRAME_MTU (MCTP_SERIAL_MTU + 6) /* + serial framing */

#define MCTP_SERIAL_VERSION 0x1 /* DSP0253 defines a single version: 1 */

#define MCTP_SERIAL_TRAILER_SIZE 3

#define BUFSIZE MCTP_SERIAL_FRAME_MTU


typedef enum {
        STATE_IDLE,
        STATE_START,
        STATE_HEADER,
        STATE_DATA,
        STATE_ESCAPE,
        STATE_TRAILER,
        STATE_DONE,
        STATE_ERR,
} mctp_serial_state;

extern struct k_sem serial_sem;

extern uint8_t serial_tx_buf[SERIAL_BUF_SIZE];
extern struct ring_buf serial_rx_buf;

mctp_serial_state rxstate;
uint16_t txfcs, rxfcs, rxfcs_rcvd;
uint32_t rxlen;
uint32_t txpos, rxpos;

extern void mctp_serial_write_data(uint8_t *src_buf, int src_len);

static void mctp_serial_push_header(uint8_t *src)
{
	uint8_t c = *src;

        switch (rxpos) {
        case 0:
                if (c == BYTE_FRAME)
                        rxpos++;
                else
                        rxstate = STATE_ERR;
                break;
        case 1:
                if (c == MCTP_SERIAL_VERSION) {
                        rxpos++;
                        rxfcs = crc16_ccitt(FCS_INIT, src, 1);
                } else {
                        rxstate = STATE_ERR;
                }
                break;
        case 2:
                if (c > MCTP_SERIAL_FRAME_MTU) {
                        rxstate = STATE_ERR;
                } else {
                        rxlen = c;
                        rxpos = 0;
                        rxstate = STATE_DATA;
                        rxfcs = crc16_ccitt(rxfcs, src, 1);
                }
                break;
        }
}

static void mctp_serial_push_trailer(uint8_t *src)
{
	uint8_t c = *src;

        switch (rxpos) {
        case 0:
                rxfcs_rcvd = c << 8;
                rxpos++;
                break;
        case 1:
                rxfcs_rcvd |= c;
                rxpos++;
                break;
        case 2:
                if (c != BYTE_FRAME) {
                        rxstate = STATE_ERR;
                } else {
                        rxstate = STATE_IDLE;
		}
                break;
        }
}

static void mctp_serial_push(uint8_t *buf, uint8_t *src)
{
	uint8_t c = *src;

        switch (rxstate) {
        case STATE_IDLE:
                rxstate = STATE_HEADER;
        case STATE_HEADER:
                mctp_serial_push_header(src);
                break;

        case STATE_ESCAPE:
                c |= 0x20;
        case STATE_DATA:
                if (rxstate != STATE_ESCAPE && c == BYTE_ESC) {
                        rxstate = STATE_ESCAPE;
                } else {
                        rxfcs = crc16_ccitt(rxfcs, src, 1);
			buf[rxpos] = c;
                        rxpos++;
                        rxstate = STATE_DATA;
                        if (rxpos == rxlen) {
                                rxpos = 0;
                                rxstate = STATE_TRAILER;
                        }
                }
                break;

        case STATE_TRAILER:
                mctp_serial_push_trailer(src);
                break;

        case STATE_ERR:
                if (c == BYTE_FRAME)
                        rxstate = STATE_IDLE;
                break;

        default:
                LOG_DBG("invalid rx state %d\n", rxstate);
        }
}

static uint16_t mctp_serial_read(void *mctp_p, uint8_t *buf, uint32_t len,
				mctp_ext_params *extra_data)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_p, 0);
	CHECK_NULL_ARG_WITH_RETURN(buf, 0);
	CHECK_ARG_WITH_RETURN(!len, 0);
	CHECK_NULL_ARG_WITH_RETURN(extra_data, 0);
	uint32_t ret = 0;
	int loop, rx_len;
	uint8_t rx_buff[MCTP_DEFAULT_MSG_MAX_SIZE + MCTP_TRANSPORT_HEADER_SIZE];

	while(true) {
		k_sem_take(&serial_sem, K_FOREVER);
		rx_len = ring_buf_get(&serial_rx_buf, rx_buff, sizeof(rx_buff));
		if(!rx_len) {
			continue;
		}

		for(loop = 0; loop < rx_len; loop++) {
			mctp_serial_push(buf, rx_buff+loop);
		}

		if((rxstate == STATE_IDLE) || (rxstate == STATE_ERR)) {
			break;
		}
	}

	if(rxfcs_rcvd == rxfcs) {
		ret = rxlen;
	}

        rxlen = 0;
        rxpos = 0;

	return ret;
}

static uint16_t mctp_serial_write(void *mctp_p, uint8_t *buf, uint32_t len,
				 mctp_ext_params extra_data)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_p, MCTP_ERROR);
	CHECK_NULL_ARG_WITH_RETURN(buf, MCTP_ERROR);
	CHECK_ARG_WITH_RETURN(!len, MCTP_ERROR);

	//make header
	txpos = 0;
	serial_tx_buf[0] = BYTE_FRAME;
	serial_tx_buf[1] = MCTP_SERIAL_VERSION;
	serial_tx_buf[2] = len;
	txpos += 3;

	txfcs = crc16_ccitt(FCS_INIT, serial_tx_buf + 1, 2);

	//copy mctp message
	memcpy(serial_tx_buf + txpos, buf, len);
	txfcs = crc16_ccitt(txfcs, serial_tx_buf + txpos, len);
	txpos += len;

	//make trailer
	serial_tx_buf[txpos] = txfcs >> 8;
	serial_tx_buf[txpos + 1] = txfcs & 0xff;
	serial_tx_buf[txpos + 2] = BYTE_FRAME;
	txpos += 3;
	mctp_serial_write_data(serial_tx_buf, txpos);

	return MCTP_SUCCESS;
}

uint8_t mctp_serial_init(mctp *mctp_inst, mctp_medium_conf medium_conf)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);

	mctp_inst->medium_conf = medium_conf;
	mctp_inst->read_data = mctp_serial_read;
	mctp_inst->write_data = mctp_serial_write;
	rxstate = STATE_IDLE;

	return MCTP_SUCCESS;
}

uint8_t mctp_serial_deinit(mctp *mctp_inst)
{
	CHECK_NULL_ARG_WITH_RETURN(mctp_inst, MCTP_ERROR);

	mctp_inst->read_data = NULL;
	mctp_inst->write_data = NULL;
	memset(&mctp_inst->medium_conf, 0, sizeof(mctp_inst->medium_conf));
	return MCTP_SUCCESS;
}
