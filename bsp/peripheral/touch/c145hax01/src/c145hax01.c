/*
 * Copyright (c) 2024, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date              Notes
 * 2024-09-04        the first version
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include "touch_common.h"
#include "c145hax01.h"
#include "aic_log.h"

#define DBG_TAG "c145hax01"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static struct rt_i2c_client c145hax01_client;

static rt_size_t c145hax01_read_regs(struct rt_i2c_client *dev, rt_uint8_t *reg,
                                    rt_uint8_t *data, rt_uint8_t len)
{
    struct rt_i2c_msg msgs[2];
    rt_size_t ret = 0;
    rt_uint8_t retry;

    msgs[0].addr = dev->client_addr;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf = reg;
    msgs[0].len = C145HAX01_REGITER_LEN;

    msgs[1].addr = dev->client_addr;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf = data;
    msgs[1].len = len;

    /* Retry the complete register write + data read transaction. */
    for (retry = 0; retry < C145HAX01_I2C_RETRY_NUMBER; retry++) {
        ret = rt_i2c_transfer(dev->bus, msgs, 2);
        if (ret == 2)
            break;
        if ((retry + 1) < C145HAX01_I2C_RETRY_NUMBER)
            rt_thread_mdelay(retry + 1);
    }

    return ret;
}

static rt_size_t c145hax01_read_report(rt_uint8_t *data,
                                       rt_uint8_t *failed_reg,
                                       rt_uint8_t *failed_len)
{
    rt_uint8_t reg = C145HAX01_DEVICE_MODE;
    rt_size_t ret;

    ret = c145hax01_read_regs(&c145hax01_client, &reg, data,
                              C145HAX01_REPORT_LEN);
    if (ret != 2) {
        *failed_reg = reg;
        *failed_len = C145HAX01_REPORT_LEN;
    }

    return ret;
}

static int16_t pre_x[C145HAX01_MAX_TOUCH] = { -1, -1, -1, -1, -1 };
static int16_t pre_y[C145HAX01_MAX_TOUCH] = { -1, -1, -1, -1, -1 };
static rt_uint8_t s_tp_dowm[C145HAX01_MAX_TOUCH] = {0};
static struct rt_touch_data *g_read_data = RT_NULL;

static void c145hax01_touch_up(void *buf, int8_t id)
{
    g_read_data = (struct rt_touch_data *)buf;

    if (s_tp_dowm[id] == 1) {
        s_tp_dowm[id] = 0;
        g_read_data[id].event = RT_TOUCH_EVENT_UP;
    } else {
        g_read_data[id].event = RT_TOUCH_EVENT_NONE;
    }

    g_read_data[id].timestamp = rt_touch_get_ts();
    g_read_data[id].x_coordinate = pre_x[id];
    g_read_data[id].y_coordinate = pre_y[id];
    g_read_data[id].track_id = id;

    /* 保存最后坐标再清除 */
    int16_t last_x = pre_x[id];
    int16_t last_y = pre_y[id];

    pre_x[id] = -1; /* last point is none */
    pre_y[id] = -1;
    /* 非投屏模式且启用 CDC 触摸数据时发送 UP 事件 */
    if (!s_in_display_mode && touch_data_enabled) {
        const int16_t max_X = C145HAX01_TOUCH_X_RANGE;
        const int16_t max_Y = C145HAX01_TOUCH_Y_RANGE;
        if (last_x >= 0 && last_y >= 0) {
            int16_t scaled_x = (int16_t)(((int32_t)last_x * 4096) / (int32_t)max_X);
            int16_t scaled_y = (int16_t)(((int32_t)last_y * 4096) / (int32_t)max_Y);
            LOG_I("cst3556_touch_up -> send UP scaled (%d,%d)", scaled_x, scaled_y);
            send_touch_data(id, scaled_x, scaled_y, false);
        } else {
            LOG_D("cst3556_touch_up -> send UP (0,0) because last coords invalid");
            send_touch_data(id, 0, 0, false);
        }
    } 
}

static void c145hax01_touch_down(void *buf, int8_t id, int16_t x, int16_t y)
{
    g_read_data = (struct rt_touch_data *)buf;

    if (s_tp_dowm[id] == 1) {
        g_read_data[id].event = RT_TOUCH_EVENT_MOVE;
    } else {
        g_read_data[id].event = RT_TOUCH_EVENT_DOWN;
        s_tp_dowm[id] = 1;
    }

    g_read_data[id].timestamp = rt_touch_get_ts();
    g_read_data[id].x_coordinate = x;
    g_read_data[id].y_coordinate = y;
    g_read_data[id].track_id = id;
    /* Save raw panel coordinates for a correctly scaled UP event. */
    if (id >= 0 && id < C145HAX01_MAX_TOUCH) {
        pre_x[id] = x;
        pre_y[id] = y;
    }
    /* 发送 DOWN（scaled）到 CDC */
    if (!s_in_display_mode && touch_data_enabled) {
        const int16_t max_X = C145HAX01_TOUCH_X_RANGE;
        const int16_t max_Y = C145HAX01_TOUCH_Y_RANGE;
        int16_t scaled_x = (int16_t)(((int32_t)x * 4096) / (int32_t)max_X);
        int16_t scaled_y = (int16_t)(((int32_t)y * 4096) / (int32_t)max_Y);
        LOG_I("cst3556_touch_down -> send DOWN scaled (%d,%d)", scaled_x, scaled_y);
        send_touch_data(id, scaled_x, scaled_y, true);
    }
}

