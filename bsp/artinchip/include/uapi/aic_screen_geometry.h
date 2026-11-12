/* Product geometry. Pixel coordinates are zero based; dimensions are counts. */
#ifndef AIC_SCREEN_GEOMETRY_H
#define AIC_SCREEN_GEOMETRY_H

#include <stdint.h>

/* Rotate coordinates clockwise and return the rotated dimensions. */
static inline void aic_screen_rotate_cw(int32_t *x, int32_t *y,
                                        int32_t *width, int32_t *height,
                                        unsigned int angle)
{
    int32_t old_x = *x, old_y = *y, w = *width, h = *height;
    switch (angle) {
    case 90:
        *x = h - 1 - old_y;
        *y = old_x;
        *width = h;
        *height = w;
        break;
    case 180:
        *x = w - 1 - old_x;
        *y = h - 1 - old_y;
        break;
    case 270:
        *x = old_y;
        *y = w - 1 - old_x;
        *width = h;
        *height = w;
        break;
    default:
        break;
    }
}

static inline int32_t aic_screen_clamp(int32_t value, int32_t count)
{
    return value < 0 ? 0 : (value >= count ? count - 1 : value);
}

#endif
