/*
 * Copyright (c) 2024, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date              Notes
 * 2024-09-03        the first version
 */

#ifndef __C145HAX01_H__
#define __C145HAX01_H__

#include <aic_hal_gpio.h>
#include <aic_drv_gpio.h>
#include "drivers/touch.h"

#define C145HAX01_SALVE_ADDR        0x38
#define C145HAX01_REGITER_LEN       1
#define C145HAX01_MAX_TOUCH         5
#define C145HAX01_FIFO_TOUCH_SLOTS  10
#define C145HAX01_POINT_INFO_LEN    6
/* The 14.5-inch panel uses a complete 63-byte FocalTech FIFO report. */
#define C145HAX01_REPORT_LEN        63
#define C145HAX01_I2C_RETRY_NUMBER  3

#ifdef AIC_SCREEN_UNIFIED_ORIENTATION
#define C145HAX01_TOUCH_X_RANGE     AIC_SCREEN_LOGICAL_WIDTH
#define C145HAX01_TOUCH_Y_RANGE     AIC_SCREEN_LOGICAL_HEIGHT
#elif defined(AIC_SCREEN_VMAXC59_14_5) || defined(AIC_SCREEN_VMAXC54_12_6)
#define C145HAX01_TOUCH_X_RANGE     2560
#define C145HAX01_TOUCH_Y_RANGE     720
#else
#define C145HAX01_TOUCH_X_RANGE     AIC_TOUCH_X_COORDINATE_RANGE
#define C145HAX01_TOUCH_Y_RANGE     AIC_TOUCH_Y_COORDINATE_RANGE
#endif

#if defined(AIC_SCREEN_VMAXC59_14_5) || defined(AIC_SCREEN_VMAXC54_12_6)
#define C145HAX01_DEVICE_NAME       "c145hax01"
#else
#define C145HAX01_DEVICE_NAME       AIC_TOUCH_PANEL_NAME
#endif

#define C145HAX01_DEVICE_MODE       0x00
#define C145HAX01_GEST_ID           0x01
#define C145HAX01_TD_STATUS         0x02
#define C145HAX01_POINT1_XH         0x03
#define C145HAX01_POINT1_XL         0x04
#define C145HAX01_POINT1_YH         0x05
#define C145HAX01_POINT1_YL         0x06
#define C145HAX01_POINT1_W          0x07
#define C145HAX01_POINT1_MISC       0x08


int rt_hw_cst3556_init(const char *name, struct rt_touch_config *cfg);
extern volatile bool s_in_display_mode;
extern bool touch_data_enabled;
extern void send_touch_data(int8_t id, int16_t x, int16_t y, bool is_down);

#endif
