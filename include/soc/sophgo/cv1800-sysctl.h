/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2023 Inochi Amaoto <inochiama@outlook.com>
 */

#ifndef CV1800_SYSCTL_H
#define CV1800_SYSCTL_H

/*
 * SOPHGO CV1800/SG2000 SoC top system controller registers offsets.
 */

#define CV1800_CONF_INFO		0x004
#define CV1800_SYS_CTRL_REG		0x008
#define CV1800_USB_PHY_CTRL_REG		0x048
#define CV1800_SDMA_DMA_CHANNEL_REMAP0	0x154
#define CV1800_SDMA_DMA_CHANNEL_REMAP1	0x158
#define CV1800_TOP_TIMER_CLK_SEL	0x1a0
#define CV1800_TOP_WDT_CTRL		0x1a8
#define CV1800_DDR_AXI_URGENT_OW	0x1b8
#define CV1800_DDR_AXI_URGENT		0x1bc
#define CV1800_DDR_AXI_QOS_0		0x1d8
#define CV1800_DDR_AXI_QOS_1		0x1dc
#define CV1800_SD_PWRSW_CTRL		0x1f4
#define CV1800_SD_PWRSW_TIME		0x1f8
#define CV1800_DDR_AXI_QOS_OW		0x23c
#define CV1800_SD_CTRL_OPT		0x294
#define CV1800_SDMA_DMA_INT_MUX		0x298

#endif // CV1800_SYSCTL_H
