/*
 * Copyright (c) 2024-2025, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date              Notes
 * 2024-10-18        the first version
 */

#include "touch_common.h"
#include <aic_screen_orientation.h>

/* Keep the C59/C54 panel geometry correct if .config still contains
 * touch values from a previously selected screen. */
#if defined(AIC_SCREEN_VMAXC59_14_5) || defined(AIC_SCREEN_VMAXC54_12_6)
#define AIC_TOUCH_EFFECTIVE_X_RANGE       2560
#define AIC_TOUCH_EFFECTIVE_Y_RANGE       720
#define AIC_SCREEN_EFFECTIVE_X_RESOLUTION 2560
#define AIC_SCREEN_EFFECTIVE_Y_RESOLUTION 720
#else
#define AIC_TOUCH_EFFECTIVE_X_RANGE       AIC_TOUCH_X_COORDINATE_RANGE
#define AIC_TOUCH_EFFECTIVE_Y_RANGE       AIC_TOUCH_Y_COORDINATE_RANGE
#define AIC_SCREEN_EFFECTIVE_X_RESOLUTION AIC_SCREEN_REAL_X_RESOLUTION
#define AIC_SCREEN_EFFECTIVE_Y_RESOLUTION AIC_SCREEN_REAL_Y_RESOLUTION
#endif

void aic_touch_flip(int16_t *x_coordinate, int16_t *y_coordinate)
{
#ifdef AIC_TOUCH_X_FLIP
    *x_coordinate = (rt_int16_t)AIC_TOUCH_EFFECTIVE_X_RANGE - *x_coordinate;
#endif
#ifdef AIC_TOUCH_Y_FLIP
    *y_coordinate = (rt_int16_t)AIC_TOUCH_EFFECTIVE_Y_RANGE - *y_coordinate;
#endif
}

void aic_touch_rotate(int16_t *x_coordinate, int16_t *y_coordinate)
{
    rt_uint16_t temp = 0;
    temp = temp;
#ifdef AIC_TOUCH_90_DEGREE_ROTATION
    temp = *x_coordinate;
    *x_coordinate = *y_coordinate;
    *y_coordinate = (rt_int16_t)AIC_TOUCH_EFFECTIVE_X_RANGE - temp;
#endif
#ifdef AIC_TOUCH_270_DEGREE_ROTATION
    temp = *x_coordinate;
    *x_coordinate = (rt_int16_t)AIC_TOUCH_EFFECTIVE_Y_RANGE - *y_coordinate;
    *y_coordinate = temp;
#endif
}

void aic_touch_scale(int16_t *x_coordinate, int16_t *y_coordinate)
{
#ifdef AIC_TOUCH_CROP
    rt_int32_t temp_x;
    rt_int32_t temp_y;

    temp_x = (*x_coordinate) * AIC_SCREEN_EFFECTIVE_X_RESOLUTION * 10 /
             AIC_TOUCH_EFFECTIVE_X_RANGE / 10;
    temp_y = (*y_coordinate) * AIC_SCREEN_EFFECTIVE_Y_RESOLUTION * 10 /
             AIC_TOUCH_EFFECTIVE_Y_RANGE / 10;
    *x_coordinate = (int16_t)temp_x;
    *y_coordinate = (int16_t)temp_y;
#endif
}


rt_int8_t aic_touch_crop(int16_t *x_coordinate, int16_t *y_coordinate)
{
#ifdef AIC_TOUCH_CROP
    if (*x_coordinate < AIC_TOUCH_CROP_POS_X || *y_coordinate < AIC_TOUCH_CROP_POS_Y
        || *x_coordinate > (AIC_TOUCH_CROP_POS_X + AIC_TOUCH_CROP_WIDTH)
        || *y_coordinate > (AIC_TOUCH_CROP_POS_Y + AIC_TOUCH_CROP_HEIGHT)) {
        return RT_FALSE;
    }

    *x_coordinate -= AIC_TOUCH_CROP_POS_X;
    *y_coordinate -= AIC_TOUCH_CROP_POS_Y;
#endif
    return RT_TRUE;
}

