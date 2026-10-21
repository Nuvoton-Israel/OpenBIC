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

#ifndef PLAT_CLASS_H
#define PLAT_CLASS_H

#include <stdbool.h>
#include <stdint.h>

enum SLOT_EID {
	SLOT1_EID = 0x0A,
	SLOT2_EID = 0x0B,
	SLOT3_EID = 0x0C,
	SLOT4_EID = 0x0D,
	SLOT5_EID = 0x0E,
	SLOT6_EID = 0x0F,
	SLOT7_EID = 0x10,
	SLOT8_EID = 0x11,
};

enum SLOT_PID {
	SLOT1_PID = 0x0607,
	SLOT2_PID = 0x0608,
	SLOT3_PID = 0x0609,
	SLOT4_PID = 0x060A,
	SLOT5_PID = 0x060B,
	SLOT6_PID = 0x060C,
	SLOT7_PID = 0x060D,
	SLOT8_PID = 0x060E,
};

void init_platform_config(void);

#endif