static rt_size_t c145hax01_read_point(struct rt_touch_device *touch, void *buf,
                                  rt_size_t read_num)
{
    rt_uint8_t touch_num = 0;
    rt_uint8_t read_buf[C145HAX01_REPORT_LEN] = { 0 };
    rt_uint8_t read_index;
    rt_size_t xfer_count;
    rt_uint8_t failed_reg = C145HAX01_DEVICE_MODE;
    rt_uint8_t failed_len = C145HAX01_REPORT_LEN;
    int8_t read_id = 0;
    int16_t input_x = 0;
    int16_t input_y = 0;
    rt_uint8_t event_flag = 0xff;
    rt_uint8_t down_count = 0;
    rt_uint8_t active_mask = 0;
    rt_uint8_t previous_mask = 0;
    rt_uint8_t log_id = 0xff;
    rt_uint16_t raw_x = 0;
    rt_uint16_t raw_y = 0;

    rt_memset(buf, 0, sizeof(struct rt_touch_data) * read_num);

    for (read_index = 0; read_index < C145HAX01_MAX_TOUCH; read_index++) {
        if (s_tp_dowm[read_index])
            previous_mask |= (1U << read_index);
    }

    rt_memset(read_buf, 0xff, sizeof(read_buf));
    xfer_count = c145hax01_read_report(read_buf, &failed_reg, &failed_len);

    if (xfer_count != 2) {
        static rt_tick_t last_log_tick;
        static rt_uint32_t fail_count;
        rt_tick_t now = rt_tick_get();

        fail_count++;
        if ((fail_count == 1) ||
            ((rt_tick_t)(now - last_log_tick) >= RT_TICK_PER_SECOND)) {
            LOG_E("I2C report read failed: bus=%s addr=0x%02x reg=0x%02x "
                  "len=%u xfer=%u/2 failures=%u",
                  touch->config.dev_name, c145hax01_client.client_addr,
                  failed_reg, (unsigned int)failed_len,
                  (unsigned int)xfer_count,
                  (unsigned int)fail_count);
            last_log_tick = now;
        }
        read_num = 0;
        goto __exit;
    }

    touch_num = read_buf[C145HAX01_TD_STATUS] & 0x0f;
    if (touch_num > C145HAX01_MAX_TOUCH) {
        static rt_tick_t last_invalid_log_tick;
        rt_tick_t now = rt_tick_get();

        if ((rt_tick_t)(now - last_invalid_log_tick) >= RT_TICK_PER_SECOND) {
            LOG_W("invalid touch count: %u", (unsigned int)touch_num);
            last_invalid_log_tick = now;
        }
        read_num = 0;
        goto __exit;
    }

    for (read_index = 0; read_index < C145HAX01_MAX_TOUCH; read_index++) {
        rt_uint8_t off_set = C145HAX01_POINT1_XH +
                             read_index * C145HAX01_POINT_INFO_LEN;

        read_id = (read_buf[off_set + 2] >> 4) & 0x0f;
        /* Unused FIFO records use ID 0xF. */
        if (read_id >= 0x0a)
            break;
        if (read_id >= C145HAX01_MAX_TOUCH) {
            LOG_W("invalid touch id: %u", (unsigned int)read_id);
            continue;
        }

        event_flag = (read_buf[off_set] >> 6) & 0x03;
        raw_x = ((read_buf[off_set] & 0x0f) << 8) |
                read_buf[off_set + 1];
        raw_y = ((read_buf[off_set + 2] & 0x0f) << 8) |
                read_buf[off_set + 3];
        log_id = read_id;

        if (event_flag == 1) {
            /* Explicit firmware UP event. */
            c145hax01_touch_up(buf, read_id);
            continue;
        }

        /* Only event 0 (DOWN) and 2 (CONTACT) are active contacts. */
        if ((event_flag != 0) && (event_flag != 2))
            continue;

        /* Ignore stale point slots beyond the active count. */
        if (down_count >= touch_num)
            continue;
        down_count++;

        /* The 2560x720 module reports raw X on the 720 axis. */
        input_y = raw_x;
        input_x = raw_y;

        if (!aic_touch_transform(&input_x, &input_y))
            continue;

        active_mask |= (1U << read_id);
        c145hax01_touch_down(buf, read_id, input_x, input_y);
    }

    /* Release contacts absent from this frame, even if firmware UP was lost. */
    for (read_index = 0; read_index < C145HAX01_MAX_TOUCH; read_index++) {
        if (s_tp_dowm[read_index] && !(active_mask & (1U << read_index)))
            c145hax01_touch_up(buf, read_index);
    }

    if (previous_mask != active_mask) {
        LOG_I("TP state: status=%u event=%u id=%u raw=(%u,%u) active=0x%02x",
              (unsigned int)touch_num, (unsigned int)event_flag,
              (unsigned int)log_id,
              (unsigned int)raw_x, (unsigned int)raw_y,
              (unsigned int)active_mask);
    }

    if ((previous_mask == 0) && (active_mask == 0))
        read_num = 0;

__exit:
    return read_num;
}

