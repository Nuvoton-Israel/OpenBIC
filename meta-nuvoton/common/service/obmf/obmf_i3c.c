/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * OBMF I3C Controller Channel (v1.0.0 RC1 spec section 4.8)
 *
 * Use case: BMC (Consumer) -> USB -> SMC (Producer) -> I3C bus -> I3C devices.
 * The SMC drives the physical I3C bus via hal_i3c and returns results to the BMC.
 *
 * Supported commands:
 *   0: I3C_CMD_EXECUTE_SEQUENCE  – CCC, CCC-w-DefByte, Private SDR transfers
 *   1: I3C_CMD_DO_DAA            – RSTDAA + ENTDAA (address assignment)
 *   2: I3C_CMD_BUS_RECOVERY      – RSTDAA / SCL recovery
 *   3: I3C_CMD_SET_ASSOCIATED_I2C – store association (no physical action)
 *   4: I3C_CMD_GET_STATS         – return hardware counters (stubbed to zero)
 */

#include "obmf_internal.h"
#include "hal_i3c.h"
#include <kernel.h>

LOG_MODULE_DECLARE(obmf);

/* Maximum total I3C response payload (must fit inside OBMF_RSP_BUF_SIZE=256) */
#define OBMF_I3C_RSP_BUF_SIZE     256
#define OBMF_I3C_MAX_RSP_PAYLOAD \
	(OBMF_I3C_RSP_BUF_SIZE - OBMF_HEADER_SIZE - OBMF_I3C_RSP_HDR_SIZE)

/* Retry count for hal_i3c calls */
#define OBMF_I3C_HAL_RETRY  3

/* ── Symbols exported from hal_i3c.c (same pattern as plat_i3c.c) ──────── */

extern const struct device *dev_i3c[I3C_MAX_NUM];
extern struct k_mutex mutex_dev[I3C_MAX_NUM];

/* ── Physical bus binding ───────────────────────────────────────────────── */

/* Physical bus of each I3C Controller channel; 0xFF = not configured */
static uint8_t i3c_controller_bus[OBMF_MAX_I3C_CONTROLLER_CHANNELS] = { 0xFF, 0xFF };

#ifdef i3c_master_register_hj_callback
static void i3c_hotjoin_cb(const struct device *dev, void *user_data);
#endif

void obmf_i3c_set_controller_bus(uint8_t channel, uint8_t bus)
{
	i3c_controller_bus[OBMF_I3C_CTRL_CHAN_IDX(channel)] = bus;

#ifdef i3c_master_register_hj_callback
	if (bus < I3C_MAX_NUM && dev_i3c[bus])
		i3c_master_register_hj_callback(dev_i3c[bus], i3c_hotjoin_cb,
						(void *)(uintptr_t)channel);
#endif
}

/* Clamp a host write of the IBI target array to what the producer can do */
void obmf_i3c_config_written(obmf_i3c_config_data_t *cfg)
{
	for (int i = 0; i < OBMF_I3C_MAX_IBI_TARGETS; i++) {
		obmf_i3c_ibi_target_cfg_t *t = &cfg->ibi_targets[i];

		t->dynamic_addr &= 0x7F;
		/* No automatic follow-up read: the BMC issues it (see below) */
		t->control_flags = 0;
		t->max_ibi_payload_len = MIN(t->max_ibi_payload_len,
					     IBI_PAYLOAD_SIZE);
		t->max_pending_read_len = MIN(t->max_pending_read_len,
					      IBI_PAYLOAD_SIZE);
	}
}

