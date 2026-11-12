#ifndef AIC_SCREEN_ORIENTATION_H
#define AIC_SCREEN_ORIENTATION_H

#include <rtconfig.h>
#include "aic_screen_geometry.h"

#ifdef AIC_SCREEN_UNIFIED_ORIENTATION
#ifndef AIC_SCREEN_PROFILE_PRESENT
#error "Unified orientation requires a registered screen profile"
#endif
#if AIC_SCREEN_ORIENTATION_DEGREE != 0 && AIC_SCREEN_ORIENTATION_DEGREE != 90 && \
    AIC_SCREEN_ORIENTATION_DEGREE != 180 && AIC_SCREEN_ORIENTATION_DEGREE != 270
#error "Invalid product orientation"
#endif
#ifdef LV_DISPLAY_ROTATE_EN
#error "Unified orientation uses framebuffer rotation only"
#endif
#if defined(AIC_USING_CTP) && !defined(AIC_TOUCH_PANEL_AXS15260) && \
    !defined(AIC_TOUCH_PANEL_C145HAX01) && !defined(AIC_TOUCH_PANEL_C123HAX022) && \
    !defined(AIC_TOUCH_PANEL_CST3240) && !defined(AIC_TOUCH_PANEL_GSL3676) && \
    !defined(AIC_TOUCH_PANEL_GT911)
#error "Integrate the selected touch driver with aic_screen_touch_to_logical first"
#endif
#ifdef AIC_USING_RTP
#error "Unified orientation requires an integrated capacitive touch driver"
#endif

#define AIC_SCREEN_SN_PREFIX "VMAX" AIC_SCREEN_ORIENTATION_LETTER \
    AIC_SCREEN_MODEL AIC_SCREEN_LOGICAL_WIDTH_TEXT "*" \
    AIC_SCREEN_LOGICAL_HEIGHT_TEXT "S"

/* Drivers decode protocol-specific axis ordering before calling this helper.
 * Calibration maps that decoded coordinate space to the native panel.
 * The inverse of the display rotation maps physical touches to logical pixels.
 */
static inline void aic_screen_touch_to_logical(int16_t *x, int16_t *y)
{
    int32_t w = AIC_SCREEN_TOUCH_WIDTH, h = AIC_SCREEN_TOUCH_HEIGHT;
    int32_t px = aic_screen_clamp(*x, w);
    int32_t py = aic_screen_clamp(*y, h);
#ifdef AIC_SCREEN_TOUCH_FLIP_X
    px = w - 1 - px;
#endif
#ifdef AIC_SCREEN_TOUCH_FLIP_Y
    py = h - 1 - py;
#endif
    aic_screen_rotate_cw(&px, &py, &w, &h, AIC_SCREEN_TOUCH_TO_NATIVE_CW);
    px = px * (AIC_SCREEN_BASE_WIDTH - 1) / (w - 1);
    py = py * (AIC_SCREEN_BASE_HEIGHT - 1) / (h - 1);
    w = AIC_SCREEN_BASE_WIDTH;
    h = AIC_SCREEN_BASE_HEIGHT;
    aic_screen_rotate_cw(&px, &py, &w, &h,
                        (360 - AIC_FB_ROTATE_DEGREE) % 360);
    *x = (int16_t)px;
    *y = (int16_t)py;
}
#endif
#endif
