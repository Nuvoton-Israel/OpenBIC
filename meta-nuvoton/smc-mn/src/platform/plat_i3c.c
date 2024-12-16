/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zephyr.h>
#include <logging/log.h>
#include "libutil.h"
#include "hal_i3c.h"
#include "plat_mctp.h"
#include "plat_i3c.h"

#ifdef TEST_I3C_CONTROLLER_BIC

LOG_MODULE_REGISTER(plat_i3c);

extern const struct device *dev_i3c[I3C_MAX_NUM];
extern struct i3c_dev_desc i3c_desc_table[I3C_MAX_NUM];
extern int i3c_desc_count;
extern struct k_mutex mutex_dev[I3C_MAX_NUM];

npcm_i3c_ibi_dev npcm_i3c_ibi_dev_table[I3C_MAX_NUM];

bool npcm_i3c_bus_rstdaa[NPCM_I3C_BUS_MAX] = {false};

static int npcm_get_bus_id(struct i3c_dev_desc *desc)
{
	char i3c_dev_name[I3C_DEV_STR_LEN] = { 0 };
	const char delim[2] = "_";
	char *saveptr = NULL;
	char *substr = NULL;
	int bus_id;

	strncpy(i3c_dev_name, desc->bus->name, I3C_DEV_STR_LEN);

	substr = strtok_r(i3c_dev_name, delim, &saveptr);
	substr = strtok_r(NULL, delim, &saveptr);

	bus_id = atoi(substr);
	if (bus_id > I3C_MAX_NUM) {
		LOG_ERR("bus id out of range, bus = %d", bus_id);
		return -1;
	}

	return bus_id;
}

static struct i3c_dev_desc *npcm_find_matching_desc(const struct device *dev, uint8_t desc_addr,
					       int *pos)
{
	CHECK_NULL_ARG_WITH_RETURN(dev, NULL);

	struct i3c_dev_desc *desc = NULL;
	int i = 0;

	for (i = 0; i < I3C_MAX_NUM; i++) {
		desc = &i3c_desc_table[i];
		if ((desc->bus == dev) && (desc->info.dynamic_addr == desc_addr)) {
			if (pos == NULL) {
				return desc;
			}
			*pos = i;
			return desc;
		}
	}

	return NULL;
}

static int npcm_find_dev_i3c_idx(struct i3c_dev_desc *desc)
{
	int bus_id = npcm_get_bus_id(desc);
	if (bus_id < 0) {
		return -1;
	}

	int i = 0;
	struct i3c_dev_desc *desc_ptr = NULL;
	for (i = 0; i < i3c_desc_count; i++) {
		desc_ptr = &i3c_desc_table[i];

		int table_bus_id = npcm_get_bus_id(desc_ptr);
		if (table_bus_id < 0) {
			return -1;
		}

		if ((table_bus_id == bus_id) &&
			(desc->info.dynamic_addr == desc_ptr->info.dynamic_addr)) {
			return i;
		}
	}

	LOG_ERR("Not found id in i3c table");

	return -1;
}

int i3c_attach(I3C_MSG *msg)
{
	CHECK_NULL_ARG_WITH_RETURN(msg, -EINVAL);

	int ret = 0, pos = 0;
	struct i3c_dev_desc *desc = NULL;

	if (!dev_i3c[msg->bus]) {
		LOG_ERR("Failed to attach address 0x%x due to undefined bus%u", msg->target_addr,
			msg->bus);
		return -ENODEV;
	}

	ret = k_mutex_lock(&mutex_dev[msg->bus], K_MSEC(2000));
	if (ret) {
		LOG_ERR("Failed to lock the mutex(%d), %s", ret, __func__);
		return -1;
	}

	desc = npcm_find_matching_desc(dev_i3c[msg->bus], msg->target_addr, &pos);
	if (desc != NULL) {
		LOG_INF("addr 0x%x already attach, update it", msg->target_addr);
		ret = i3c_master_detach_device(dev_i3c[msg->bus], desc);
		if (ret == 0) {
			i3c_desc_count--;
		} else {
			LOG_ERR("Failed to update address 0x%x, ret: %d", msg->target_addr, ret);
			k_mutex_unlock(&mutex_dev[msg->bus]);
			return -1;
		}
	} else {
		desc = &i3c_desc_table[i3c_desc_count];
		desc->info.assigned_dynamic_addr = msg->target_addr;
		desc->info.static_addr = desc->info.assigned_dynamic_addr;
		desc->info.i2c_mode = 0;
	}

	ret = i3c_master_attach_device(dev_i3c[msg->bus], desc);
	if (ret == 0) {
		i3c_desc_count++;
	} else {
		LOG_ERR("Failed to attach address 0x%x, ret: %d", msg->target_addr, ret);
	}

	k_mutex_unlock(&mutex_dev[msg->bus]);
	return -ret;
}