/* I3C Controller channel bound to a physical bus, or 0 if none is. */
static uint8_t i3c_channel_of_bus(uint8_t bus)
{
	if (i3c_controller_bus[0] == bus)
		return OBMF_ICP_I3C_CONTROLLER_CHANNEL;
	if (i3c_controller_bus[1] == bus)
		return OBMF_ICP_I3C_CONTROLLER_CHANNEL_1;
	return 0;
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

/**
 * @brief Map a negative hal_i3c / Zephyr errno to an OBMF I3C channel status.
 */
static uint8_t i3c_errno_to_chan_status(int ret)
{
	switch (ret) {
	case 0:       return OBMF_ICP_STATUS_SUCCESS;
	case -ENXIO:
	case -ENODEV: return OBMF_I3C_STATUS_NACK;
	case -EBUSY:  return OBMF_I3C_STATUS_BUS_BUSY;
	case -ETIMEDOUT: return OBMF_I3C_STATUS_BUS_TIMEOUT;
	case -EIO:    return OBMF_I3C_STATUS_BUS_FRAMING;
	default:      return OBMF_ICP_STATUS_PERMANENT_ERROR;
	}
}

/**
 * @brief Map a negative hal_i3c / Zephyr errno to an OBMF per-op status code.
 */
static uint8_t i3c_errno_to_op_status(int ret)
{
	switch (ret) {
	case 0:          return OBMF_I3C_OP_SUCCESS;
	case -EIO:
	case -ENXIO:
	case -ENODEV:    return OBMF_I3C_OP_NACK;
	case -ETIMEDOUT: return OBMF_I3C_OP_TIMEOUT;
	case -EAGAIN:    return OBMF_I3C_OP_ARB_LOST;
	default:         return OBMF_I3C_OP_BUS_ERR;
	}
}

/**
 * @brief Build the fixed I3C channel response headers and set *rsp_len.
 */
static void build_i3c_response(uint8_t channel, uint8_t chan_status,
				uint8_t command, uint8_t result_count,
				uint32_t extra_payload_len,
				uint8_t *rsp_buf, uint32_t *rsp_len)
{
	obmf_icp_header_t *rsp_hdr = (obmf_icp_header_t *)rsp_buf;

	OBMF_ICP_SET_CHANNEL(rsp_hdr, channel);
	OBMF_ICP_SET_RQRESP_STATUS(rsp_hdr, OBMF_ICP_MSG_TYPE_RESPONSE, chan_status);

	obmf_i3c_response_t *body =
		(obmf_i3c_response_t *)(rsp_buf + OBMF_HEADER_SIZE);
	body->command = command;
	body->result_count = result_count;

	uint32_t payload = OBMF_I3C_RSP_HDR_SIZE + extra_payload_len;
	OBMF_ICP_SET_SIZE(rsp_hdr, payload);
	*rsp_len = OBMF_HEADER_SIZE + payload;
}

/* ── I3C_CMD_EXECUTE_SEQUENCE handler ──────────────────────────────────── */

/**
 * @brief Execute a single CCC operation.
 *
 * @param bus        Physical I3C bus index.
 * @param ccc_id     CCC ID.
 * @param dyn_addr   Target dynamic address (0x7E = broadcast).
 * @param is_write   1=Write, 0=Read.
 * @param data_len   Payload length.
 * @param data_in    Write payload (ignored when is_write=0).
 * @param data_out   Read buffer (filled when is_write=0 and ret==0).
 * @param out_actual Set to actual bytes transferred on success.
 * @return OBMF_I3C_OP_* result code.
 */
static uint8_t exec_ccc(uint8_t bus, uint8_t ccc_id, uint8_t dyn_addr,
			uint8_t is_write, uint16_t data_len,
			const uint8_t *data_in,
			uint8_t *data_out, uint16_t *out_actual)
{
	struct i3c_ccc_cmd ccc;
	uint8_t ccc_buf[I3C_MAX_DATA_SIZE] = { 0 };

	memset(&ccc, 0, sizeof(ccc));
	ccc.id = ccc_id;
	/* Unicast CCCs require the DIRECT bit (0x80). */
	if (dyn_addr != I3C_BROADCAST_ADDR) {
		ccc.id |= I3C_CCC_DIRECT;
	}
	ccc.addr = dyn_addr;
	ccc.rnw = is_write ? I3C_WRITE_CMD : I3C_READ_CMD;
	ccc.payload.data = ccc_buf;

	if (is_write) {
		uint16_t copy = data_len < I3C_MAX_DATA_SIZE ? data_len : I3C_MAX_DATA_SIZE;
		if (copy > 0 && data_in) {
			memcpy(ccc_buf, data_in, copy);
		}
		ccc.payload.length = copy;
	} else {
		ccc.payload.length = data_len < I3C_MAX_DATA_SIZE ? data_len : I3C_MAX_DATA_SIZE;
	}

	int ret = k_mutex_lock(&mutex_dev[bus], K_MSEC(2000));
	if (ret) {
		return OBMF_I3C_OP_TIMEOUT;
	}
	ret = i3c_master_send_ccc(dev_i3c[bus], &ccc);
	k_mutex_unlock(&mutex_dev[bus]);

	if (ret == 0) {
		uint16_t actual = (uint16_t)ccc.payload.length;
		if (!is_write && data_out && actual > 0) {
			memcpy(data_out, ccc_buf, actual);
		}
		*out_actual = actual;
	}
	return i3c_errno_to_op_status(ret);
}

/**
 * @brief Execute a single CCC-with-defining-byte operation.
 *
 * The defining byte is prepended to the payload before calling the driver.
 */
static uint8_t exec_ccc_w_db(uint8_t bus, uint8_t ccc_id, uint8_t defining_byte,
			      uint8_t dyn_addr, uint8_t is_write,
			      uint16_t data_len, const uint8_t *data_in,
			      uint16_t *out_actual)
{
	struct i3c_ccc_cmd ccc;
	/* defining byte + data payload */
	uint8_t ccc_buf[I3C_MAX_DATA_SIZE + 1] = { 0 };

	memset(&ccc, 0, sizeof(ccc));
	ccc.id = ccc_id;
	ccc.addr = dyn_addr;
	ccc.rnw = I3C_WRITE_CMD; /* defining-byte CCCs are write */
	ccc_buf[0] = defining_byte;

	uint16_t copy = data_len < I3C_MAX_DATA_SIZE ? data_len : I3C_MAX_DATA_SIZE;
	if (is_write && copy > 0 && data_in) {
		memcpy(ccc_buf + 1, data_in, copy);
	}
	ccc.payload.data = ccc_buf;
	ccc.payload.length = 1 + (is_write ? copy : 0);

	int ret = k_mutex_lock(&mutex_dev[bus], K_MSEC(2000));
	if (ret) {
		return OBMF_I3C_OP_TIMEOUT;
	}
	ret = i3c_master_send_ccc(dev_i3c[bus], &ccc);
	k_mutex_unlock(&mutex_dev[bus]);

	if (ret == 0) {
		*out_actual = (uint16_t)ccc.payload.length;
	}
	return i3c_errno_to_op_status(ret);
}

/**
 * @brief Execute a single Private SDR operation (write or read).
 */
static uint8_t exec_private_sdr(uint8_t bus, uint8_t dyn_addr,
				uint8_t is_write, uint16_t data_len,
				const uint8_t *data_in,
				uint8_t *data_out, uint16_t *out_actual)
{
	/*
	 * Look up the registered descriptor and
	 * call i3c_master_priv_xfer directly with a single-direction xfer.
	 * Avoid i3c_transfer() which always emits write+read (2 xfers) sharing
	 * the same buffer — that issues a zero-length write Sr before a pure
	 * read, which is wrong for standalone private reads.
	 */
	int ret = k_mutex_lock(&mutex_dev[bus], K_MSEC(2000));
	if (ret) {
		return OBMF_I3C_OP_TIMEOUT;
	}

	struct i3c_dev_desc *desc = i3c_master_get_desc(dev_i3c[bus], dyn_addr);
	if (!desc) {
		LOG_ERR("I3C exec_private_sdr: addr 0x%02x not registered", dyn_addr);
		k_mutex_unlock(&mutex_dev[bus]);
		return OBMF_I3C_OP_NACK;
	}

	uint8_t  xfer_buf[I3C_MAX_DATA_SIZE] = { 0 };
	uint16_t xfer_len = data_len < I3C_MAX_DATA_SIZE ? data_len : I3C_MAX_DATA_SIZE;
	struct i3c_priv_xfer xfer;

	if (is_write) {
		if (xfer_len > 0 && data_in) {
			memcpy(xfer_buf, data_in, xfer_len);
		}
		xfer.rnw = I3C_WRITE_CMD;
		xfer.data.out = xfer_buf;
		xfer.len = xfer_len;
	} else {
		xfer.rnw = I3C_READ_CMD;
		xfer.data.in = xfer_buf;
		xfer.len = xfer_len;
	}

	ret = i3c_master_priv_xfer(desc, &xfer, 1);
	k_mutex_unlock(&mutex_dev[bus]);

	if (ret == 0) {
		uint16_t actual = (uint16_t)xfer.len;
		if (!is_write && data_out && actual > 0) {
			memcpy(data_out, xfer_buf, actual);
		}
		*out_actual = actual;
	}

	return i3c_errno_to_op_status(ret);
}

/**
 * @brief Execute a write-then-read Private SDR transfer using Repeated Start (Sr).
 *
 * Combines a write and a read into a single i3c_master_priv_xfer(desc, xfers, 2)
 * call.  This produces  S W... Sr R... P  on the bus instead of the
 * stop-start sequence  S W... P S R... P  that two independent calls would
 * generate.  Most I3C targets require Sr for correct register-read behaviour.
 *
 * The caller is responsible for passing a read_buf of at least read_len bytes
 * (capped internally to I3C_MAX_DATA_SIZE).
 *
 * @param bus              Physical I3C bus index.
 * @param dyn_addr         Target 7-bit dynamic address.
 * @param write_data       Write payload (may be NULL when write_len == 0).
 * @param write_len        Number of bytes to write.
 * @param read_buf         Output buffer for received data.
 * @param read_len         Number of bytes to read.
 * @param out_write_actual Set to actual bytes written on success.
 * @param out_read_actual  Set to actual bytes read on success.
 * @return OBMF_I3C_OP_* result code (identical for write and read results).
 */
static uint8_t exec_private_sdr_wr(uint8_t bus, uint8_t dyn_addr,
				    const uint8_t *write_data, uint16_t write_len,
				    uint8_t *read_buf, uint16_t read_len,
				    uint16_t *out_write_actual,
				    uint16_t *out_read_actual)
{
	int ret = k_mutex_lock(&mutex_dev[bus], K_MSEC(2000));
	if (ret) {
		return OBMF_I3C_OP_TIMEOUT;
	}

	struct i3c_dev_desc *desc = i3c_master_get_desc(dev_i3c[bus], dyn_addr);
	if (!desc) {
		LOG_ERR("I3C exec_private_sdr_wr: addr 0x%02x not registered", dyn_addr);
		k_mutex_unlock(&mutex_dev[bus]);
		return OBMF_I3C_OP_NACK;
	}

	uint16_t wlen = write_len < I3C_MAX_DATA_SIZE ? write_len : I3C_MAX_DATA_SIZE;
	uint16_t rlen = read_len  < I3C_MAX_DATA_SIZE ? read_len  : I3C_MAX_DATA_SIZE;
	uint8_t  wbuf[I3C_MAX_DATA_SIZE] = { 0 };

	if (wlen > 0 && write_data) {
		memcpy(wbuf, write_data, wlen);
	}

	/*
	 * Two xfers in one call: driver issues Sr between them.
	 * xfers[1].data.in points directly into caller's read_buf to avoid
	 * an extra memcpy.
	 */
	struct i3c_priv_xfer xfers[2];
	xfers[0].rnw      = I3C_WRITE_CMD;
	xfers[0].data.out = wbuf;
	xfers[0].len      = wlen;
	xfers[1].rnw      = I3C_READ_CMD;
	xfers[1].data.in  = read_buf;
	xfers[1].len      = rlen;

	ret = i3c_master_priv_xfer(desc, xfers, 2);
	k_mutex_unlock(&mutex_dev[bus]);

	if (ret == 0) {
		*out_write_actual = (uint16_t)xfers[0].len;
		*out_read_actual  = (uint16_t)xfers[1].len;
	} else {
		*out_write_actual = 0;
		*out_read_actual  = 0;
	}

	return i3c_errno_to_op_status(ret);
}

/**
 * @brief Process I3C_CMD_EXECUTE_SEQUENCE.
 *
 * Iterates through the operation descriptors, executes each transfer, and
 * appends the result descriptor (plus optional read data) to the response.
 * Execution halts on the first failure (the failing op is included in the
 * result count per the spec).
 *
 * Write-then-Read Sr optimisation
 * ────────────────────────────────
 * When op[i] is PRIVATE_SDR write with send_stop == 0 (Repeated Start
 * requested by the BMC) and op[i+1] is PRIVATE_SDR read to the same dynamic
 * address, both ops are merged into exec_private_sdr_wr() which calls
 * i3c_master_priv_xfer(desc, xfers, 2).  The bus sequence becomes:
 *
 *   S  W(addr+W) <write_data>  Sr  R(addr+R) <read_data>  P
 *
 * instead of the two-transaction variant:
 *
 *   S  W(addr+W) <write_data>  P   S  R(addr+R) <read_data>  P
 *
 * The OBMF response still contains one result descriptor per original op
 * (write result immediately followed by read result + read_data payload),
 * so the BMC sees exactly num_ops results as expected.
 */
static void handle_execute_sequence(uint8_t channel, uint8_t bus,
				    uint8_t num_ops,
				    const uint8_t *ops_data, uint32_t ops_len,
				    uint8_t *rsp_buf, uint32_t *rsp_len)
{
	uint8_t *results    = rsp_buf + OBMF_HEADER_SIZE + OBMF_I3C_RSP_HDR_SIZE;
	uint32_t results_offset = 0;
	uint32_t results_space  = OBMF_I3C_MAX_RSP_PAYLOAD;

	const uint8_t *ptr = ops_data;
	uint32_t remaining = ops_len;
	uint8_t  ops_done  = 0;

	/* Shared read buffer – reused for every op to keep stack usage bounded. */
	uint8_t read_buf[I3C_MAX_DATA_SIZE];

	int i = 0;

	while (i < num_ops) {

		/* ── Parse current op header ─────────────────────────────────── */
		if (remaining < OBMF_I3C_OP_HDR_SIZE) {
			LOG_ERR("I3C CH%d: ops data truncated at op %d", channel, i);
			break;
		}

		const obmf_i3c_op_hdr_t *op_hdr = (const obmf_i3c_op_hdr_t *)ptr;
		uint16_t desc_len   = op_hdr->descriptor_len;
		uint32_t avail_desc = remaining - OBMF_I3C_OP_HDR_SIZE;

		if (avail_desc < desc_len) {
			LOG_ERR("I3C CH%d: descriptor truncated at op %d", channel, i);
			break;
		}

		const uint8_t *desc     = ptr + OBMF_I3C_OP_HDR_SIZE;
		const uint8_t *next_ptr = desc + desc_len;      /* first byte of op[i+1] */
		uint32_t next_rem       = avail_desc - desc_len; /* bytes from op[i+1] onward */

		/* ── Sr optimisation: merge write + read SDR ops ────────────────
		 *
		 * All conditions must hold:
		 *   1. op[i]: PRIVATE_SDR write, send_stop == 0
		 *   2. op[i+1] exists and is reachable in the buffer
		 *   3. op[i+1]: PRIVATE_SDR read, same dynamic_addr
		 */
		if (op_hdr->send_stop == 0 &&
		    op_hdr->op_type   == OBMF_I3C_OP_PRIVATE_SDR &&
		    desc_len >= OBMF_I3C_SDR_OP_HDR_SIZE &&
		    (i + 1) < num_ops &&
		    next_rem >= OBMF_I3C_OP_HDR_SIZE) {

			const obmf_i3c_sdr_op_hdr_t *curr_sdr =
				(const obmf_i3c_sdr_op_hdr_t *)desc;

			if (curr_sdr->is_write) {
				const obmf_i3c_op_hdr_t *n_hdr =
					(const obmf_i3c_op_hdr_t *)next_ptr;
				uint16_t n_desc_len = n_hdr->descriptor_len;
				uint32_t n_avail    = next_rem - OBMF_I3C_OP_HDR_SIZE;

				if (n_hdr->op_type == OBMF_I3C_OP_PRIVATE_SDR &&
				    n_avail         >= n_desc_len &&
				    n_desc_len      >= OBMF_I3C_SDR_OP_HDR_SIZE) {

					const obmf_i3c_sdr_op_hdr_t *n_sdr =
						(const obmf_i3c_sdr_op_hdr_t *)
						(next_ptr + OBMF_I3C_OP_HDR_SIZE);

					if (!n_sdr->is_write &&
					    n_sdr->dynamic_addr == curr_sdr->dynamic_addr) {

						/* Need space for 2 result descriptors */
						if (results_offset +
						    2 * OBMF_I3C_OP_RESULT_SIZE > results_space) {
							LOG_ERR("I3C CH%d: buf full at Sr op %d",
								channel, i);
							break;
						}

						const uint8_t *wdata =
							desc + OBMF_I3C_SDR_OP_HDR_SIZE;
						uint16_t write_actual = 0;
						uint16_t read_actual  = 0;

						memset(read_buf, 0, sizeof(read_buf));
						uint8_t op_status = exec_private_sdr_wr(
							bus, curr_sdr->dynamic_addr,
							wdata, curr_sdr->data_len,
							read_buf, n_sdr->data_len,
							&write_actual, &read_actual);

						/*
						 * Write op result descriptor.
						 * No read_data payload follows (it's a write).
						 */
						obmf_i3c_op_result_t *wr =
							(obmf_i3c_op_result_t *)
							(results + results_offset);
						wr->op_status  = op_status;
						wr->actual_len = write_actual;
						results_offset += OBMF_I3C_OP_RESULT_SIZE;
						ops_done++;

						/*
						 * Read op result descriptor.
						 * read_data payload appended immediately after.
						 */
						obmf_i3c_op_result_t *rd =
							(obmf_i3c_op_result_t *)
							(results + results_offset);
						rd->op_status  = op_status;
						rd->actual_len = read_actual;
						results_offset += OBMF_I3C_OP_RESULT_SIZE;
						ops_done++;

						if (op_status == OBMF_I3C_OP_SUCCESS &&
						    read_actual > 0) {
							uint32_t space =
								results_space - results_offset;
							uint16_t copy =
								read_actual < (uint16_t)space
								? read_actual : (uint16_t)space;
							memcpy(results + results_offset,
							       read_buf, copy);
							results_offset += copy;
						}

						/* Consume both ops and restart loop */
						ptr       = next_ptr + OBMF_I3C_OP_HDR_SIZE +
							    n_desc_len;
						remaining = n_avail - n_desc_len;
						i += 2;

						if (op_status != OBMF_I3C_OP_SUCCESS) {
							break;
						}
						continue;
					}
				}
			}
		}

		/* ── Single-op path (no Sr merge) ───────────────────────────── */
		if (results_offset + OBMF_I3C_OP_RESULT_SIZE > results_space) {
			LOG_ERR("I3C CH%d: response buffer full at op %d", channel, i);
			break;
		}

		uint8_t  op_status    = OBMF_I3C_OP_BUS_ERR;
		uint16_t actual_len   = 0;
		bool     has_read_data = false;

		memset(read_buf, 0, sizeof(read_buf));

		switch (op_hdr->op_type) {

		case OBMF_I3C_OP_CCC: {
			if (desc_len < OBMF_I3C_CCC_OP_HDR_SIZE) {
				LOG_ERR("I3C CH%d op%d: CCC descriptor too short",
					channel, i);
				op_status = OBMF_I3C_OP_BUS_ERR;
				break;
			}
			const obmf_i3c_ccc_op_hdr_t *ccc_h =
				(const obmf_i3c_ccc_op_hdr_t *)desc;
			const uint8_t *wdata = desc + OBMF_I3C_CCC_OP_HDR_SIZE;

			if (!ccc_h->is_write) {
				has_read_data = true;
			}
			op_status = exec_ccc(bus, ccc_h->ccc_id, ccc_h->dynamic_addr,
					     ccc_h->is_write, ccc_h->data_len,
					     wdata, read_buf, &actual_len);
			break;
		}

		case OBMF_I3C_OP_CCC_W_DB: {
			if (desc_len < OBMF_I3C_CCC_W_DB_OP_HDR_SIZE) {
				LOG_ERR("I3C CH%d op%d: CCC_W_DB descriptor too short",
					channel, i);
				op_status = OBMF_I3C_OP_BUS_ERR;
				break;
			}
			const obmf_i3c_ccc_w_db_op_hdr_t *ccc_db =
				(const obmf_i3c_ccc_w_db_op_hdr_t *)desc;
			const uint8_t *wdata = desc + OBMF_I3C_CCC_W_DB_OP_HDR_SIZE;

			op_status = exec_ccc_w_db(bus,
						  ccc_db->ccc_id, ccc_db->defining_byte,
						  ccc_db->dynamic_addr, ccc_db->is_write,
						  ccc_db->data_len, wdata, &actual_len);
			break;
		}

		case OBMF_I3C_OP_PRIVATE_SDR: {
			if (desc_len < OBMF_I3C_SDR_OP_HDR_SIZE) {
				LOG_ERR("I3C CH%d op%d: SDR descriptor too short",
					channel, i);
				op_status = OBMF_I3C_OP_BUS_ERR;
				break;
			}
			const obmf_i3c_sdr_op_hdr_t *sdr =
				(const obmf_i3c_sdr_op_hdr_t *)desc;
			const uint8_t *wdata = desc + OBMF_I3C_SDR_OP_HDR_SIZE;

			if (!sdr->is_write) {
				has_read_data = true;
			}
			op_status = exec_private_sdr(bus,
						     sdr->dynamic_addr, sdr->is_write,
						     sdr->data_len, wdata,
						     read_buf, &actual_len);
			break;
		}

		default:
			LOG_ERR("I3C CH%d op%d: unknown op_type %u",
				channel, i, op_hdr->op_type);
			op_status = OBMF_I3C_OP_BUS_ERR;
			break;
		}

		/* Append result descriptor */
		obmf_i3c_op_result_t *result =
			(obmf_i3c_op_result_t *)(results + results_offset);
		result->op_status  = op_status;
		result->actual_len = actual_len;
		results_offset += OBMF_I3C_OP_RESULT_SIZE;
		ops_done++;

		/* Append read data payload when present */
		if (has_read_data && op_status == OBMF_I3C_OP_SUCCESS && actual_len > 0) {
			uint32_t avail = results_space - results_offset;
			uint16_t copy  = actual_len < (uint16_t)avail
					 ? actual_len : (uint16_t)avail;
			memcpy(results + results_offset, read_buf, copy);
			results_offset += copy;
		}

		/* Advance past this op */
		ptr       = next_ptr;
		remaining = next_rem;
		i++;

		/* Stop on first failure (spec: result_count = successful + first failed) */
		if (op_status != OBMF_I3C_OP_SUCCESS) {
			break;
		}
	}

	build_i3c_response(channel, OBMF_ICP_STATUS_SUCCESS,
			   OBMF_I3C_CMD_EXECUTE_SEQUENCE, ops_done,
			   results_offset, rsp_buf, rsp_len);
}


/* ── I3C_CMD_DO_DAA handler ─────────────────────────────────────────────── */

/**
 * @brief Process I3C_CMD_DO_DAA.
 *
 * Issues RSTDAA (broadcast) to release all dynamic addresses, then issues a
 * single ENTDAA call.  The NPCM4xx hardware runs the full arbitration loop
 * internally, assigning addresses to every responding device until a NACK
 * indicates no more participants.  Only one i3c_master_send_entdaa() call is
 * needed regardless of device count; the desc argument merely identifies the
 * bus port.
 */
static void handle_daa(uint8_t channel, uint8_t bus,
		       uint8_t num_excluded, const uint8_t *payload, uint32_t payload_len,
		       uint8_t *rsp_buf, uint32_t *rsp_len)
{
	uint8_t addrs[I3C_MAX_ADDR];
	struct i3c_dev_desc *desc;
	const struct device *master = dev_i3c[bus];
	int i3c_desc_count = 0;
	struct i3c_dev_desc i3c_descs[I3C_MAX_NUM];

	/* Step 1: RSTDAA – release all previously assigned dynamic addresses. */
	I3C_MSG msg = { 0 };
	msg.bus = bus;
	int ret = i3c_brocast_ccc(&msg, I3C_CCC_RSTDAA, I3C_BROADCAST_ADDR);
	if (ret != 0) {
		LOG_DBG("I3C CH%d: RSTDAA failed (ret=%d), continuing", channel, ret);
	}

	ret = i3c_master_do_daa(master, addrs, &i3c_desc_count);
	if (ret < 0) {
		LOG_ERR("I3C CH%d: DAA failed: %d", channel, ret);
		build_i3c_response(channel, i3c_errno_to_chan_status(ret),
				   OBMF_I3C_CMD_DO_DAA, 0, 0, rsp_buf, rsp_len);
		return;
	}
	LOG_INF("bus_init: DAA assigned %d device(s)", i3c_desc_count);

	if (i3c_desc_count > I3C_MAX_NUM) {
		LOG_ERR("I3C CH%d: DAA found %d devices, at most %d supported",
			channel, i3c_desc_count, I3C_MAX_NUM);
		build_i3c_response(channel, OBMF_ICP_STATUS_PERMANENT_ERROR,
				   OBMF_I3C_CMD_DO_DAA, 0, 0, rsp_buf, rsp_len);
		return;
	}

	/* Attach i3c devices */
	for (int i = 0; i < i3c_desc_count; i++) {
		uint8_t addr = addrs[i];

		desc = &i3c_descs[i];
		desc->info.assigned_dynamic_addr = addr;
		ret = i3c_master_send_getpid(master, addr, &desc->info.pid);
		if (ret)
			LOG_ERR("bus_init: GETPID 0x%02x failed: %d", addr, ret);

		ret = i3c_master_send_getbcr(master, addr, &desc->info.bcr);
		if (ret)
			LOG_ERR("bus_init: GETBCR 0x%02x failed: %d", addr, ret);

		ret = i3c_master_send_getdcr(master, addr, &desc->info.dcr);
		if (ret)
			LOG_ERR("bus_init: GETDCR 0x%02x failed: %d", addr, ret);

		i3c_master_send_getmwl(master, addr, &desc->info.mwl);
		i3c_master_send_getmrl(master, addr, &desc->info.mrl, &desc->info.ibi_payload_sz);

		ret = i3c_master_attach_device(master, desc);
		if (ret)
			LOG_ERR("bus_init: attach 0x%02x failed: %d", addr, ret);

		desc->info.dynamic_addr = addr;
		desc->bus = master;
		/* Zephyr deferred logging packs args as 32-bit words; %llx breaks
		 * argument alignment on 32-bit targets.  Split the 64-bit PID. */
		LOG_INF("Register an I3C device: 0x%02x pid=0x%x%08x bcr=0x%02x dcr=0x%02x",
		       addr,
		       (uint32_t)(desc->info.pid >> 32), (uint32_t)desc->info.pid,
		       desc->info.bcr, desc->info.dcr);

		/* Register IBI for devices that are IBI-request capable (BCR[1]). */
		if (desc->info.bcr & BIT(1)) {
			ret = obmf_i3c_request_ibi(bus, addr);
			if (ret) {
				LOG_WRN("I3C DAA: IBI register failed for dev@%02x: %d",
					addr, ret);
			}
		}
	}

	/* Step 3: Build response – report all descriptors with assigned addresses. */
	uint8_t *dev_list = rsp_buf + OBMF_HEADER_SIZE + OBMF_I3C_RSP_HDR_SIZE;
	uint32_t dev_space = OBMF_I3C_MAX_RSP_PAYLOAD;
	uint8_t num_devs = 0;
	uint32_t dev_offset = 0;

	for (int i = 0; i < i3c_desc_count; i++) {
		if (dev_offset + sizeof(obmf_i3c_daa_dev_t) > dev_space)
			break;

		obmf_i3c_daa_dev_t *dev_entry =
			(obmf_i3c_daa_dev_t *)(dev_list + dev_offset);
		dev_entry->dynamic_addr = i3c_descs[i].info.dynamic_addr;
		/* PID is a 64-bit LE integer in the desc; serialise as 6-byte MSB-first. */
		uint64_t pid_le = i3c_descs[i].info.pid;
		for (int b = 0; b < 6; b++) {
			dev_entry->pid[5 - b] = (uint8_t)(pid_le >> (b * 8));
		}
		dev_entry->bcr = i3c_descs[i].info.bcr;
		dev_entry->dcr = i3c_descs[i].info.dcr;
		dev_offset += sizeof(obmf_i3c_daa_dev_t);
		num_devs++;
	}

	build_i3c_response(channel, OBMF_ICP_STATUS_SUCCESS,
			   OBMF_I3C_CMD_DO_DAA, num_devs,
			   dev_offset, rsp_buf, rsp_len);
}

/* ── I3C_CMD_BUS_RECOVERY handler ──────────────────────────────────────── */

static void handle_bus_recovery(uint8_t channel, uint8_t bus,
				uint8_t action_type,
				uint8_t *rsp_buf, uint32_t *rsp_len)
{
	I3C_MSG msg = { 0 };
	msg.bus = bus;
	int ret = 0;

	switch (action_type) {
	case OBMF_I3C_RECOVERY_SCL_LOW:
	case OBMF_I3C_RECOVERY_TARGET_RESET:
	case OBMF_I3C_RECOVERY_RSTACT_RESET:
	case OBMF_I3C_RECOVERY_HDR_EXIT:
	default:
		/* Best-effort: send RSTDAA to unblock the bus */
		ret = i3c_brocast_ccc(&msg, I3C_CCC_RSTDAA, I3C_BROADCAST_ADDR);
		if (ret != 0) {
			LOG_WRN("I3C CH%d: bus recovery RSTDAA returned %d", channel, ret);
		}
		break;
	}

	uint8_t chan_status = i3c_errno_to_chan_status(ret);
	build_i3c_response(channel, chan_status,
			   OBMF_I3C_CMD_BUS_RECOVERY, 0, 0, rsp_buf, rsp_len);
}

/* ── I3C_CMD_GET_STATS handler ─────────────────────────────────────────── */

static void handle_get_stats(uint8_t channel, uint8_t *rsp_buf, uint32_t *rsp_len)
{
	/* Hardware counter access is not yet abstracted in hal_i3c.
	 * Return 0 entries; platforms can override this via the response. */
	build_i3c_response(channel, OBMF_ICP_STATUS_SUCCESS,
			   OBMF_I3C_CMD_GET_STATS, 0, 0, rsp_buf, rsp_len);
}

/* ── I3C_CMD_SET_ASSOCIATED_I2C handler ────────────────────────────────── */

static uint16_t associated_i2c_channel; /* stored association, no physical effect */

static void handle_set_associated_i2c(uint8_t channel, uint16_t i2c_chan,
				      uint8_t *rsp_buf, uint32_t *rsp_len)
{
	associated_i2c_channel = i2c_chan;
	LOG_DBG("I3C CH%d: associated I2C channel = %u", channel, i2c_chan);
	build_i3c_response(channel, OBMF_ICP_STATUS_SUCCESS,
			   OBMF_I3C_CMD_SET_ASSOCIATED_I2C, 0, 0, rsp_buf, rsp_len);
}

/* ── IBI support ─────────────────────────────────────────────────────────── */

#define OBMF_I3C_IBI_MAX_DEVS   I3C_MAX_NUM
#define OBMF_I3C_IBI_BUF_SIZE   IBI_PAYLOAD_SIZE

struct obmf_ibi_ctx {
	struct i3c_dev_desc      *desc;
	struct k_work             work;
	struct i3c_ibi_payload    payload;
	struct i3c_ibi_callbacks  callbacks;
	uint8_t                   ibi_buf[OBMF_I3C_IBI_BUF_SIZE];
	uint64_t                  pid;
	uint8_t                   bus;
	bool                      active;
};

static struct obmf_ibi_ctx g_ibi_ctx[OBMF_I3C_IBI_MAX_DEVS];

static struct obmf_ibi_ctx *ibi_ctx_find(struct i3c_dev_desc *desc)
{
	for (int i = 0; i < OBMF_I3C_IBI_MAX_DEVS; i++) {
		if (g_ibi_ctx[i].active && g_ibi_ctx[i].desc == desc) {
			return &g_ibi_ctx[i];
		}
	}
	return NULL;
}

/*
 * Look up by PID, not descriptor or address: when the driver re-attaches a
 * device (e.g. after a Hot-Join DAA) it replaces the descriptor and may
 * assign a new dynamic address.
 */
static struct obmf_ibi_ctx *ibi_ctx_find_pid(uint8_t bus, uint64_t pid)
{
	for (int i = 0; i < OBMF_I3C_IBI_MAX_DEVS; i++) {
		if (g_ibi_ctx[i].active && g_ibi_ctx[i].bus == bus &&
		    g_ibi_ctx[i].pid == pid) {
			return &g_ibi_ctx[i];
		}
	}
	return NULL;
}

static struct obmf_ibi_ctx *ibi_ctx_alloc(struct i3c_dev_desc *desc, uint8_t bus)
{
	for (int i = 0; i < OBMF_I3C_IBI_MAX_DEVS; i++) {
		if (!g_ibi_ctx[i].active) {
			memset(&g_ibi_ctx[i], 0, sizeof(g_ibi_ctx[i]));
			g_ibi_ctx[i].desc   = desc;
			g_ibi_ctx[i].pid    = desc->info.pid;
			g_ibi_ctx[i].bus    = bus;
			g_ibi_ctx[i].active = true;
			return &g_ibi_ctx[i];
		}
	}
	return NULL;
}

static struct i3c_ibi_payload *ibi_write_requested(struct i3c_dev_desc *desc)
{
	struct obmf_ibi_ctx *ctx = ibi_ctx_find(desc);

	if (!ctx) {
		return NULL;
	}

	ctx->payload.buf              = ctx->ibi_buf;
	ctx->payload.size             = 0;
	ctx->payload.max_payload_size = sizeof(ctx->ibi_buf);

	return &ctx->payload;
}

static void ibi_write_done(struct i3c_dev_desc *desc)
{
	struct obmf_ibi_ctx *ctx = ibi_ctx_find(desc);

	if (ctx) {
		k_work_submit(&ctx->work);
	}
}

static void ibi_worker(struct k_work *work)
{
	struct obmf_ibi_ctx *ctx = CONTAINER_OF(work, struct obmf_ibi_ctx, work);

	/*
	 * Forward the raw IBI payload to the BMC.  The SMC does not interpret
	 * the MDB or perform any private read on its own — that is the BMC's
	 * responsibility.  The BMC will examine parameter_1 (dynamic_addr) and
	 * the payload bytes (MDB + any IBI data bytes the device sent) and may
	 * follow up with an I3C_CMD_EXECUTE_SEQUENCE private-read if needed.
	 *
	 * Packet layout per spec section 4.8.1:
	 *
	 *   OBMF common header           (OBMF_HEADER_SIZE bytes)
	 *   obmf_i3c_request_hdr_t       (OBMF_I3C_REQ_HDR_SIZE = 6 bytes):
	 *     Byte 0    command     = OBMF_I3C_EVT_IBI_NOTIFICATION (5)
	 *     Byte 1    parameter_1 = dynamic_addr (7-bit address of IBI source)
	 *     Bytes 2-3 parameter_2 = payload_len  (IBI MDB + data byte count)
	 *     Bytes 4-5 parameter_3 = pending_read = 0  (SMC does not read)
	 *   Bytes 6..   payload_data: raw IBI bytes (MDB first, then data bytes)
	 */
	uint16_t ibi_len  = (uint16_t)ctx->payload.size;
	uint32_t body_len = OBMF_I3C_REQ_HDR_SIZE + ibi_len;
	uint32_t total_len = OBMF_HEADER_SIZE + body_len;

	uint8_t *tx_buf = (uint8_t *)malloc(total_len);

	if (!tx_buf) {
		LOG_ERR("I3C IBI: failed to allocate notify buffer");
		return;
	}
	memset(tx_buf, 0, total_len);

	obmf_i3c_request_hdr_t *req =
		(obmf_i3c_request_hdr_t *)(tx_buf + OBMF_HEADER_SIZE);
	req->command     = OBMF_I3C_EVT_IBI_NOTIFICATION;
	req->parameter_1 = ctx->desc->info.dynamic_addr;
	req->parameter_2 = ibi_len; /* payload_len: IBI bytes (MDB + data) */
	req->parameter_3 = 0;       /* pending_read: BMC decides, not SMC */

	if (ibi_len > 0) {
		memcpy(tx_buf + OBMF_HEADER_SIZE + OBMF_I3C_REQ_HDR_SIZE,
		       ctx->ibi_buf, ibi_len);
	}

	uint8_t channel = i3c_channel_of_bus(ctx->bus);
	int retries = OBMF_REQUEST_MAX_RETRY;
	int rc;

	if (!channel) {
		LOG_ERR("I3C IBI: bus %u has no I3C Controller channel", ctx->bus);
		free(tx_buf);
		return;
	}

	do {
		rc = obmf_send_request(channel, tx_buf, total_len);
		if (rc == -EBUSY) {
			k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
		}
	} while (rc == -EBUSY && --retries > 0);

	if (rc != 0 && retries == 0) {
		LOG_ERR("I3C IBI: send notification failed after retries (dev@%02x): %d",
			ctx->desc->info.dynamic_addr, rc);
		channel_tags[channel].is_outstanding = false;
	} else if (rc != 0 && rc != -ENETDOWN) {
		LOG_ERR("I3C IBI: send notification failed (dev@%02x): %d",
			ctx->desc->info.dynamic_addr, rc);
	}

	free(tx_buf);
}

/**
 * @brief Register IBI callbacks for an I3C device and enable IBI.
 *
 * When the target device raises an IBI:
 *  1. ibi_write_requested() provides a buffer to receive the MDB.
 *  2. ibi_write_done() submits a work item.
 *  3. ibi_worker() performs an optional private SDR read (if the MDB indicates
 *     a pending read) then sends an OBMF_I3C_EVT_IBI_NOTIFICATION event to the
 *     BMC via obmf_send_request().
 *
 * @param bus          Physical I3C bus index (bound to an I3C Controller channel).
 * @param dynamic_addr 7-bit dynamic address of the target device.
 * @return 0 on success, negative errno on failure.
 */
int obmf_i3c_request_ibi(uint8_t bus, uint8_t dynamic_addr)
{
	if (bus >= I3C_MAX_NUM || !dev_i3c[bus]) {
		LOG_ERR("I3C request_ibi: invalid bus %u", bus);
		return -EINVAL;
	}

	/*
	 * Use i3c_master_get_desc() to obtain the driver-internal descriptor.
	 * The driver's IBI handler fires with its own internal desc pointer, so
	 * registering callbacks on our local i3c_descs[] copy would leave the
	 * driver unable to find the callback at IBI time.
	 */
	struct i3c_dev_desc *desc = i3c_master_get_desc(dev_i3c[bus], dynamic_addr);

	LOG_INF("request ibi for device @%02x desc %p", dynamic_addr, desc);
	if (!desc) {
		LOG_ERR("I3C request_ibi: dev@%02x not found on bus %u",
			dynamic_addr, bus);
		return -ENODEV;
	}

	/* Reuse existing context if already registered for this device. */
	struct obmf_ibi_ctx *ctx = ibi_ctx_find_pid(bus, desc->info.pid);

	if (ctx) {
		ctx->desc = desc;
	} else {
		ctx = ibi_ctx_alloc(desc, bus);
		if (!ctx) {
			LOG_ERR("I3C request_ibi: no free IBI context slots");
			return -ENOMEM;
		}
	}

	k_work_init(&ctx->work, ibi_worker);
	ctx->callbacks.write_requested = ibi_write_requested;
	ctx->callbacks.write_done      = ibi_write_done;

	int ret = i3c_master_request_ibi(desc, &ctx->callbacks);

	if (ret) {
		LOG_ERR("I3C request_ibi: i3c_master_request_ibi failed (dev@%02x): %d",
			dynamic_addr, ret);
		ctx->active = false;
		return ret;
	}

	ret = i3c_master_enable_ibi(desc);
	if (ret) {
		LOG_ERR("I3C request_ibi: i3c_master_enable_ibi failed (dev@%02x): %d",
			dynamic_addr, ret);
		ctx->active = false;
		return ret;
	}

	LOG_INF("I3C IBI registered and enabled for dev@%02x on bus %u",
		dynamic_addr, bus);

	/* Publish the target in the channel's IBI_TARGET_CONFIG_ARRAY */
	if (obmf_i3c_ibi_target_set(i3c_channel_of_bus(bus), dynamic_addr,
				    OBMF_I3C_IBI_BUF_SIZE)) {
		LOG_WRN("I3C dev@%02x: not listed in IBI_TARGET_CONFIG_ARRAY",
			dynamic_addr);
	}
	return 0;
}

/**
 * @brief Re-register IBI callbacks for every device registered on a bus.
 *
 * A Hot-Join DAA run by the driver re-attaches the device with a fresh
 * descriptor that carries no IBI callbacks, possibly at a new dynamic
 * address, so its IBIs would be dropped. Find each device by PID among the
 * attached descriptors (never through ctx->desc, which may have been freed)
 * and register again. Re-registering only sets the callback pointers, so it
 * is cheap enough to do before every request that may make the device raise
 * an IBI.
 */
static void ibi_ctx_refresh(uint8_t bus)
{
	for (uint8_t addr = 0; addr < I3C_MAX_ADDR; addr++) {
		struct i3c_dev_desc *desc = i3c_master_get_desc(dev_i3c[bus], addr);

		if (!desc)
			continue;

		struct obmf_ibi_ctx *ctx = ibi_ctx_find_pid(bus, desc->info.pid);

		if (!ctx)
			continue;

		ctx->desc = desc;
		i3c_master_request_ibi(desc, &ctx->callbacks);
	}
}

/* ── I3C_EVT_HOTJOIN_NOTIFICATION ──────────────────────────────────────── */

#ifdef i3c_master_register_hj_callback
/*
 * Called by the driver (system workqueue) after the DAA it runs for a
 * Hot-Join request. Re-register the IBIs of re-attached devices, then tell
 * the BMC, which re-enumerates the bus with DO_DAA. The event carries no
 * parameters or payload (spec section 4.8.1).
 */
static void i3c_hotjoin_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);

	uint8_t channel = (uint8_t)(uintptr_t)user_data;
	uint8_t tx_buf[OBMF_HEADER_SIZE + OBMF_I3C_REQ_HDR_SIZE] = { 0 };
	obmf_i3c_request_hdr_t *req =
		(obmf_i3c_request_hdr_t *)(tx_buf + OBMF_HEADER_SIZE);
	int retries = OBMF_REQUEST_MAX_RETRY;
	int rc;

	ibi_ctx_refresh(i3c_controller_bus[OBMF_I3C_CTRL_CHAN_IDX(channel)]);

	req->command = OBMF_I3C_EVT_HOTJOIN_NOTIFICATION;

	do {
		rc = obmf_send_request(channel, tx_buf, sizeof(tx_buf));
		if (rc == -EBUSY) {
			k_msleep(OBMF_REQUEST_RETRY_DELAY_MS);
		}
	} while (rc == -EBUSY && --retries > 0);

	if (rc != 0 && retries == 0) {
		LOG_ERR("I3C CH%d Hot-Join: send notification failed after retries: %d",
			channel, rc);
		channel_tags[channel].is_outstanding = false;
	} else if (rc != 0 && rc != -ENETDOWN) {
		LOG_ERR("I3C CH%d Hot-Join: send notification failed: %d", channel, rc);
	} else if (rc == 0) {
		LOG_INF("I3C CH%d Hot-Join: notified BMC", channel);
	}
}
#endif