static rt_err_t c145hax01_control(struct rt_touch_device *touch, int cmd, void *data)
{
    struct rt_touch_info *info = RT_NULL;

    switch(cmd)
    {
    case RT_TOUCH_CTRL_GET_ID:
        break;
    case RT_TOUCH_CTRL_GET_INFO:
        info = (struct rt_touch_info *)data;
        if (info == RT_NULL)
            return -RT_EINVAL;

        info->point_num = touch->info.point_num;
        info->range_x = touch->info.range_x;
        info->range_y = touch->info.range_y;
        info->type = touch->info.type;
        info->vendor = touch->info.vendor;
        break;
    case RT_TOUCH_CTRL_SET_MODE:
    case RT_TOUCH_CTRL_SET_X_RANGE:
    case RT_TOUCH_CTRL_SET_Y_RANGE:
    case RT_TOUCH_CTRL_SET_X_TO_Y:
    case RT_TOUCH_CTRL_DISABLE_INT:
    case RT_TOUCH_CTRL_ENABLE_INT:
    default:
        break;
    }

    return RT_EOK;
}

const struct rt_touch_ops c145hax01_touch_ops =
{
    .touch_readpoint = c145hax01_read_point,
    .touch_control = c145hax01_control,
};

struct rt_touch_info c145hax01_info =
{
    RT_TOUCH_TYPE_CAPACITANCE,
    RT_TOUCH_VENDOR_UNKNOWN,
    5,
    (rt_int32_t)C145HAX01_TOUCH_X_RANGE,
    (rt_int32_t)C145HAX01_TOUCH_Y_RANGE,
};

int c145hax01_hw_init(const char *name, struct rt_touch_config *cfg)
{
    rt_touch_t touch_device = RT_NULL;

    touch_device = (rt_touch_t)rt_calloc(1, sizeof(struct rt_touch_device));
    if (touch_device == RT_NULL) {
        LOG_E("touch device malloc fail");
        return -RT_ENOMEM;
    }

    c145hax01_client.bus = (struct rt_i2c_bus_device *)rt_device_find(cfg->dev_name);
    if (c145hax01_client.bus == RT_NULL) {
        LOG_E("Can't find %s device", cfg->dev_name);
        return -RT_ERROR;
    }

    if (rt_device_open((rt_device_t)c145hax01_client.bus, RT_DEVICE_FLAG_RDWR) != RT_EOK) {
        LOG_E("open %s device failed", cfg->dev_name);
        return -RT_ERROR;
    }

    c145hax01_client.client_addr = C145HAX01_SALVE_ADDR;

    /* register touch device */

    touch_device->info = c145hax01_info;
    rt_memcpy(&touch_device->config, cfg, sizeof(struct rt_touch_config));
    touch_device->ops = &c145hax01_touch_ops;

    if (RT_EOK != rt_hw_touch_register(touch_device, name, RT_DEVICE_FLAG_INT_RX, RT_NULL)) {
        LOG_E("touch device c145hax01 init failed !!!");
        return -RT_ERROR;
    }

    LOG_I("touch device c145hax01 init success");
    return RT_EOK;

}

static int c145hax01_gpio_cfg()
{
    unsigned int g, p;
    long pin;

    // RST
    pin = drv_pin_get(AIC_TOUCH_PANEL_RST_PIN);
    g = GPIO_GROUP(pin);
    p = GPIO_GROUP_PIN(pin);
    hal_gpio_direction_input(g, p);

    // INT
    pin = drv_pin_get(AIC_TOUCH_PANEL_INT_PIN);
    g = GPIO_GROUP(pin);
    p = GPIO_GROUP_PIN(pin);
    hal_gpio_direction_input(g, p);
    hal_gpio_set_irq_mode(g, p, 0);

    return 0;
}

static int rt_hw_c145hax01_port(void)
{
    int ret;
    struct rt_touch_config cfg;
    rt_uint8_t rst_pin;

    c145hax01_gpio_cfg();

    rst_pin = drv_pin_get(AIC_TOUCH_PANEL_RST_PIN);
    cfg.dev_name = AIC_TOUCH_PANEL_I2C_CHAN;
    cfg.irq_pin.pin = drv_pin_get(AIC_TOUCH_PANEL_INT_PIN);
    /* C145HAX01 INT is active-low. */
    cfg.irq_pin.mode = PIN_MODE_INPUT_PULLUP;
    cfg.user_data = &rst_pin;

    ret = c145hax01_hw_init(C145HAX01_DEVICE_NAME, &cfg);
    if (ret != RT_EOK) {
        LOG_E("touch port init failed: %d", ret);
        return ret;
    }

    return 0;
}

INIT_DEVICE_EXPORT(rt_hw_c145hax01_port);