int i3c_detach(I3C_MSG *msg)
{
	CHECK_NULL_ARG_WITH_RETURN(msg, -EINVAL);

	int ret = 0, pos = 0, i;
	struct i3c_dev_desc *desc;

	if (!dev_i3c[msg->bus]) {
		LOG_ERR("Failed to detach address 0x%x due to undefined bus%u", msg->target_addr,
			msg->bus);
		return -ENODEV;
	}

	ret = k_mutex_lock(&mutex_dev[msg->bus], K_MSEC(2000));
	if (ret) {
		LOG_ERR("Failed to lock the mutex(%d), %s", ret, __func__);
		return -1;
	}

	desc = npcm_find_matching_desc(dev_i3c[msg->bus], msg->target_addr, &pos);
	if (desc == NULL) {
		LOG_ERR("Failed to detach address 0x%x due to unknown address", msg->target_addr);
		k_mutex_unlock(&mutex_dev[msg->bus]);
		return -ENODEV;
	}

	ret = i3c_master_detach_device(dev_i3c[msg->bus], desc);
	if (ret != 0) {
		LOG_ERR("Failed to detach address 0x%x, ret: %d", msg->target_addr, ret);
		k_mutex_unlock(&mutex_dev[msg->bus]);
		return -ret;
	}

	for (i = pos; i < i3c_desc_count; i++) {
		i3c_desc_table[i] = i3c_desc_table[i + 1];
	}
	i3c_desc_count--;

	k_mutex_unlock(&mutex_dev[msg->bus]);
	return -ret;
}

static struct i3c_ibi_payload *npcm_ibi_write_requested(struct i3c_dev_desc *desc)
{
	int idx = npcm_find_dev_i3c_idx(desc);
	if (idx < 0) {
		LOG_ERR("%s: find dev i3c idx failed. idx = %d", __func__, idx);
	}
	npcm_i3c_ibi_dev_table[idx].i3c_payload.max_payload_size = IBI_MDB_SIZE;
	npcm_i3c_ibi_dev_table[idx].i3c_payload.size = 0;
	npcm_i3c_ibi_dev_table[idx].i3c_payload.buf = npcm_i3c_ibi_dev_table[idx].data_mdb;

	return &npcm_i3c_ibi_dev_table[idx].i3c_payload;
}

static void npcm_ibi_write_done(struct i3c_dev_desc *desc)
{
	int idx = npcm_find_dev_i3c_idx(desc);
	if (idx < 0) {
		LOG_ERR("%s: find dev i3c idx failed. idx = %d", __func__, idx);
	}
	k_sem_give(&npcm_i3c_ibi_dev_table[idx].ibi_complete);
}

static struct i3c_ibi_callbacks npcm_i3c_ibi_def_callbacks = {
	.write_requested = npcm_ibi_write_requested,
	.write_done = npcm_ibi_write_done,
};

