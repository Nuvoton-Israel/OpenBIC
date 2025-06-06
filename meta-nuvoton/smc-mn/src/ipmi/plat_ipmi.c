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
#include <stdlib.h>
#include <logging/log.h>
#include <sys/crc.h>

#include "libutil.h"
#include "ipmb.h"
#include "ipmi.h"
#include "plat_ipmi.h"

bool pal_request_msg_to_BIC_from_HOST(uint8_t netfn, uint8_t cmd)
{
	return true;
}
