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

#ifndef PLAT_GPIO_H
#define PLAT_GPIO_H

#include "hal_gpio.h"

// gpio_cfg(chip, number, is_init, direction, status, int_type, int_callback)
// dedicate gpio A0~A7, B0~B7, C0~C7, D0~D7, E0~E7, total 40 gpios
// Default name: Reserve_GPIOH0

// clang-format off

#define name_gpio0 \
	gpio_name_to_num(ASIC_DEV_RST_N) \
	gpio_name_to_num(ASIC_PERST0_N) \
	gpio_name_to_num(ASIC_PERST1_N) \
	gpio_name_to_num(ASIC_FAIL_N) \
	gpio_name_to_num(ASIC_EVENT_N) \
	gpio_name_to_num(ASIC_DUALPORTEN_N) \
	gpio_name_to_num(JTAG2_BIC_ASIC_NTRST2) \
	gpio_name_to_num(ASIC_TAP_SEL)

#define name_gpio1 \
	gpio_name_to_num(ASIC_CPU_BOOT_0) \
	gpio_name_to_num(ASIC_CPU_BOOT_1) \
	gpio_name_to_num(ASIC_M_SCAN_PCAP_SEL) \
	gpio_name_to_num(ASIC_GPIO_R_0) \
	gpio_name_to_num(ASIC_GPIO_R_1) \
	gpio_name_to_num(AUX_PWR_EN_4C) \
	gpio_name_to_num(I2CS_SRSTB_GPIO) \
	gpio_name_to_num(FM_ISOLATED_EN_N)

#define name_gpio2 \
	gpio_name_to_num(FM_P0V8_ASICD_EN) \
	gpio_name_to_num(P1V8_ASIC_EN_R) \
	gpio_name_to_num(FM_P0V8_ASICA_EN) \
	gpio_name_to_num(PVTT_AB_EN_R) \
	gpio_name_to_num(PVTT_CD_EN_R) \
	gpio_name_to_num(FM_P0V9_ASICA_EN) \
	gpio_name_to_num(PVPP_CD_EN_R) \
	gpio_name_to_num(FM_PVDDQ_AB_EN)

#define name_gpio3 \
	gpio_name_to_num(PVPP_AB_EN_R) \
	gpio_name_to_num(FM_PVDDQ_CD_EN) \
	gpio_name_to_num(SLOT_ID0) \
	gpio_name_to_num(PVPP_CD_PG_R) \
	gpio_name_to_num(P0V8_ASICA_PWRGD) \
	gpio_name_to_num(PVTT_AB_PG_R) \
	gpio_name_to_num(SMB_SENSOR_LVC3_ALERT_N) \
	gpio_name_to_num(PVTT_CD_PG_R)

#define name_gpio4 \
	gpio_name_to_num(FM_POWER_EN) \
	gpio_name_to_num(PWRGD_CARD_PWROK) \
	gpio_name_to_num(RST_MB_N) \
	gpio_name_to_num(SPI_MASTER_SEL) \
	gpio_name_to_num(FM_SPI_MUX_OE_CTL_N) \
	gpio_name_to_num(SMB_12V_INA_ALRT_N) \
	gpio_name_to_num(SMB_3V3_INA_ALRT_N) \
	gpio_name_to_num(FM_MEM_THERM_EVENT_LVT3_N)

#define name_gpio5 \
	gpio_name_to_num(SPI_RST_FLASH_N) \
	gpio_name_to_num(SMBUS_ALERT_R_N) \
	gpio_name_to_num(LSFT_SMB_DIMM_EN) \
	gpio_name_to_num(P0V9_ASICA_PWRGD) \
	gpio_name_to_num(P1V8_ASIC_PG_R) \
	gpio_name_to_num(JTAG2_ASIC_PORT_SEL_EN_R) \
	gpio_name_to_num(SAVE_N_BIC) \
	gpio_name_to_num(FM_ADR_COMPLETE_DLY)

#define name_gpio6 \
	gpio_name_to_num(P5V_STBY_PG) \
	gpio_name_to_num(PVPP_AB_PG_R) \
	gpio_name_to_num(P1V2_STBY_PG_R) \
	gpio_name_to_num(SLOT_ID1) \
	gpio_name_to_num(SMB_VR_PVDDQ_CD_ALERT_N) \
	gpio_name_to_num(P0V8_ASICD_PWRGD) \
	gpio_name_to_num(PWRGD_PVDDQ_CD) \
	gpio_name_to_num(SMB_VR_PASICA_ALERT_N)

#define name_gpio7 \
	gpio_name_to_num(JTAG2_BIC_SHIFT_EN) \
	gpio_name_to_num(SMB_VR_PVDDQ_AB_ALERT_N) \
	gpio_name_to_num(SPI_BIC_SHIFT_EN) \
	gpio_name_to_num(PWRGD_PVDDQ_AB) \
	gpio_name_to_num(Reserve_GPIOH4) \
	gpio_name_to_num(Reserve_GPIOH5) \
	gpio_name_to_num(Reserve_GPIOH6) \
	gpio_name_to_num(Reserve_GPIOH7)