int i3c_controller_ibi_init(I3C_MSG *msg)
{
	/* only support i3c spec 1.0, need set dasa to assign static addr to dynamic */
	struct i3c_ccc_cmd ccc;
	uint8_t address;
	struct i3c_dev_desc *target;
	int ret;

	CHECK_NULL_ARG_WITH_RETURN(msg, -EINVAL);

	if (!dev_i3c[msg->bus]) {
		LOG_ERR("Failed to receive messages to address 0x%x due to undefined bus%u",
			msg->target_addr, msg->bus);
		return -ENODEV;
	}

	ret = k_mutex_lock(&mutex_dev[msg->bus], K_MSEC(2000));
	if (ret) {
		LOG_ERR("Failed to lock the mutex(%d), %s", ret, __func__);
		return -1;
	}

	target = npcm_find_matching_desc(dev_i3c[msg->bus], msg->target_addr, NULL);
	if (target == NULL) {
		LOG_ERR("Failed to reveive messages to address 0x%x due to unknown address",
			msg->target_addr);
		k_mutex_unlock(&mutex_dev[msg->bus]);
		return -ENODEV;
	}

	int idx = npcm_find_dev_i3c_idx(target);
	if (idx < 0) {
		LOG_ERR("%s: find dev i3c idx failed. idx = %d", __func__, idx);
	}

	if (msg->bus > NPCM_I3C_BUS_MAX) {
		LOG_ERR("Bus number exceed 0x%x msg->bus 0x%x", NPCM_I3C_BUS_MAX, msg->bus);
		k_mutex_unlock(&mutex_dev[msg->bus]);
		return -EINVAL;
	}

	/* Initial ibi complete semaphore */
	k_sem_init(&npcm_i3c_ibi_dev_table[idx].ibi_complete, 0, 1);

	if (npcm_i3c_bus_rstdaa[msg->bus] == false) {
		ret = i3c_master_send_rstdaa(dev_i3c[msg->bus]);
		if (ret) {
			LOG_ERR("Failed to send rstdaa");
		}

		ret = i3c_master_send_rstdaa(dev_i3c[msg->bus]);
		if (ret) {
			LOG_ERR("Failed to 2nd send rstdaa");
		}

		npcm_i3c_bus_rstdaa[msg->bus] = true;
	}

	address = msg->target_addr << 1;

	ccc.addr = msg->target_addr;
	ccc.id = I3C_CCC_SETDASA;
	ccc.rnw = I3C_WRITE_CMD;
	ccc.payload.data = &address;
	ccc.payload.length = 1;

	ret = i3c_master_send_ccc(dev_i3c[msg->bus], &ccc);
	if (ret != 0) {
		LOG_ERR("Failed to send dasa");
	}

	ret = i3c_master_send_getpid(dev_i3c[msg->bus], target->info.dynamic_addr,
				     &target->info.pid);
	if (ret) {
		LOG_ERR("Failed to get pid");
	}

	ret = i3c_master_send_getbcr(dev_i3c[msg->bus], target->info.dynamic_addr,
				     &target->info.bcr);
	if (ret) {
		LOG_ERR("Failed to get bcr");
	}

	ccc.addr = msg->target_addr;
	ccc.id = I3C_CCC_GETDCR;
	ccc.rnw = I3C_WRITE_CMD;
	ccc.payload.data = &target->info.dcr;
	ccc.payload.length = 1;

	ret = i3c_master_send_ccc(dev_i3c[msg->bus], &ccc);
	if (ret != 0) {
		LOG_ERR("Failed to get dcr");
	}

	k_mutex_unlock(&mutex_dev[msg->bus]);

	/* Attach again to update PID/BCR/DCR for ENTDAA */
	i3c_attach(msg);

	ret = k_mutex_lock(&mutex_dev[msg->bus], K_MSEC(2000));
	if (ret) {
		LOG_ERR("Failed to lock the mutex(%d) after update, %s", ret, __func__);
		i3c_detach(msg);
		return -1;
	}

	ret = i3c_master_request_ibi(target, &npcm_i3c_ibi_def_callbacks);
	if (ret != 0) {
		LOG_ERR("Failed to request SIR, bus = %x, addr = %u", msg->target_addr, msg->bus);
	}

	ret = i3c_master_enable_ibi(target);
	if (ret != 0) {
		LOG_ERR("Failed to enable SIR, bus = %x, addr = %u", msg->target_addr, msg->bus);
	}

	k_mutex_unlock(&mutex_dev[msg->bus]);

	return -ret;
}