void aic_touch_dynamic_rotate(struct rt_touch_device *touch, int16_t *x_coordinate, int16_t *y_coordinate)
{
    uint16_t angle = 0;
    rt_uint16_t temp = 0;

    rt_device_control((rt_device_t)touch, RT_TOUCH_CTRL_GET_DYNAMIC_ROTATE, &angle);

    if (angle == 90) {
        temp = *x_coordinate;
        *x_coordinate = *y_coordinate;
        *y_coordinate = (rt_int16_t)AIC_TOUCH_EFFECTIVE_X_RANGE - temp;
    } else if (angle == 180) {
        *x_coordinate = (rt_int16_t)AIC_TOUCH_EFFECTIVE_X_RANGE - *x_coordinate;
        *y_coordinate = (rt_int16_t)AIC_TOUCH_EFFECTIVE_Y_RANGE - *y_coordinate;
    } else if (angle == 270) {
        temp = *x_coordinate;
        *x_coordinate = (rt_int16_t)AIC_TOUCH_EFFECTIVE_Y_RANGE - *y_coordinate;
        *y_coordinate = temp;
    } else {
        return;
    }
}

rt_int8_t aic_touch_dynamic_crop(struct rt_touch_device *touch, int16_t *input_x, int16_t *input_y)
{
    struct rt_touch_crop_info crop_info = {0};
    int16_t div_x, div_y;

    rt_device_control((rt_device_t)touch, RT_TOUCH_CTRL_GET_DYNAMIC_CROP, &crop_info);

    if (crop_info.enable == 0)
        return RT_EOK;

    if (crop_info.width > AIC_SCREEN_EFFECTIVE_X_RESOLUTION ||
            crop_info.height > AIC_SCREEN_EFFECTIVE_Y_RESOLUTION) {
        rt_kprintf("Crop width or height out of screen bound!\n");
        return RT_EINVAL;
    }

    div_x = (AIC_SCREEN_EFFECTIVE_X_RESOLUTION - (int16_t)crop_info.width) / 2;
    div_y = (AIC_SCREEN_EFFECTIVE_Y_RESOLUTION - (int16_t)crop_info.height) / 2;

    if (crop_info.width >= crop_info.height) {
        *input_x -= div_y;
        if (*input_x <= 0)
            return RT_EINVAL;

        *input_y -= div_x;
        if (*input_y <= 0)
            return RT_EINVAL;

        *input_x = (*input_x * AIC_SCREEN_EFFECTIVE_Y_RESOLUTION * 100 /
                    (int16_t)crop_info.height / 100);
        if (*input_x >= AIC_SCREEN_EFFECTIVE_Y_RESOLUTION)
            return RT_EINVAL;
    } else {
        *input_x -= div_x;
        if (*input_x <= 0)
            return RT_EINVAL;

        *input_y -= div_y;
        if (*input_y <= 0)
            return RT_EINVAL;

        *input_x = (*input_x * AIC_SCREEN_EFFECTIVE_X_RESOLUTION * 100 /
                    (int16_t)crop_info.width / 100);
        if (*input_x >= AIC_SCREEN_EFFECTIVE_X_RESOLUTION)
            return RT_EINVAL;
    }

    return RT_EOK;
}

/* One entry point after the controller-specific packet/axis decode. */
rt_int8_t aic_touch_transform(int16_t *x, int16_t *y)
{
#ifdef AIC_SCREEN_UNIFIED_ORIENTATION
    aic_screen_touch_to_logical(x, y);
    return RT_TRUE;
#else
    aic_touch_flip(x, y);
    aic_touch_rotate(x, y);
    aic_touch_scale(x, y);
    return aic_touch_crop(x, y);
#endif
}