#define name_gpio8 \
	gpio_name_to_num(Reserve_GPIOI0) \
	gpio_name_to_num(Reserve_GPIOI1) \
	gpio_name_to_num(Reserve_GPIOI2) \
	gpio_name_to_num(Reserve_GPIOI3) \
	gpio_name_to_num(P0V9_ASICA_FT_R) \
	gpio_name_to_num(PVDDQ_AB_FT_R) \
	gpio_name_to_num(PVDDQ_CD_FT_R) \
	gpio_name_to_num(FM_PWRBRK_PRIMARY_R_N)

#define name_gpio9 \
	gpio_name_to_num(Reserve_GPIOJ0) \
	gpio_name_to_num(Reserve_GPIOJ1) \
	gpio_name_to_num(P0V8_ASICD_FT_R) \
	gpio_name_to_num(P0V8_ASICA_FT_R) \
	gpio_name_to_num(Reserve_GPIOJ4) \
	gpio_name_to_num(Reserve_GPIOJ5) \
	gpio_name_to_num(Reserve_GPIOJ6) \
	gpio_name_to_num(Reserve_GPIOJ7)

#define name_gpioA \
	gpio_name_to_num(Reserve_GPIOK0) \
	gpio_name_to_num(Reserve_GPIOK1) \
	gpio_name_to_num(Reserve_GPIOK2) \
	gpio_name_to_num(Reserve_GPIOK3) \
	gpio_name_to_num(Reserve_GPIOK4) \
	gpio_name_to_num(Reserve_GPIOK5) \
	gpio_name_to_num(Reserve_GPIOK6) \
	gpio_name_to_num(Reserve_GPIOK7)

#define name_gpioB \
	gpio_name_to_num(Reserve_GPIOL0) \
	gpio_name_to_num(Reserve_GPIOL1) \
	gpio_name_to_num(LED_CXL_POWER) \
	gpio_name_to_num(FM_BOARD_REV_ID2) \
	gpio_name_to_num(FM_BOARD_REV_ID1) \
	gpio_name_to_num(FM_BOARD_REV_ID0) \
	gpio_name_to_num(BOARD_ID0) \
	gpio_name_to_num(BOARD_ID1)

// GPIOM6, M7 hardware not define
#define name_gpioC \
	gpio_name_to_num(BIC_SECUREBOOT) \
	gpio_name_to_num(BOARD_ID2) \
	gpio_name_to_num(BOARD_ID3) \
	gpio_name_to_num(BIC_ESPI_SELECT) \
	gpio_name_to_num(LED_CXL_FAULT) \
	gpio_name_to_num(Reserve_GPIOM5) \
	gpio_name_to_num(Reserve_GPIOM6) \
	gpio_name_to_num(Reserve_GPIOM7)

#define name_gpioD \
	gpio_name_to_num(Reserve_GPION0) \
	gpio_name_to_num(Reserve_GPION1) \
	gpio_name_to_num(Reserve_GPION2) \
	gpio_name_to_num(CLK_100M_OSC_EN) \
	gpio_name_to_num(Reserve_GPION4) \
	gpio_name_to_num(Reserve_GPION5) \
	gpio_name_to_num(Reserve_GPION6) \
	gpio_name_to_num(Reserve_GPION7)

#define name_gpioE \
	gpio_name_to_num(Reserve_GPIOO0) \
	gpio_name_to_num(Reserve_GPIOO1) \
	gpio_name_to_num(Reserve_GPIOO2) \
	gpio_name_to_num(Reserve_GPIOO3) \
	gpio_name_to_num(Reserve_GPIOO4) \
	gpio_name_to_num(Reserve_GPIOO5) \
	gpio_name_to_num(Reserve_GPIOO6) \
	gpio_name_to_num(Reserve_GPIOO7)

#define name_gpioF \
	gpio_name_to_num(Reserve_GPIOP0) \
	gpio_name_to_num(Reserve_GPIOP1) \
	gpio_name_to_num(Reserve_GPIOP2) \
	gpio_name_to_num(Reserve_GPIOP3) \
	gpio_name_to_num(Reserve_GPIOP4) \
	gpio_name_to_num(Reserve_GPIOP5) \
	gpio_name_to_num(Reserve_GPIOP6) \
	gpio_name_to_num(Reserve_GPIOP7)

// clang-format on

#define gpio_name_to_num(x) x,
enum _GPIO_NUMS_ {
	name_gpio0 name_gpio1 name_gpio2 name_gpio3 name_gpio4 name_gpio5 name_gpio6 name_gpio7
		name_gpio8 name_gpio9 name_gpioA name_gpioB name_gpioC name_gpioD name_gpioE
			name_gpioF
};

extern enum _GPIO_NUMS_ GPIO_NUMS;
#undef gpio_name_to_num

extern char *gpio_name[];

#endif