int i3c_controller_ibi_read(I3C_MSG *msg)
{
	CHECK_NULL_ARG_WITH_RETURN(msg, -EINVAL);

	if (!dev_i3c[msg->bus]) {
		LOG_ERR("Failed to receive messages to address 0x%x due to undefined bus%u",
			msg->target_addr, msg->bus);
		return -ENODEV;
	}

	struct i3c_dev_desc *target;
	target = npcm_find_matching_desc(dev_i3c[msg->bus], msg->target_addr, NULL);
	if (target == NULL) {
		LOG_ERR("Failed to reveive messages to address 0x%x due to unknown address",
			msg->target_addr);
		return -ENODEV;
	}

	int idx = npcm_find_dev_i3c_idx(target);
	if (idx < 0) {
		LOG_ERR("%s: find dev i3c idx failed. idx = %d", __func__, idx);
	}

	/* master device waits for the IBI from the target */
	k_sem_take(&npcm_i3c_ibi_dev_table[idx].ibi_complete, K_FOREVER);

	/* init the flag for the next loop */
	k_sem_init(&npcm_i3c_ibi_dev_table[idx].ibi_complete, 0, 1);

	/* check result: first byte (MDB) shall match the DT property mandatory-data-byte */
	if (IS_MDB_PENDING_READ_NOTIFY(npcm_i3c_ibi_dev_table[idx].data_mdb[0])) {
		struct i3c_priv_xfer xfer;

		/* initiate a private read transfer to read the pending data */
		xfer.rnw = 1;
		xfer.len = IBI_PAYLOAD_SIZE;
		xfer.data.in = npcm_i3c_ibi_dev_table[idx].data_rx;
		k_yield();
		int ret = k_mutex_lock(&mutex_dev[msg->bus], K_MSEC(2000));
		if (ret) {
			LOG_ERR("Failed to lock the mutex(%d), %s", ret, __func__);
			return -1;
		}
		ret = i3c_master_priv_xfer(target, &xfer, 1);
		if (ret) {
			LOG_ERR("ibi read failed. ret = %d", ret);
		}
		msg->rx_len = xfer.len;
		memcpy(msg->data, npcm_i3c_ibi_dev_table[idx].data_rx, IBI_PAYLOAD_SIZE);
		k_mutex_unlock(&mutex_dev[msg->bus]);
	}

	return msg->rx_len;
}

int i3c_controller_write(I3C_MSG *msg)
{
	CHECK_NULL_ARG_WITH_RETURN(msg, -EINVAL);

	int ret = k_mutex_lock(&mutex_dev[msg->bus], K_MSEC(2000));
	if (ret) {
		LOG_ERR("Failed to lock the mutex(%d), %s", ret, __func__);
		return -1;
	}

	int *pos = NULL;
	struct i3c_dev_desc *target;
	target = npcm_find_matching_desc(dev_i3c[msg->bus], msg->target_addr, pos);
	if (target == NULL) {
		LOG_ERR("Failed to write messages to address 0x%x due to unknown address",
			msg->target_addr);
		k_mutex_unlock(&mutex_dev[msg->bus]);
		return -ENODEV;
	}

	struct i3c_priv_xfer xfer[1];

	xfer[0].rnw = I3C_WRITE_CMD;
	xfer[0].len = msg->tx_len;
	xfer[0].data.out = &msg->data;

	ret = i3c_master_priv_xfer(target, xfer, 1);
	if (ret != 0) {
		LOG_ERR("Failed to write messages to bus 0x%d addr 0x%x, ret: %d", msg->bus,
			msg->target_addr, ret);
		k_mutex_unlock(&mutex_dev[msg->bus]);
		return -1;
	}

	k_mutex_unlock(&mutex_dev[msg->bus]);
	return 0;
}

#endif