/* ── I3C_EVT_IBI_NOTIFICATION response handler ─────────────────────────── */

/**
 * @brief Handle an incoming response on the I3C Controller Channel.
 *
 * The BMC acknowledges Producer-initiated events (IBI notifications, hot-join,
 * bus-error) with a response packet.  There is no payload to parse — we only
 * need to clear the outstanding-request flag so that subsequent notifications
 * can be sent.
 */
void handle_i3c_response(obmf_icp_header_t *rsp_hdr,
			 uint8_t *payload, uint32_t payload_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(rsp_hdr);

	channel_tags[channel].is_outstanding = false;
	LOG_DBG("I3C CH%d: response received (IBI ack)", channel);
}

/* ── Top-level request dispatcher ──────────────────────────────────────── */

/**
 * @brief Main I3C Controller Channel request handler.
 *
 * Called from the OBMF RX thread when the host sends an I3C request on
 * an I3C Controller channel.
 */
void handle_i3c_request(obmf_icp_header_t *req_hdr,
			uint8_t *payload, uint32_t payload_len,
			uint8_t *rsp_buf, uint32_t *rsp_len)
{
	uint8_t channel = OBMF_ICP_GET_CHANNEL(req_hdr);

	uint8_t bus = i3c_controller_bus[OBMF_I3C_CTRL_CHAN_IDX(channel)];

	if (bus == 0xFF) {
		LOG_ERR("I3C CH%d: bus not configured", channel);
		build_i3c_response(channel, OBMF_ICP_STATUS_INVALID_CMD,
				   0, 0, 0, rsp_buf, rsp_len);
		return;
	}

	if (!dev_i3c[bus]) {
		LOG_ERR("I3C CH%d: dev_i3c[%u] not initialised", channel, bus);
		build_i3c_response(channel, OBMF_ICP_STATUS_NOT_READY,
				   0, 0, 0, rsp_buf, rsp_len);
		return;
	}

	/* Minimum payload: command (1B) + parameter_1 (1B) */
	if (payload_len < 2) {
		LOG_ERR("I3C CH%d: request too short (%u)", channel, payload_len);
		build_i3c_response(channel, OBMF_ICP_STATUS_INVALID_CMD,
				   0, 0, 0, rsp_buf, rsp_len);
		return;
	}

	uint8_t command     = payload[0];
	uint8_t parameter_1 = payload[1];

	/* DO_DAA registers IBIs for the devices it assigns itself. */
	if (command != OBMF_I3C_CMD_DO_DAA)
		ibi_ctx_refresh(bus);

	switch (command) {

	case OBMF_I3C_CMD_EXECUTE_SEQUENCE:
		/* Operations array starts at Byte 2 (immediately after parameter_1) */
		handle_execute_sequence(channel, bus,
					parameter_1,
					payload + 2, payload_len - 2,
					rsp_buf, rsp_len);
		break;

	case OBMF_I3C_CMD_DO_DAA:
		handle_daa(channel, bus, parameter_1,
			   payload, payload_len, rsp_buf, rsp_len);
		break;

	case OBMF_I3C_CMD_BUS_RECOVERY:
		/* parameter_1 = action_type enum */
		handle_bus_recovery(channel, bus, parameter_1, rsp_buf, rsp_len);
		break;

	case OBMF_I3C_CMD_SET_ASSOCIATED_I2C: {
		/* parameter_2 (bytes 2-3) = i2c_channel number */
		if (payload_len < OBMF_I3C_REQ_HDR_SIZE) {
			build_i3c_response(channel, OBMF_ICP_STATUS_INVALID_CMD,
					   command, 0, 0, rsp_buf, rsp_len);
			break;
		}
		uint16_t i2c_chan;
		memcpy(&i2c_chan, payload + 2, sizeof(uint16_t));
		handle_set_associated_i2c(channel, i2c_chan, rsp_buf, rsp_len);
		break;
	}

	case OBMF_I3C_CMD_GET_STATS:
		handle_get_stats(channel, rsp_buf, rsp_len);
		break;

	default:
		LOG_ERR("I3C CH%d: unknown command 0x%02x", channel, command);
		build_i3c_response(channel, OBMF_ICP_STATUS_INVALID_CMD,
				   command, 0, 0, rsp_buf, rsp_len);
		break;
	}
}
