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

#ifndef PLAT_I3C_H
#define PLAT_I3C_H

#define NPCM_I3C_BUS_MAX	6
#define IBI_MDB_SIZE		8

#define LDO_VOLT										\
	V_LDO_SETTING(rg3mxxb12_ldo_1_8_volt, rg3mxxb12_ldo_1_8_volt, rg3mxxb12_ldo_1_8_volt,	\
			rg3mxxb12_ldo_1_8_volt)

typedef struct _npcm_i3c_ibi_dev {
	uint8_t data_mdb[IBI_MDB_SIZE];
	uint8_t data_rx[I3C_MAX_DATA_SIZE];
	struct i3c_ibi_payload i3c_payload;
	struct k_sem ibi_complete;
} npcm_i3c_ibi_dev;

#endif
