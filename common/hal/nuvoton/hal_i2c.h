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

#ifndef HAL_I2C_H
#define HAL_I2C_H

#include <drivers/i2c.h>
#include <drivers/i2c/slave/ipmb.h>

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c1a), okay)
#define DEV_I2C_0
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c1b), okay)
#define DEV_I2C_0
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c3a), okay)
#define DEV_I2C_1
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c4a), okay)
#define DEV_I2C_2
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c5a), okay)
#define DEV_I2C_3
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c6a), okay)
#define DEV_I2C_4
#endif

#define DEV_I2C(n) DEV_I2C_##n

#define I2C_BUFF_SIZE 256
#define MUTEX_LOCK_ENABLE true
#define MUTEX_LOCK_DISENABLE false

enum I2C_TRANSFER_TYPE {
	I2C_READ,
	I2C_WRITE,
};

typedef struct _I2C_MSG_ {
	uint8_t bus;
	uint8_t target_addr;
	uint8_t rx_len;
	uint8_t tx_len;
	uint8_t data[I2C_BUFF_SIZE];
	struct k_mutex lock;
} I2C_MSG;

int i2c_freq_set(uint8_t i2c_bus, uint8_t i2c_speed_mode, uint8_t en_slave);
int i2c_addr_set(uint8_t i2c_bus, uint8_t i2c_addr);
int i2c_master_read(I2C_MSG *msg, uint8_t retry);
int i2c_master_read_without_mutex(I2C_MSG *msg, uint8_t retry);
int i2c_master_write(I2C_MSG *msg, uint8_t retry);
int i2c_master_write_without_mutex(I2C_MSG *msg, uint8_t retry);
void i2c_scan(uint8_t bus, uint8_t *target_addr, uint8_t *target_addr_len);
void util_init_I2C(void);
int check_i2c_bus_valid(uint8_t bus);

#endif
