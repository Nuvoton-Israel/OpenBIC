/*
 * Copyright (c) 2025 The OpenBIC Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PLAT_OBMF_H
#define PLAT_OBMF_H

/* Start the OBMF service with this platform's channel table (plat_obmf.c) */
void plat_obmf_init(void);

/* i3c buses used by the OBMF I3C Controller channels (ch18, ch19) */
#define I3C_BUS4_CONTROLLER_TO_OBMF	4
#define I3C_BUS5_CONTROLLER_TO_OBMF	5

#endif
