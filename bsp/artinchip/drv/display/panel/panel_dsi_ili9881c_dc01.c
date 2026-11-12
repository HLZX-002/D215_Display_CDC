/*
 * Copyright (c) 2024, Artinchip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "panel_com.h"
#include "panel_dsi.h"

#ifndef PANEL_RESET
#define PANEL_RESET 1
#if PANEL_RESET
#define AIC_PANEL_RESET_GPIO "PD.14"
#endif
#endif

#if PANEL_RESET
static struct gpio_desc reset;
#endif

static int panel_enable(struct aic_panel *panel)
{
    int ret;

#if PANEL_RESET
    panel_get_gpio(&reset, AIC_PANEL_RESET_GPIO);

    panel_gpio_set_value(&reset, 1);
    aic_delay_ms(120);
    panel_gpio_set_value(&reset, 0);
    aic_delay_ms(120);
    panel_gpio_set_value(&reset, 1);
    aic_delay_ms(120);
#endif

    panel_di_enable(panel, 0);
    panel_dsi_send_perpare(panel);

    panel_dsi_dcs_send_seq(panel, 0xFF, 0x98, 0x81, 0x03);
    /* GIP_1 */
    panel_dsi_dcs_send_seq(panel, 0x01, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x02, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x03, 0x53);
    panel_dsi_dcs_send_seq(panel, 0x04, 0x53);
    panel_dsi_dcs_send_seq(panel, 0x05, 0x13);
    panel_dsi_dcs_send_seq(panel, 0x06, 0x04);
    panel_dsi_dcs_send_seq(panel, 0x07, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x08, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x09, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x0A, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x0B, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x0C, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x0D, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x0E, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x0F, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x10, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x11, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x12, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x13, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x14, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x15, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x16, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x17, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x18, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x19, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x1A, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x1B, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x1C, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x1D, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x1E, 0xC0);
    panel_dsi_dcs_send_seq(panel, 0x1F, 0x80);

    panel_dsi_dcs_send_seq(panel, 0x20, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x21, 0x09);
    panel_dsi_dcs_send_seq(panel, 0x22, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x23, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x24, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x25, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x26, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x27, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x28, 0x55);
    panel_dsi_dcs_send_seq(panel, 0x29, 0x03);
    panel_dsi_dcs_send_seq(panel, 0x2A, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x2B, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x2C, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x2D, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x2E, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x2F, 0x00);

    panel_dsi_dcs_send_seq(panel, 0x30, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x31, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x32, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x33, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x34, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x35, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x36, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x37, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x38, 0x3C);
    panel_dsi_dcs_send_seq(panel, 0x39, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x3A, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x3B, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x3C, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x3D, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x3E, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x3F, 0x00);

    panel_dsi_dcs_send_seq(panel, 0x40, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x41, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x42, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x43, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x44, 0x00);

    /* GIP 2 */
    panel_dsi_dcs_send_seq(panel, 0x50, 0x01);
    panel_dsi_dcs_send_seq(panel, 0x51, 0x23);
    panel_dsi_dcs_send_seq(panel, 0x52, 0x45);
    panel_dsi_dcs_send_seq(panel, 0x53, 0x67);
    panel_dsi_dcs_send_seq(panel, 0x54, 0x89);
    panel_dsi_dcs_send_seq(panel, 0x55, 0xAB);
    panel_dsi_dcs_send_seq(panel, 0x56, 0x01);
    panel_dsi_dcs_send_seq(panel, 0x57, 0x23);
    panel_dsi_dcs_send_seq(panel, 0x58, 0x45);
    panel_dsi_dcs_send_seq(panel, 0x59, 0x67);
    panel_dsi_dcs_send_seq(panel, 0x5A, 0x89);
    panel_dsi_dcs_send_seq(panel, 0x5B, 0xAB);
    panel_dsi_dcs_send_seq(panel, 0x5C, 0xCD);
    panel_dsi_dcs_send_seq(panel, 0x5D, 0xEF);

    /* GIP 3 */
    panel_dsi_dcs_send_seq(panel, 0x5E, 0x01);
    panel_dsi_dcs_send_seq(panel, 0x5F, 0x08);
    panel_dsi_dcs_send_seq(panel, 0x60, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x61, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x62, 0x0A);
    panel_dsi_dcs_send_seq(panel, 0x63, 0x15);
    panel_dsi_dcs_send_seq(panel, 0x64, 0x14);
    panel_dsi_dcs_send_seq(panel, 0x65, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x66, 0x11);
    panel_dsi_dcs_send_seq(panel, 0x67, 0x10);
    panel_dsi_dcs_send_seq(panel, 0x68, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x69, 0x0F);
    panel_dsi_dcs_send_seq(panel, 0x6A, 0x0E);
    panel_dsi_dcs_send_seq(panel, 0x6B, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x6C, 0x0D);
    panel_dsi_dcs_send_seq(panel, 0x6D, 0x0C);
    panel_dsi_dcs_send_seq(panel, 0x6E, 0x06);
    panel_dsi_dcs_send_seq(panel, 0x6F, 0x02);

    panel_dsi_dcs_send_seq(panel, 0x70, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x71, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x72, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x73, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x74, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x75, 0x06);
    panel_dsi_dcs_send_seq(panel, 0x76, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x77, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x78, 0x0A);
    panel_dsi_dcs_send_seq(panel, 0x79, 0x15);
    panel_dsi_dcs_send_seq(panel, 0x7A, 0x14);
    panel_dsi_dcs_send_seq(panel, 0x7B, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x7C, 0x10);
    panel_dsi_dcs_send_seq(panel, 0x7D, 0x11);
    panel_dsi_dcs_send_seq(panel, 0x7E, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x7F, 0x0C);

    panel_dsi_dcs_send_seq(panel, 0x80, 0x0D);
    panel_dsi_dcs_send_seq(panel, 0x81, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x82, 0x0E);
    panel_dsi_dcs_send_seq(panel, 0x83, 0x0F);
    panel_dsi_dcs_send_seq(panel, 0x84, 0x08);
    panel_dsi_dcs_send_seq(panel, 0x85, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x86, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x87, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x88, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x89, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x8A, 0x02);

    /* CMD_Page 4 */
    panel_dsi_dcs_send_seq(panel, 0xFF, 0x98, 0x81, 0x04);
    panel_dsi_dcs_send_seq(panel, 0x6C, 0x15);
    panel_dsi_dcs_send_seq(panel, 0x6E, 0x30);
    panel_dsi_dcs_send_seq(panel, 0x6F, 0x33);

    panel_dsi_dcs_send_seq(panel, 0x8D, 0x1F);
    panel_dsi_dcs_send_seq(panel, 0x87, 0xBA);

    panel_dsi_dcs_send_seq(panel, 0x26, 0x76);
    panel_dsi_dcs_send_seq(panel, 0xB2, 0xD1);
    panel_dsi_dcs_send_seq(panel, 0x35, 0x1F);
    panel_dsi_dcs_send_seq(panel, 0x33, 0x14);
    panel_dsi_dcs_send_seq(panel, 0x3A, 0xA9);
    panel_dsi_dcs_send_seq(panel, 0x3B, 0x98);
    panel_dsi_dcs_send_seq(panel, 0x38, 0x01);
    panel_dsi_dcs_send_seq(panel, 0x39, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x3B, 0xC0);

    /* CMD_Page 1 */
    panel_dsi_dcs_send_seq(panel, 0xFF, 0x98, 0x81, 0x01);
    panel_dsi_dcs_send_seq(panel, 0x22, 0x0A);
    panel_dsi_dcs_send_seq(panel, 0x31, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x40, 0x53);
    panel_dsi_dcs_send_seq(panel, 0x50, 0xE9);
    panel_dsi_dcs_send_seq(panel, 0x51, 0xE4);

    panel_dsi_dcs_send_seq(panel, 0x53, 0x3D);
    panel_dsi_dcs_send_seq(panel, 0x55, 0x48);

    panel_dsi_dcs_send_seq(panel, 0x60, 0x28);
    panel_dsi_dcs_send_seq(panel, 0x2E, 0xC8);
    panel_dsi_dcs_send_seq(panel, 0x34, 0x01);

    panel_dsi_dcs_send_seq(panel, 0xA0, 0x00);
    panel_dsi_dcs_send_seq(panel, 0xA1, 0x15);
    panel_dsi_dcs_send_seq(panel, 0xA2, 0x24);
    panel_dsi_dcs_send_seq(panel, 0xA3, 0x15);
    panel_dsi_dcs_send_seq(panel, 0xA4, 0x19);
    panel_dsi_dcs_send_seq(panel, 0xA5, 0x2E);
    panel_dsi_dcs_send_seq(panel, 0xA6, 0x23);
    panel_dsi_dcs_send_seq(panel, 0xA7, 0x24);
    panel_dsi_dcs_send_seq(panel, 0xA8, 0x86);
    panel_dsi_dcs_send_seq(panel, 0xA9, 0x1B);
    panel_dsi_dcs_send_seq(panel, 0xAA, 0x26);
    panel_dsi_dcs_send_seq(panel, 0xAB, 0x76);
    panel_dsi_dcs_send_seq(panel, 0xAC, 0x1C);
    panel_dsi_dcs_send_seq(panel, 0xAD, 0x1B);
    panel_dsi_dcs_send_seq(panel, 0xAE, 0x4F);
    panel_dsi_dcs_send_seq(panel, 0xAF, 0x24);
    panel_dsi_dcs_send_seq(panel, 0xB0, 0x29);
    panel_dsi_dcs_send_seq(panel, 0xB1, 0x4D);
    panel_dsi_dcs_send_seq(panel, 0xB2, 0x5A);
    panel_dsi_dcs_send_seq(panel, 0xB3, 0x27);
    panel_dsi_dcs_send_seq(panel, 0xC0, 0x00);
    panel_dsi_dcs_send_seq(panel, 0xC1, 0x15);
    panel_dsi_dcs_send_seq(panel, 0xC2, 0x24);
    panel_dsi_dcs_send_seq(panel, 0xC3, 0x15);
    panel_dsi_dcs_send_seq(panel, 0xC4, 0x19);
    panel_dsi_dcs_send_seq(panel, 0xC5, 0x2E);
    panel_dsi_dcs_send_seq(panel, 0xC6, 0x23);
    panel_dsi_dcs_send_seq(panel, 0xC7, 0x24);
    panel_dsi_dcs_send_seq(panel, 0xC8, 0x86);
    panel_dsi_dcs_send_seq(panel, 0xC9, 0x1B);
    panel_dsi_dcs_send_seq(panel, 0xCA, 0x26);
    panel_dsi_dcs_send_seq(panel, 0xCB, 0x76);
    panel_dsi_dcs_send_seq(panel, 0xCC, 0x1C);
    panel_dsi_dcs_send_seq(panel, 0xCD, 0x1B);
    panel_dsi_dcs_send_seq(panel, 0xCE, 0x4F);
    panel_dsi_dcs_send_seq(panel, 0xCF, 0x24);
    panel_dsi_dcs_send_seq(panel, 0xD0, 0x29);
    panel_dsi_dcs_send_seq(panel, 0xD1, 0x4D);
    panel_dsi_dcs_send_seq(panel, 0xD2, 0x5A);
    panel_dsi_dcs_send_seq(panel, 0xD3, 0x27);

    /* CMD_Page 2 */
    panel_dsi_dcs_send_seq(panel, 0xFF, 0x98, 0x81, 0x02);
    panel_dsi_dcs_send_seq(panel, 0x04, 0x17);
    panel_dsi_dcs_send_seq(panel, 0x05, 0x12);
    panel_dsi_dcs_send_seq(panel, 0x06, 0x40);
    panel_dsi_dcs_send_seq(panel, 0x07, 0x0B);

    /* CMD_Page 0 */
    panel_dsi_dcs_send_seq(panel, 0xFF, 0x98, 0x81, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x51, 0x0F, 0xF0);
    panel_dsi_dcs_send_seq(panel, 0x53, 0x2C);
    panel_dsi_dcs_send_seq(panel, 0x55, 0x00);

    /* CMD_Page 0 */
    panel_dsi_dcs_send_seq(panel, 0xFF, 0x98, 0x81, 0x00);
    panel_dsi_dcs_send_seq(panel, 0x35, 0x00);

    ret = panel_dsi_dcs_exit_sleep_mode(panel);
    if (ret < 0) {
        pr_err("Failed to exit sleep mode: %d\n", ret);
        return ret;
    }
    aic_delay_ms(120);

    ret = panel_dsi_dcs_set_display_on(panel);
    if (ret < 0) {
        pr_err("Failed to set display on: %d\n", ret);
        return ret;
    }
    aic_delay_ms(5);

    panel_dsi_setup_realmode(panel);

    panel_de_timing_enable(panel, 0);
    panel_backlight_enable(panel, 0);
    return 0;
}

static int panel_disable(struct aic_panel *panel)
{
    panel_default_disable(panel);
#if PANEL_RESET
    panel_gpio_set_value(&reset, 0);
#endif
    return 0;
}

static struct aic_panel_funcs panel_funcs = {
    .disable = panel_disable,
    .unprepare = panel_default_unprepare,
    .prepare = panel_default_prepare,
    .enable = panel_enable,
    .register_callback = panel_register_callback,
};

static struct display_timing ili9881c_timing = {
    .pixelclock = 84000000,
    .hactive = 800,
    .hfront_porch = 120,
    .hback_porch = 120,
    .hsync_len = 28,
    .vactive = 1280,
    .vfront_porch = 24,
    .vback_porch = 20,
    .vsync_len = 16,
};

struct panel_dsi dsi = {
    .mode = DSI_MOD_VID_BURST,
    .format = DSI_FMT_RGB888,
    .lane_num = 4,
    .ln_assign = 0x2103,
};

struct aic_panel dsi_ili9881c_dc01 = {
    .name = "panel-ili9881c",
    .timings = &ili9881c_timing,
    .funcs = &panel_funcs,
    .dsi = &dsi,
    .connector_type = AIC_MIPI_COM,
};
