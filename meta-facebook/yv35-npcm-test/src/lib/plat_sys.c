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
#include "hal_wdt.h"
#include <drivers/watchdog.h>

/* give a chance since we may need upgrade fw before reboot system */
#define REBOOT_WDT_TIMEOUT (30 * 1000) /* 30s */

#ifdef CONFIG_XIP
static void pal_wdt_reset(void)
{
	const struct device *wdt_dev = NULL;
	struct wdt_timeout_cfg wdt_config;
	int ret = 0;

	wdt_dev = device_get_binding(WDT_DEVICE_NAME);
	if (wdt_dev) {
		/* disable wdt handler thread feed */
		set_wdt_continue_feed(0);

		/* disable current wdt first */
		wdt_disable(wdt_dev);

		/* re-init wdt timeout */
		wdt_config.window.min = 0U;
		wdt_config.window.max = REBOOT_WDT_TIMEOUT;
		wdt_config.callback = NULL;

		ret = wdt_install_timeout(wdt_dev, &wdt_config);
		if (ret != 0) {
			printk("wdt install failed\n");
			return;
		}

		ret = wdt_setup(wdt_dev, WDT_FLAG_RESET_CPU_CORE);
		if (ret != 0) {
			printk("wdt setup failed\n");
			return;
		}
		/* after configuration done, feed once */
		wdt_feed(wdt_dev, 0);
        }
}
#endif

void pal_warm_reset_prepare(void)
{
#ifdef CONFIG_XIP
	/* increase wdt timeout since we may need upgrade fw 
	 * before system reboot.
	*/
	pal_wdt_reset();
#endif
}

void pal_cold_reset_prepare(void)
{
#ifdef CONFIG_XIP
	/* increase wdt timeout since we may need upgrade fw
	 * before system reboot.
	*/
        pal_wdt_reset();
#endif
}
