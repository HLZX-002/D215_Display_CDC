/*
 * Copyright (C) 2024-2025 ArtInChip Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:  Huahui Mai <huahui.mai@artinchip.com>
 */

#include <rtthread.h>
#include <rtdevice.h>

#include "aic_osal.h"
#include "usb_osd_ui.h"
#include "video/usb_osd_video.h"
#include "aic_lv_ffmpeg.h"
#include "lv_tpc_run.h"

//#define USB_OSD_LOG
#ifdef USB_OSD_LOG
#define USB_OSD_DEBUG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define USB_OSD_DEBUG(fmt, ...)
#endif

static lv_osd_manager_t* instance = NULL;

#ifdef LV_USB_OSD_LOGO_TYPE_IMAGE
enum usb_osd_static_logo_request {
    USB_OSD_STATIC_LOGO_NONE = 0,
    USB_OSD_STATIC_LOGO_SHOW,
    USB_OSD_STATIC_LOGO_HIDE,
    USB_OSD_STATIC_LOGO_RELEASE_CDC,
};

#define USB_OSD_STATIC_LOGO_QUEUE_DEPTH 8
struct usb_osd_static_logo_message {
    enum usb_osd_static_logo_request request;
    unsigned int epoch;
};
static volatile struct usb_osd_static_logo_message
    static_logo_queue[USB_OSD_STATIC_LOGO_QUEUE_DEPTH];
static volatile unsigned int static_logo_queue_head = 0;
static volatile unsigned int static_logo_queue_tail = 0;
static unsigned int static_logo_active_epoch = 0;
/* Private SHOW retries must not outlive a newer SHOW or its matching HIDE. */
static volatile unsigned int static_logo_requested_epoch = 0;
static volatile bool static_logo_show_requested = false;
static volatile bool static_logo_session_visible = false;
static lv_draw_buf_t *static_logo_pixels = NULL;

static bool usb_osd_handle_static_logo_request(lv_osd_manager_t *mgr);

/*
 * AICFB_POWERON implicitly turns PWM back on, so the panel driver helper is
 * needed to keep the backlight dark until the logo has actually been committed.
 */
extern void aic_panel_backlight_disable(void);

/* The arbiter owns both the power-on PNG and the wake/restart PNG. */
RT_WEAK bool usb_osd_static_logo_ui_owned(void)
{
    return false;
}

/* HIDE clears session_visible before requesting alpha=0 below. That explicit
 * hand-off must pass; Display enable/reload and D0 must not uncover the PNG. */
int usb_osd_static_logo_mask_required(void)
{
    bool required;
    rt_base_t level = rt_hw_interrupt_disable();

    required = static_logo_session_visible && usb_osd_static_logo_ui_owned();
    rt_hw_interrupt_enable(level);
    return required;
}

RT_WEAK void usb_osd_static_logo_visible(unsigned int epoch)
{
    (void)epoch;
}

RT_WEAK void usb_osd_static_logo_released(unsigned int epoch)
{
    (void)epoch;
}

static void usb_osd_static_logo_post(
    enum usb_osd_static_logo_request request, unsigned int epoch)
{
    rt_base_t level;
    unsigned int next;

    level = rt_hw_interrupt_disable();
    next = (static_logo_queue_tail + 1) % USB_OSD_STATIC_LOGO_QUEUE_DEPTH;
    if (next == static_logo_queue_head) {
        /* Retain the newest USB state transition.  Epoch validation below
         * prevents an evicted old RELEASE from affecting the new session. */
        static_logo_queue_head =
            (static_logo_queue_head + 1) % USB_OSD_STATIC_LOGO_QUEUE_DEPTH;
    }
    static_logo_queue[static_logo_queue_tail].request = request;
    static_logo_queue[static_logo_queue_tail].epoch = epoch;
    static_logo_queue_tail = next;
    rt_hw_interrupt_enable(level);
}

void usb_osd_static_logo_show(unsigned int epoch)
{
    rt_base_t level = rt_hw_interrupt_disable();
    static_logo_requested_epoch = epoch;
    static_logo_show_requested = true;
    usb_osd_static_logo_post(USB_OSD_STATIC_LOGO_SHOW, epoch);
    rt_hw_interrupt_enable(level);
}

static void usb_osd_static_logo_cancel_show(unsigned int epoch)
{
    rt_base_t level = rt_hw_interrupt_disable();
    if (epoch == static_logo_requested_epoch)
        static_logo_show_requested = false;
    rt_hw_interrupt_enable(level);
}

void usb_osd_static_logo_hide(unsigned int epoch)
{
    usb_osd_static_logo_post(USB_OSD_STATIC_LOGO_HIDE, epoch);
}

void usb_osd_static_logo_release_cdc(unsigned int epoch)
{
    usb_osd_static_logo_post(USB_OSD_STATIC_LOGO_RELEASE_CDC, epoch);
}
#endif /* LV_USB_OSD_LOGO_TYPE_IMAGE */

#ifdef LV_USB_OSD_LOGO_TYPE_VIDEO
static volatile usb_osd_logo_request_t logo_pending_request =
    USB_OSD_LOGO_REQUEST_REPLAY_ONCE;
static volatile usb_osd_logo_state_t logo_state = USB_OSD_LOGO_STATE_IDLE;
static volatile unsigned int logo_request_sequence = 1;
static volatile unsigned int logo_pending_epoch = 0;

#define LOGO_PLAYER_UPDATE_PERIOD_MS  50
#define LOGO_PLAYER_RETRY_PERIOD_MS   1000
#define LOGO_PLAYER_STALL_PERIOD_MS   15000
#define LOGO_PLAYER_LAST_FRAME_HOLD_MS 2000
#define LOGO_PLAYER_RETRY_TICKS       \
    (LOGO_PLAYER_RETRY_PERIOD_MS / LOGO_PLAYER_UPDATE_PERIOD_MS)
#define LOGO_PLAYER_STALL_TICKS       \
    (LOGO_PLAYER_STALL_PERIOD_MS / LOGO_PLAYER_UPDATE_PERIOD_MS)

static lv_obj_t *ffmpeg = NULL;
static lv_obj_t *logo_screen = NULL;
static lv_timer_t *logo_player_timer = NULL;
static usb_osd_logo_request_t logo_active_request =
    USB_OSD_LOGO_REQUEST_LOOP;
static usb_osd_logo_state_t logo_state_before_suspend =
    USB_OSD_LOGO_STATE_IDLE;
static unsigned int logo_handled_sequence = 0;
static unsigned int logo_active_epoch = 0;
static unsigned int logo_retry_ticks = 0;
static unsigned int logo_stall_ticks = 0;
static u64 logo_last_play_time = (u64)-1;
static rt_tick_t logo_hold_deadline = 0;
static bool logo_screen_prepared = false;
static bool logo_finished_notified = false;
static bool logo_started_notified = false;

static void ffmpeg_player_update_cb(lv_timer_t *timer);

static void logo_player_ensure_timer(void)
{
    if (logo_player_timer)
        return;

    logo_player_timer = lv_timer_create(ffmpeg_player_update_cb,
                                        LOGO_PLAYER_UPDATE_PERIOD_MS,
                                        NULL);
    if (logo_player_timer)
        lv_timer_resume(logo_player_timer);
}
#endif

RT_WEAK void usb_osd_logo_playback_finished(unsigned int epoch)
{
    (void)epoch;
}

RT_WEAK void usb_osd_logo_released(unsigned int epoch)
{
    (void)epoch;
}

RT_WEAK void usb_osd_logo_first_frame_ready(unsigned int epoch)
{
    (void)epoch;
}

RT_WEAK void usb_osd_host_power_changed(bool awake)
{
    (void)awake;
}

/*
 * The USB Display entry overrides this.  Keeping the weak ready default makes
 * the OSD widget usable in products which do not link the Display template.
 */
RT_WEAK bool usb_display_runtime_is_ready(void)
{
    return true;
}

#ifdef LV_USB_OSD_LOGO_TYPE_VIDEO
void usb_osd_logo_post_request(usb_osd_logo_request_t request,
                               unsigned int epoch)
{
    size_t flag;

    if ((unsigned int)request > (unsigned int)USB_OSD_LOGO_REQUEST_RELEASE)
        return;

    flag = aicos_enter_critical_section();
    logo_pending_request = request;
    logo_pending_epoch = epoch;
    logo_request_sequence++;
    aicos_leave_critical_section(flag);
}

void usb_osd_logo_play_loop(unsigned int epoch)
{
    usb_osd_logo_post_request(USB_OSD_LOGO_REQUEST_LOOP, epoch);
}

void usb_osd_logo_play_once_current(unsigned int epoch)
{
    usb_osd_logo_post_request(USB_OSD_LOGO_REQUEST_ONCE_CURRENT, epoch);
}

void usb_osd_logo_replay_once(unsigned int epoch)
{
    usb_osd_logo_post_request(USB_OSD_LOGO_REQUEST_REPLAY_ONCE, epoch);
}

void usb_osd_logo_suspend(unsigned int epoch)
{
    usb_osd_logo_post_request(USB_OSD_LOGO_REQUEST_SUSPEND, epoch);
}

void usb_osd_logo_release(unsigned int epoch)
{
    usb_osd_logo_post_request(USB_OSD_LOGO_REQUEST_RELEASE, epoch);
}

usb_osd_logo_state_t usb_osd_logo_get_state(void)
{
    usb_osd_logo_state_t state;
    size_t flag;

    flag = aicos_enter_critical_section();
    state = logo_state;
    aicos_leave_critical_section(flag);

    return state;
}
#endif /* LV_USB_OSD_LOGO_TYPE_VIDEO */

static lv_osd_manager_t* usb_osd_get_ui_manager()
{
    return instance;
}

static void blank_timer_cb(lv_timer_t *tmr)
{
    lv_osd_manager_t* mgr = tmr->user_data;

    if (!mgr)
        return;

    if (!lv_get_logo_state())
        /* the status of the logo screen has been changed */
        return;

    if (mgr->usb_suspend) {
        mgr->screen_blank = true;
        lvgl_put_tp();
        if (mgr->fb)
            mpp_fb_ioctl(mgr->fb, AICFB_POWEROFF, 0);
        if (mgr->osd_sem)
            rt_sem_take(mgr->osd_sem, RT_WAITING_FOREVER);
    }
}

void handle_logo_state(lv_osd_manager_t* mgr)
{
    lv_obj_t * screen = mgr->screens[SETTINGS_SCREEN];

    if (!mgr->usb_suspend || !(mgr->new_state & STATE_LOGO))
        return;

    USB_OSD_DEBUG("%s\n", __func__);

    lv_scr_load(screen);
    lv_settings_disp_logo();

    unsigned int blank_time = lv_get_screen_blank_time_ms();
    if (blank_time > 0) {
        lv_timer_t* timer = lv_timer_create(blank_timer_cb, blank_time, mgr);
        lv_timer_set_repeat_count(timer, 1);
    }

    if (mgr->usb_suspend) {
        mgr->adjusted_alpha_mode = AICFB_GLOBAL_ALPHA_MODE;
        mgr->adjusted_alpha_value = 255;
    } else {
        mgr->adjusted_alpha_mode = AICFB_GLOBAL_ALPHA_MODE;
        mgr->adjusted_alpha_value = 0;
    }

}

void handle_usb_resume_state(lv_osd_manager_t* mgr)
{
    (void)mgr;
    USB_OSD_DEBUG("%s\n", __func__);
}

void handle_pictures_state(lv_osd_manager_t* mgr)
{
    lv_obj_t * screen = mgr->screens[SETTINGS_SCREEN];

    if (!mgr->usb_suspend || !(mgr->new_state & STATE_PICTURES))
        return;

    lv_scr_load(screen);

    lv_settings_disp_pic();

    if (mgr->usb_suspend) {
        mgr->adjusted_alpha_mode = AICFB_GLOBAL_ALPHA_MODE;
        mgr->adjusted_alpha_value = 255;
    } else {
        mgr->adjusted_alpha_mode = AICFB_GLOBAL_ALPHA_MODE;
        mgr->adjusted_alpha_value = 0;
    }
}

void handle_settings_state(lv_osd_manager_t* mgr)
{
    lv_obj_t * screen = mgr->screens[SETTINGS_SCREEN];
    bool usb_suspend = mgr->new_state & STATE_USB_SUSPEND;

    USB_OSD_DEBUG("%s\n", __func__);

    lv_scr_load(screen);

    lv_settings_changed_state(usb_suspend);

    if (usb_suspend) {
        if (mgr->new_state & STATE_VIDEO) {
            mgr->adjusted_alpha_mode = AICFB_PIXEL_ALPHA_MODE;
            mgr->adjusted_alpha_value = 0;
        } else {
            mgr->adjusted_alpha_mode = AICFB_GLOBAL_ALPHA_MODE;
            mgr->adjusted_alpha_value = 255;
        }
    } else {
        mgr->adjusted_alpha_mode = AICFB_MIXDER_ALPHA_MODE;
        mgr->adjusted_alpha_value = 220;
    }
}

void handle_blank_state(lv_osd_manager_t* mgr)
{
    mgr->screen_blank = true;
    USB_OSD_DEBUG("%s\n", __func__);
}

void handle_video_state(lv_osd_manager_t* mgr)
{
    if (!mgr->usb_suspend || (mgr->current_state & STATE_VIDEO))
        return;

    lv_obj_t * screen = mgr->screens[VIDEO_SCREEN];
    lv_scr_load(screen);
    lv_video_play();

    USB_OSD_DEBUG("%s\n", __func__);

    mgr->adjusted_alpha_mode = AICFB_PIXEL_ALPHA_MODE;
    mgr->adjusted_alpha_value = 0;
}

static const struct state_info station[] = {
    {
        .osd_state = STATE_USB_RESUME,
        .handler = handle_usb_resume_state,
    },
    {
        .osd_state = STATE_LOGO,
        .handler = handle_logo_state,
    },
    {
        .osd_state = STATE_PICTURES,
        .handler = handle_pictures_state,
    },
    {
        .osd_state = STATE_VIDEO,
        .handler = handle_video_state,
    },
    {
        .osd_state = STATE_SETTINGS,
        .handler = handle_settings_state,
    },
    {
        .osd_state = STATE_BLANK,
        .handler = handle_blank_state,
    },
};

static bool is_usb_osd_state_modify(lv_osd_manager_t* mgr)
{
    if (mgr->current_state != mgr->new_state)
        return true;

    if (mgr->current_alpha_mode != mgr->adjusted_alpha_mode ||
        mgr->current_alpha_value != mgr->adjusted_alpha_value)
        return true;

    if (mgr->screen_blank)
        return true;

    return false;
}

static void usb_osd_set_alpha_mode(struct mpp_fb* fb, unsigned int mode, unsigned int value)
{
    struct aicfb_alpha_config alpha = {0};
    int ret;

    if (!fb)
        return;

    alpha.layer_id = AICFB_LAYER_TYPE_UI;
    alpha.enable = 1;
    alpha.mode = mode;
    alpha.value = value;
    ret = mpp_fb_ioctl(fb, AICFB_UPDATE_ALPHA_CONFIG, &alpha);
    if (ret < 0)
        printf("set alpha failed\n");
}

#ifdef LV_USB_OSD_LOGO_TYPE_IMAGE
static void usb_osd_apply_alpha(lv_osd_manager_t *mgr, unsigned int mode,
                                unsigned int value)
{
    usb_osd_set_alpha_mode(mgr->fb, mode, value);
    mgr->current_alpha_mode = mode;
    mgr->current_alpha_value = value;
    mgr->adjusted_alpha_mode = mode;
    mgr->adjusted_alpha_value = value;
}

/*
 * cdc_image_processor_blank_output() deliberately disables the UI layer
 * during suspend so a retained CDC frame cannot leak through.  POWERON and an
 * LVGL redraw do not re-enable layer.enable.  Restore that single hardware
 * bit only when the static PNG has explicitly taken ownership; otherwise a
 * post-suspend Logo is rendered into an invisible layer and the panel shows
 * backlight only for this and every later USB re-plug.
 */
static void usb_osd_static_logo_enable_ui_layer(lv_osd_manager_t *mgr)
{
    struct aicfb_layer_data layer = {0};

    if (!mgr || !mgr->fb)
        return;

    layer.layer_id = AICFB_LAYER_TYPE_UI;
    layer.rect_id = 0;
    if (mpp_fb_ioctl(mgr->fb, AICFB_GET_LAYER_CONFIG, &layer) < 0)
        return;

    if (!layer.enable) {
        layer.enable = 1;
        mpp_fb_ioctl(mgr->fb, AICFB_UPDATE_LAYER_CONFIG, &layer);
    }
}

/* Recreate the image object on every SHOW.  The V1.64 suspend path clears the
 * complete UI framebuffer, so merely invalidating the pre-suspend object is
 * not sufficient on all panels/LVGL cache states. */
static bool usb_osd_static_logo_cache(void)
{
    static const char logo_path[] = LVGL_DIR USB_OSD_UI_LOGO_IMAGE;
    const lv_image_decoder_args_t args = {
        .no_cache = true,
        .premultiply = false,
        .stride_align = false,
        .use_indexed = true,
    };
    lv_image_decoder_dsc_t decoder;

    if (static_logo_pixels)
        return true;

    if (lv_image_decoder_open(&decoder, logo_path, &args) != LV_RESULT_OK)
        return false;

    static_logo_pixels = lv_draw_buf_dup(decoder.decoded);
    lv_image_decoder_close(&decoder);
    return static_logo_pixels != NULL;
}

static unsigned int usb_osd_static_logo_pixel_size(enum mpp_pixel_format fmt)
{
    switch (fmt) {
    case MPP_FMT_ARGB_8888:
    case MPP_FMT_ABGR_8888:
    case MPP_FMT_RGBA_8888:
    case MPP_FMT_BGRA_8888:
    case MPP_FMT_XRGB_8888:
    case MPP_FMT_XBGR_8888:
    case MPP_FMT_RGBX_8888:
    case MPP_FMT_BGRX_8888:
        return 4;
    case MPP_FMT_RGB_888:
    case MPP_FMT_BGR_888:
        return 3;
    case MPP_FMT_ARGB_1555:
    case MPP_FMT_ABGR_1555:
    case MPP_FMT_RGBA_5551:
    case MPP_FMT_BGRA_5551:
    case MPP_FMT_RGB_565:
    case MPP_FMT_BGR_565:
    case MPP_FMT_ARGB_4444:
    case MPP_FMT_ABGR_4444:
    case MPP_FMT_RGBA_4444:
    case MPP_FMT_BGRA_4444:
        return 2;
    default:
        return 0;
    }
}

static void usb_osd_static_logo_write_pixel(uint8_t *dst,
                                             enum mpp_pixel_format fmt,
                                             uint8_t r, uint8_t g,
                                             uint8_t b)
{
    uint32_t value32;
    uint16_t value16;

    switch (fmt) {
    case MPP_FMT_ARGB_8888:
    case MPP_FMT_XRGB_8888:
        value32 = 0xff000000U | ((uint32_t)r << 16) |
                  ((uint32_t)g << 8) | b;
        memcpy(dst, &value32, sizeof(value32));
        break;
    case MPP_FMT_ABGR_8888:
    case MPP_FMT_XBGR_8888:
        value32 = 0xff000000U | ((uint32_t)b << 16) |
                  ((uint32_t)g << 8) | r;
        memcpy(dst, &value32, sizeof(value32));
        break;
    case MPP_FMT_RGBA_8888:
    case MPP_FMT_RGBX_8888:
        value32 = ((uint32_t)r << 24) | ((uint32_t)g << 16) |
                  ((uint32_t)b << 8) | 0xffU;
        memcpy(dst, &value32, sizeof(value32));
        break;
    case MPP_FMT_BGRA_8888:
    case MPP_FMT_BGRX_8888:
        value32 = ((uint32_t)b << 24) | ((uint32_t)g << 16) |
                  ((uint32_t)r << 8) | 0xffU;
        memcpy(dst, &value32, sizeof(value32));
        break;
    case MPP_FMT_RGB_888:
        dst[0] = b; dst[1] = g; dst[2] = r;
        break;
    case MPP_FMT_BGR_888:
        dst[0] = r; dst[1] = g; dst[2] = b;
        break;
    case MPP_FMT_RGB_565:
        value16 = ((uint16_t)(r >> 3) << 11) |
                  ((uint16_t)(g >> 2) << 5) | (b >> 3);
        memcpy(dst, &value16, sizeof(value16));
        break;
    case MPP_FMT_BGR_565:
        value16 = ((uint16_t)(b >> 3) << 11) |
                  ((uint16_t)(g >> 2) << 5) | (r >> 3);
        memcpy(dst, &value16, sizeof(value16));
        break;
    default:
        /* The product UI framebuffer is ARGB8888.  Keep uncommon 16-bit
         * alpha formats black instead of risking an incorrectly packed Logo. */
        memset(dst, 0, usb_osd_static_logo_pixel_size(fmt));
        break;
    }
}

static bool usb_osd_static_logo_commit(lv_osd_manager_t *mgr)
{
    struct aicfb_screeninfo info = {0};
    lv_display_t *disp = lv_display_get_default();
    lv_display_rotation_t rotation;
    const lv_image_header_t *header;
    const lv_color32_t *src;
    uint8_t *fb;
    unsigned int bpp;
    int logical_w, logical_h, logo_x, logo_y;
    unsigned int x, y;
    int pan_id = 0;

    if (!mgr || !mgr->fb || !disp || !usb_osd_static_logo_cache())
        return false;
    if (mpp_fb_ioctl(mgr->fb, AICFB_GET_SCREENINFO, &info) < 0 ||
        !info.framebuffer)
        return false;

    bpp = usb_osd_static_logo_pixel_size(info.format);
    if (!bpp || info.stride < info.width * bpp)
        return false;

    header = &static_logo_pixels->header;
    if (header->cf != LV_COLOR_FORMAT_ARGB8888)
        return false;

    rotation = lv_display_get_rotation(disp);
    logical_w = lv_display_get_horizontal_resolution(disp);
    logical_h = lv_display_get_vertical_resolution(disp);
#if LV_USB_OSD_LOGO_IMAGE_X < 0 || LV_USB_OSD_LOGO_IMAGE_Y < 0
    logo_x = (logical_w - (int)header->w) / 2;
    logo_y = (logical_h - (int)header->h) / 2;
#else
    logo_x = LV_USB_OSD_LOGO_IMAGE_X;
    logo_y = LV_USB_OSD_LOGO_IMAGE_Y;
#endif

    /* The Logo owns UI while this runs, so selecting buffer 0 is deterministic.
     * CDC will select its own buffer after its first complete decoded frame. */
    fb = info.framebuffer;
    memset(fb, 0, info.height * info.stride);
    src = (const lv_color32_t *)static_logo_pixels->data;
    for (y = 0; y < header->h; y++) {
        for (x = 0; x < header->w; x++) {
            const lv_color32_t *pixel =
                (const lv_color32_t *)((const uint8_t *)src +
                    y * header->stride + x * sizeof(lv_color32_t));
            int lx = logo_x + (int)x;
            int ly = logo_y + (int)y;
            int px, py;
            uint8_t alpha, r, g, b;

            if (lx < 0 || ly < 0 || lx >= logical_w || ly >= logical_h)
                continue;
            switch (rotation) {
            case LV_DISPLAY_ROTATION_90:
                px = (int)info.width - 1 - ly;
                py = lx;
                break;
            case LV_DISPLAY_ROTATION_180:
                px = (int)info.width - 1 - lx;
                py = (int)info.height - 1 - ly;
                break;
            case LV_DISPLAY_ROTATION_270:
                px = ly;
                py = (int)info.height - 1 - lx;
                break;
            default:
                px = lx;
                py = ly;
                break;
            }
            if (px < 0 || py < 0 || px >= (int)info.width ||
                py >= (int)info.height)
                continue;

            alpha = pixel->alpha;
            r = ((uint16_t)pixel->red * alpha + 127) / 255;
            g = ((uint16_t)pixel->green * alpha + 127) / 255;
            b = ((uint16_t)pixel->blue * alpha + 127) / 255;
            usb_osd_static_logo_write_pixel(
                fb + py * info.stride + px * bpp, info.format, r, g, b);
        }
    }

    aicos_dcache_clean_invalid_range((ulong *)fb,
        (ulong)ALIGN_UP(info.height * info.stride, CACHE_LINE_SIZE));
    if (mpp_fb_ioctl(mgr->fb, AICFB_PAN_DISPLAY, &pan_id) < 0)
        return false;
    /* Cold power-on is different from USB re-plug/resume: fb0 already exists
     * while the panel/DSI scanout may still be starting.  Do not publish Logo
     * visibility until two real scan boundaries have completed. */
    if (mpp_fb_ioctl(mgr->fb, AICFB_WAIT_FOR_VSYNC, 0) < 0)
        return false;
    if (mpp_fb_ioctl(mgr->fb, AICFB_WAIT_FOR_VSYNC, 0) < 0)
        return false;
    return true;
}

static bool usb_osd_handle_static_logo_request(lv_osd_manager_t *mgr)
{
    enum usb_osd_static_logo_request request;
    unsigned int epoch;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    if (static_logo_queue_head == static_logo_queue_tail) {
        rt_hw_interrupt_enable(level);
        return false;
    }
    request = static_logo_queue[static_logo_queue_head].request;
    epoch = static_logo_queue[static_logo_queue_head].epoch;
    static_logo_queue_head =
        (static_logo_queue_head + 1) % USB_OSD_STATIC_LOGO_QUEUE_DEPTH;
    rt_hw_interrupt_enable(level);

    if (request == USB_OSD_STATIC_LOGO_NONE) {
        return false;
    }

    if (request == USB_OSD_STATIC_LOGO_SHOW) {
        if (!static_logo_show_requested || epoch != static_logo_requested_epoch)
            return true;
        /*
         * Static Logo bypasses the LVGL object/render queue.  Freeze LVGL
         * invalidation, synchronously write the cached PNG into UI buffer 0,
         * pan it on VSYNC, and only then publish visibility.  CONFIGURED may
         * precede Display D0, so power the framebuffer here while PWM is gated.
         */
        aic_panel_backlight_disable();
        if (!mgr->fb || mpp_fb_ioctl(mgr->fb, AICFB_POWERON, 0) < 0) {
            usb_osd_static_logo_post(USB_OSD_STATIC_LOGO_SHOW, epoch);
            return true;
        }
        aic_panel_backlight_disable();
        lv_display_enable_invalidation(lv_display_get_default(), false);
        usb_osd_static_logo_enable_ui_layer(mgr);
        usb_osd_apply_alpha(mgr, AICFB_GLOBAL_ALPHA_MODE, 255);
        if (!usb_osd_static_logo_commit(mgr)) {
            /* Panel timing is not ready yet.  Preserve ownership and retry on
             * the next 20-ms static-Logo tick; never consume this SHOW as if a
             * user-visible frame had been committed. */
            aic_panel_backlight_disable();
            usb_osd_static_logo_post(USB_OSD_STATIC_LOGO_SHOW, epoch);
            return true;
        }
        static_logo_active_epoch = epoch;
        static_logo_session_visible = true;
        /* usb_disp_suspend_exit() has already completed framebuffer POWERON
         * and touch resume.  Consume its delayed generic USB_RESUME state now
         * so the next 200-ms UI tick cannot turn this buffer transparent. */
        if (mgr->new_state == STATE_USB_RESUME)
            mgr->current_state = STATE_USB_RESUME;
        usb_osd_static_logo_visible(epoch);
    } else if (request == USB_OSD_STATIC_LOGO_HIDE) {
        if (!static_logo_session_visible ||
            epoch != static_logo_active_epoch)
            return true;
        usb_osd_static_logo_cancel_show(epoch);
        static_logo_session_visible = false;
        usb_osd_apply_alpha(mgr, AICFB_GLOBAL_ALPHA_MODE, 0);
        lv_display_enable_invalidation(lv_display_get_default(), true);
        usb_osd_static_logo_released(epoch);
    } else {
        if (!static_logo_session_visible ||
            epoch != static_logo_active_epoch)
            return true;
        /* V1.64: CDC owns this same UI layer, so keep it opaque. */
        usb_osd_static_logo_cancel_show(epoch);
        static_logo_session_visible = false;
        lv_display_enable_invalidation(lv_display_get_default(), true);
        usb_osd_apply_alpha(mgr, AICFB_GLOBAL_ALPHA_MODE, 255);
        usb_osd_static_logo_released(epoch);
    }

    return true;
}

static void usb_osd_static_logo_timer_cb(lv_timer_t *timer)
{
    lv_osd_manager_t *mgr = (lv_osd_manager_t *)timer->user_data;

    if (mgr)
        usb_osd_handle_static_logo_request(mgr);
}
#endif /* LV_USB_OSD_LOGO_TYPE_IMAGE */

static void usb_osd_ui_callback(lv_timer_t* tmr)
{
    lv_osd_manager_t* mgr = (lv_osd_manager_t*)tmr->user_data;
    unsigned int mode, value;

#ifdef LV_USB_OSD_LOGO_TYPE_VIDEO
    /* Retry timer allocation from an already-running LVGL timer if needed. */
    logo_player_ensure_timer();
#endif

    if (!mgr)
        return;

#ifdef LV_USB_OSD_LOGO_TYPE_IMAGE
    /*
     * The static PNG has its own ownership protocol (show/hide/release-cdc)
     * driven by the USB mode arbiter.  Handle it before the generic OSD state
     * machine so a pending request is never overwritten by a state change.
     */
    if (usb_osd_handle_static_logo_request(mgr))
        return;

    /* Keep generic state/blanking handlers out of a visible boot or wake PNG.
     * SHOW/HIDE/RELEASE_CDC above still run first and retain their old timing. */
    if (usb_osd_static_logo_mask_required())
        return;

#endif

    if (!is_usb_osd_state_modify(mgr))
        return;

    if (mgr->screen_blank) {
        if (mgr->fb)
            mpp_fb_ioctl(mgr->fb, AICFB_POWEROFF, 0);
        if (mgr->osd_sem)
            rt_sem_take(mgr->osd_sem, RT_WAITING_FOREVER);
    }

    USB_OSD_DEBUG("current_state(%d) switch new state(%d)\n",
                  mgr->current_state, mgr->new_state);
    USB_OSD_DEBUG("current alpha mode(%d), value(%d)\n",
                  mgr->current_alpha_mode, mgr->current_alpha_value);

    if (mgr->current_state != mgr->new_state) {
        unsigned int state = mgr->current_state ^ mgr->new_state;

        for (int i = 0; i < ARRAY_SIZE(station); i++) {
            if (state & station[i].osd_state) {
                station[i].handler(mgr);
            }
        }

        mgr->current_state = mgr->new_state;
    } else {
        mode = mgr->adjusted_alpha_mode;
        value = mgr->adjusted_alpha_value;

        usb_osd_set_alpha_mode(mgr->fb, mode, value);
        mgr->current_alpha_mode = mode;
        mgr->current_alpha_value = value;
    }
}

static void usb_osd_ui_theme_set(void)
{
    lv_disp_t * disp = lv_disp_get_default();

    lv_theme_t * th = lv_theme_default_init(disp,
                                            lv_palette_main(LV_PALETTE_BLUE),
                                            lv_palette_main(LV_PALETTE_RED),
                                            true,   /* dark theme */
                                            LV_FONT_DEFAULT);
    lv_disp_set_theme(disp, th);
}

#ifdef LV_USB_OSD_SETTINGS_MENU
static bool wake_up_key = false;

static void gpio_input_irq_handler(void *args)
{
    wake_up_key = true;
}

static void keypad_init(void)
{
    u32 pin;

    /* 1.get pin number */
    pin = rt_pin_get(USB_OSD_WAKEUP_KEY);

    /* 2.set pin mode to Input-PullUp */
    rt_pin_mode(pin, PIN_MODE_INPUT_PULLUP);

    /* 3.attach irq handler */
    rt_pin_attach_irq(pin, PIN_IRQ_MODE_FALLING,
                      gpio_input_irq_handler, &pin);

    /* 4.enable pin irq */
    rt_pin_irq_enable(pin, PIN_IRQ_ENABLE);
}

static uint32_t keypad_get_key(void)
{
    if (wake_up_key) {
        wake_up_key = false;
        return 1;
    }

    return 0;
}

static void keypad_read(lv_indev_t * indev_drv, lv_indev_data_t * data)
{
    static uint32_t last_key = 0;

    uint32_t act_key = keypad_get_key();
    if (act_key != 0) {
        data->state = LV_INDEV_STATE_PR;

        act_key = LV_KEY_ENTER;
        last_key = act_key;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }

    data->key = last_key;
}

static void wakeup_key_event_callback(lv_event_t *e)
{
    struct osd_manager *mgr = usb_osd_get_ui_manager();

    if (!mgr)
        return;

    if (mgr->current_state & STATE_SETTINGS)
        mgr->new_state &= ~(STATE_SETTINGS);
    else
        mgr->new_state |= STATE_SETTINGS;
}
#endif

static void usb_osd_wakeup_key_register(void)
{
#ifdef LV_USB_OSD_SETTINGS_MENU
    keypad_init();

    lv_indev_t * indev_keypad;
    indev_keypad = lv_indev_create();
    lv_indev_set_type(indev_keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev_keypad, keypad_read);

    lv_group_t * group = lv_group_create();
    lv_group_add_obj(group, lv_scr_act());
    lv_indev_set_group(indev_keypad, group);

    lv_indev_add_event_cb(indev_keypad, wakeup_key_event_callback, LV_EVENT_CLICKED, NULL);
#endif
}

static int usb_osd_mode_to_state(unsigned int lock_mode)
{
    unsigned int state = 0;

    if (lock_mode == DISPLAY_LOGO)
        state |= STATE_LOGO;
    else if (lock_mode == DISPLAY_PICTURES)
        state |= STATE_PICTURES;
    else if (lock_mode == DISPLAY_VIDEO)
        state |= STATE_VIDEO;
    else if (lock_mode == BLANK_SCREEN)
        state |= STATE_BLANK;

    USB_OSD_DEBUG("osd lock mode: %d\n", lock_mode);

    return state;
}

void usb_disp_suspend_enter(void)
{
    struct osd_manager *mgr;
    unsigned int mode;

    usb_osd_host_power_changed(false);
    mgr = usb_osd_get_ui_manager();

    if (!mgr || mgr->usb_suspend)
        return;

    if (lv_get_screen_lock_time_ms() == 0) {
        /* Never Lock Screen */
        return;
    }

    mode = lv_get_screen_lock_mode();

    mgr->new_state = STATE_USB_SUSPEND | usb_osd_mode_to_state(mode);
    mgr->usb_suspend = true;

    if (mgr->new_state & STATE_BLANK)
        mgr->screen_blank = true;

    USB_OSD_DEBUG("usb suspend, osd new_state: %d\n", mgr->new_state);

    lvgl_get_tp();
}

void usb_disp_suspend_exit(void)
{
    struct osd_manager *mgr;

    mgr = usb_osd_get_ui_manager();

    if (!mgr) {
        usb_osd_host_power_changed(true);
        return;
    }

    if (mgr->usb_suspend)
        lvgl_put_tp();

    usb_osd_set_alpha_mode(mgr->fb, AICFB_GLOBAL_ALPHA_MODE, 0);
    if (mgr->fb)
        mpp_fb_ioctl(mgr->fb, AICFB_POWERON, 0);

    if (mgr->screen_blank) {
        if (mgr->osd_sem)
            rt_sem_release(mgr->osd_sem);
        mgr->screen_blank = false;
    }

    if (mgr->current_state & STATE_VIDEO)
        lv_video_stop();

    mgr->new_state = STATE_USB_RESUME;
    mgr->current_alpha_mode = AICFB_GLOBAL_ALPHA_MODE;
    mgr->current_alpha_value = 0;
    mgr->adjusted_alpha_mode = AICFB_GLOBAL_ALPHA_MODE;
    mgr->adjusted_alpha_value = 0;
    mgr->usb_suspend = false;
    mgr->screen_blank = false;
    USB_OSD_DEBUG("%s\n", __func__);

    /*
     * POWERON and the alpha reset above restore the Display framebuffer.  Tell
     * the USB mode owner only after that work is complete, so a retained CDC
     * session can make the final layer/source decision for this wake.
     */
    usb_osd_host_power_changed(true);
}

int usb_disp_get_suspend_ms(void)
{
    int suspend_ms = lv_get_screen_lock_time_ms();

    USB_OSD_DEBUG("usb disp suspend ms: %d\n", suspend_ms);
    return suspend_ms;
}

#ifdef LV_USB_OSD_LOGO_TYPE_VIDEO
static const char logo_video_src[] =
    USB_OSD_UI_LOGO_VIDEO_PATH USB_OSD_UI_LOGO_VIDEO;

static void logo_player_set_state(usb_osd_logo_state_t state)
{
    size_t flag;

    flag = aicos_enter_critical_section();
    logo_state = state;
    aicos_leave_critical_section(flag);
}

static void logo_player_prepare_screen(void)
{
    if (logo_screen_prepared)
        return;

    logo_screen = lv_scr_act();
    if (!logo_screen)
        return;

    lv_obj_clean(logo_screen);
    lv_obj_set_style_bg_color(logo_screen, lv_color_hex(0x000000),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(logo_screen, LV_OPA_COVER,
                            LV_PART_MAIN | LV_STATE_DEFAULT);
    logo_screen_prepared = true;
}

static void logo_player_destroy(void)
{
    if (!ffmpeg)
        return;

    lv_obj_del(ffmpeg);
    ffmpeg = NULL;
}

static int logo_player_get_rotation(void)
{
    lv_display_rotation_t rotation = lv_display_get_rotation(NULL);

#if defined(LV_DISPLAY_ROTATE_EN)
    static const unsigned int lvgl_rotation[] = {
        LV_DISPLAY_ROTATION_0,
        LV_DISPLAY_ROTATION_270,
        LV_DISPLAY_ROTATION_180,
        LV_DISPLAY_ROTATION_90
    };

    return (int)lvgl_rotation[rotation] * 90;
#elif defined(AIC_FB_ROTATE_EN)
    return AIC_FB_ROTATE_DEGREE;
#else
    return (int)rotation * 90;
#endif
}

static void logo_player_notify_finished(void)
{
    if (logo_finished_notified)
        return;

    logo_finished_notified = true;
    usb_osd_logo_playback_finished(logo_active_epoch);
}

static void logo_player_reset_watchdog(void)
{
    logo_stall_ticks = 0;
    logo_last_play_time = (u64)-1;
    logo_started_notified = false;
}

static bool logo_player_is_stalled(void)
{
    bool video_first_frame = false;
    u64 play_time = (u64)-1;

    if (!logo_started_notified) {
        lv_ffmpeg_player_set_cmd_ex(
            ffmpeg, LV_FFMPEG_PLAYER_CMD_VIDEO_FIRST_FRAME_EX,
            &video_first_frame);
        if (video_first_frame) {
            logo_started_notified = true;
            usb_osd_logo_first_frame_ready(logo_active_epoch);
        }
    }

    lv_ffmpeg_player_set_cmd_ex(ffmpeg,
                                LV_FFMPEG_PLAYER_CMD_GET_PLAY_TIME_EX,
                                &play_time);
    if (play_time == (u64)-1) {
        if (logo_stall_ticks < LOGO_PLAYER_STALL_TICKS)
            logo_stall_ticks++;
        return logo_stall_ticks >= LOGO_PLAYER_STALL_TICKS;
    }

    if (play_time != logo_last_play_time) {
        logo_last_play_time = play_time;
        logo_stall_ticks = 0;
        return false;
    }

    if (logo_stall_ticks < LOGO_PLAYER_STALL_TICKS)
        logo_stall_ticks++;

    return logo_stall_ticks >= LOGO_PLAYER_STALL_TICKS;
}

static bool logo_player_start(bool loop)
{
    s32 logo_volume = 0;

    logo_player_destroy();
    logo_finished_notified = false;
    logo_hold_deadline = 0;
    logo_player_reset_watchdog();

    if (!logo_screen)
        goto failed;

    ffmpeg = lv_ffmpeg_player_create(logo_screen);
    if (!ffmpeg)
        goto failed;

    lv_obj_set_pos(ffmpeg, 0, 0);
    lv_obj_set_size(ffmpeg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_transform_angle(ffmpeg, logo_player_get_rotation(),
                                     LV_PART_MAIN);

    /* The video is rendered by the MPP layer; the LVGL object only owns it. */
    lv_obj_add_flag(ffmpeg, LV_OBJ_FLAG_HIDDEN);
    lv_ffmpeg_player_set_auto_restart(ffmpeg, loop);

    if (lv_ffmpeg_player_set_src(ffmpeg, logo_video_src) != LV_RES_OK)
        goto failed;

    /* Preserve the final video frame while Display prepares its first frame. */
    lv_ffmpeg_player_set_cmd_ex(ffmpeg,
                                LV_FFMPEG_PLAYED_CMD_KEEP_LAST_FRAME_EX, NULL);
    lv_ffmpeg_player_set_cmd_ex(ffmpeg, LV_FFMPEG_PLAYER_CMD_START, NULL);
    /* Keep the AAC queue consumed for A/V demux flow, but output no sound. */
    lv_ffmpeg_player_set_cmd_ex(ffmpeg,
                                LV_FFMPEG_PLAYER_CMD_SET_VOLUME_EX,
                                &logo_volume);
    logo_retry_ticks = 0;
    logo_player_set_state(loop ? USB_OSD_LOGO_STATE_LOOPING :
                                 USB_OSD_LOGO_STATE_PLAYING_ONCE);
    return true;

failed:
    logo_player_destroy();
    logo_player_set_state(USB_OSD_LOGO_STATE_ERROR);

    /* Never enter the system unless one complete video iteration reached EOS. */
    logo_retry_ticks = LOGO_PLAYER_RETRY_TICKS;

    return false;
}

static void logo_player_handle_loop(void)
{
    logo_active_request = USB_OSD_LOGO_REQUEST_LOOP;
    logo_retry_ticks = 0;
    logo_player_reset_watchdog();

    if (!ffmpeg) {
        logo_player_start(true);
        return;
    }

    switch (logo_state) {
    case USB_OSD_LOGO_STATE_LOOPING:
        lv_ffmpeg_player_set_auto_restart(ffmpeg, true);
        break;
    case USB_OSD_LOGO_STATE_PLAYING_ONCE:
        lv_ffmpeg_player_set_auto_restart(ffmpeg, true);
        logo_player_set_state(USB_OSD_LOGO_STATE_LOOPING);
        break;
    case USB_OSD_LOGO_STATE_SUSPENDED:
        if (logo_state_before_suspend == USB_OSD_LOGO_STATE_LOOPING ||
            logo_state_before_suspend == USB_OSD_LOGO_STATE_PLAYING_ONCE) {
            lv_ffmpeg_player_set_auto_restart(ffmpeg, true);
            lv_ffmpeg_player_set_cmd_ex(ffmpeg,
                                        LV_FFMPEG_PLAYER_CMD_RESUME, NULL);
            logo_player_set_state(USB_OSD_LOGO_STATE_LOOPING);
        } else {
            logo_player_start(true);
        }
        break;
    default:
        logo_player_start(true);
        break;
    }
}

static void logo_player_handle_once_current(void)
{
    logo_active_request = USB_OSD_LOGO_REQUEST_ONCE_CURRENT;
    logo_retry_ticks = 0;
    logo_finished_notified = false;
    logo_player_reset_watchdog();

    if (!ffmpeg) {
        logo_player_start(false);
        return;
    }

    switch (logo_state) {
    case USB_OSD_LOGO_STATE_LOOPING:
        /* Do not seek: finish the iteration which is already on screen. */
        lv_ffmpeg_player_set_auto_restart(ffmpeg, false);
        logo_player_set_state(USB_OSD_LOGO_STATE_PLAYING_ONCE);
        break;
    case USB_OSD_LOGO_STATE_PLAYING_ONCE:
        lv_ffmpeg_player_set_auto_restart(ffmpeg, false);
        break;
    case USB_OSD_LOGO_STATE_HOLD_LAST:
        /* Preserve the EOS-based hold deadline; the timer will notify it. */
        break;
    case USB_OSD_LOGO_STATE_SUSPENDED:
        if (logo_state_before_suspend == USB_OSD_LOGO_STATE_LOOPING ||
            logo_state_before_suspend == USB_OSD_LOGO_STATE_PLAYING_ONCE) {
            lv_ffmpeg_player_set_auto_restart(ffmpeg, false);
            lv_ffmpeg_player_set_cmd_ex(ffmpeg,
                                        LV_FFMPEG_PLAYER_CMD_RESUME, NULL);
            logo_player_set_state(USB_OSD_LOGO_STATE_PLAYING_ONCE);
        } else if (logo_state_before_suspend ==
                   USB_OSD_LOGO_STATE_HOLD_LAST) {
            logo_player_set_state(USB_OSD_LOGO_STATE_HOLD_LAST);
        } else {
            logo_player_start(false);
        }
        break;
    default:
        logo_player_start(false);
        break;
    }
}

static void logo_player_handle_request(usb_osd_logo_request_t request,
                                       unsigned int epoch)
{
    logo_active_epoch = epoch;
    switch (request) {
    case USB_OSD_LOGO_REQUEST_LOOP:
        logo_player_handle_loop();
        break;
    case USB_OSD_LOGO_REQUEST_ONCE_CURRENT:
        logo_player_handle_once_current();
        break;
    case USB_OSD_LOGO_REQUEST_REPLAY_ONCE:
        logo_active_request = request;
        logo_retry_ticks = 0;
        logo_player_start(false);
        break;
    case USB_OSD_LOGO_REQUEST_SUSPEND:
        logo_active_request = request;
        logo_retry_ticks = 0;
        if (logo_state == USB_OSD_LOGO_STATE_SUSPENDED)
            break;

        logo_state_before_suspend = logo_state;
        if (ffmpeg &&
            (logo_state == USB_OSD_LOGO_STATE_LOOPING ||
             logo_state == USB_OSD_LOGO_STATE_PLAYING_ONCE)) {
            lv_ffmpeg_player_set_cmd_ex(ffmpeg,
                                        LV_FFMPEG_PLAYER_CMD_PAUSE, NULL);
        }
        logo_player_set_state(USB_OSD_LOGO_STATE_SUSPENDED);
        break;
    case USB_OSD_LOGO_REQUEST_RELEASE:
        logo_active_request = request;
        logo_retry_ticks = 0;
        logo_finished_notified = false;
        logo_hold_deadline = 0;
        if (ffmpeg) {
            /*
             * KEEP_LAST_FRAME is only needed while selecting the next owner.
             * Release it before CDC/Display starts rendering or the video
             * layer can remain above the new UI frame indefinitely.
             */
            lv_ffmpeg_player_set_cmd_ex(
                ffmpeg, LV_FFMPEG_PLAYED_CMD_RELEASE_LAST_FRAME_EX, NULL);
        }
        logo_player_destroy();
        logo_player_set_state(USB_OSD_LOGO_STATE_IDLE);
        usb_osd_logo_released(logo_active_epoch);
        break;
    default:
        break;
    }
}

static void ffmpeg_player_update_cb(lv_timer_t *timer)
{
    usb_osd_logo_request_t request;
    unsigned int sequence;
    unsigned int epoch;
    bool handled_request = false;
    bool video_end = false;
    size_t flag;

    (void)timer;

    /*
     * usb_display_init_delay_ms is deliberately zero for a video-logo build.
     * Do not create the H.264 renderer until the Display worker has completed
     * its one-time framebuffer/layer setup, otherwise that setup can flash over
     * an arbitrary movie frame.
     */
    if (!usb_display_runtime_is_ready())
        return;

    logo_player_prepare_screen();

    flag = aicos_enter_critical_section();
    request = logo_pending_request;
    sequence = logo_request_sequence;
    epoch = logo_pending_epoch;
    aicos_leave_critical_section(flag);

    if (sequence != logo_handled_sequence) {
        logo_handled_sequence = sequence;
        logo_player_handle_request(request, epoch);
        handled_request = true;
    }

    if (!handled_request && logo_state == USB_OSD_LOGO_STATE_ERROR) {
        if (logo_retry_ticks > 0)
            logo_retry_ticks--;
        else
            logo_player_start(logo_active_request ==
                              USB_OSD_LOGO_REQUEST_LOOP);
    }

    if (ffmpeg && logo_state == USB_OSD_LOGO_STATE_HOLD_LAST) {
        if (!logo_finished_notified &&
            (rt_int32_t)(rt_tick_get() - logo_hold_deadline) >= 0) {
            logo_player_notify_finished();
        }
        return;
    }

    if (!ffmpeg ||
        (logo_state != USB_OSD_LOGO_STATE_PLAYING_ONCE &&
         logo_state != USB_OSD_LOGO_STATE_LOOPING))
        return;

    if (logo_state == USB_OSD_LOGO_STATE_PLAYING_ONCE) {
        bool play_end = false;

        lv_ffmpeg_player_set_cmd_ex(ffmpeg,
                                    LV_FFMPEG_PLAYER_CMD_VIDEO_END_EX,
                                    &video_end);
        lv_ffmpeg_player_set_cmd_ex(ffmpeg,
                                    LV_FFMPEG_PLAYER_CMD_PLAY_END_EX,
                                    &play_end);
        if (video_end || play_end) {
            /*
             * Keep the object and retained MPP layer alive for a real two
             * seconds after visual EOS.  Only then may the mode arbiter enter
             * CDC/Display, so the final frame cannot be replaced by an early
             * host packet.
             */
            logo_player_set_state(USB_OSD_LOGO_STATE_HOLD_LAST);
            logo_hold_deadline =
                rt_tick_get() +
                rt_tick_from_millisecond(LOGO_PLAYER_LAST_FRAME_HOLD_MS);
            return;
        }
    }

    if (logo_player_is_stalled()) {
        if (logo_state == USB_OSD_LOGO_STATE_LOOPING) {
            logo_player_destroy();
            logo_player_set_state(USB_OSD_LOGO_STATE_ERROR);
            logo_retry_ticks = LOGO_PLAYER_RETRY_TICKS;
        } else {
            /* A stalled partial iteration is invalid: restart from frame zero. */
            logo_player_destroy();
            logo_player_set_state(USB_OSD_LOGO_STATE_ERROR);
            logo_retry_ticks = LOGO_PLAYER_RETRY_TICKS;
        }
    }
}
#endif

static lv_obj_t* create_screen(const char* type)
{
    if (rt_strcmp(type, "video") == 0)
        return lv_video_screen_create();
    else if (rt_strcmp(type, "settings") == 0)
        return lv_settings_screen_create();

    return NULL;
}

bool lv_get_ui_settings_state(void)
{
    lv_osd_manager_t* mgr = usb_osd_get_ui_manager();

    if (!mgr)
        return false;

    return mgr->usb_suspend;
}

void lv_update_ui_settings_state(void)
{
    lv_osd_manager_t* mgr = usb_osd_get_ui_manager();

    if (!mgr)
        return;

    if (mgr->current_state & STATE_SETTINGS)
        mgr->current_state &= ~(STATE_SETTINGS);

    if (mgr->new_state & STATE_SETTINGS)
        mgr->new_state &= ~(STATE_SETTINGS);
}

void lv_blank_screen_enable(bool enable)
{
    lv_osd_manager_t* mgr = usb_osd_get_ui_manager();

    if (!mgr)
        return;

    if (enable)
        mgr->screen_blank = true;
    else
        mgr->screen_blank = false;
}

bool lv_is_usb_disp_suspend(void)
{
    lv_osd_manager_t* mgr = usb_osd_get_ui_manager();

    if (!mgr)
        return false;

    return mgr->usb_suspend;
}

static lv_osd_manager_t* ui_manager_get_instance(void)
{
    instance = rt_malloc(sizeof(lv_osd_manager_t));
    if (!instance)
        return NULL;

    rt_memset(instance, 0, sizeof(lv_osd_manager_t));

    instance->fb = mpp_fb_open();
    instance->osd_sem = rt_sem_create("osdsem", 0, RT_IPC_FLAG_PRIO);

    instance->screens[VIDEO_SCREEN] = create_screen("video");
    instance->screens[SETTINGS_SCREEN] = create_screen("settings");

    instance->current_state = STATE_USB_RESUME;
    instance->current_alpha_mode = AICFB_GLOBAL_ALPHA_MODE;
    instance->current_alpha_value = 0;
    instance->adjusted_alpha_mode = AICFB_GLOBAL_ALPHA_MODE;
    instance->adjusted_alpha_value = 0;

    instance->screen_blank = false;

    return instance;
}

static void usb_osd_ui_display_logo(void)
{
#if defined(LV_USB_OSD_LOGO_TYPE_IMAGE)
    /* Decode once into a small CPU buffer.  All later SHOW operations write
     * it directly to the existing UI framebuffer without LVGL rendering. */
    if (!usb_osd_static_logo_cache())
        rt_kprintf("[E/USB] static logo cache decode failed\n");
#elif defined(LV_USB_OSD_LOGO_TYPE_VIDEO)
    logo_screen = NULL;
    logo_screen_prepared = false;
    logo_player_ensure_timer();
#endif
}

void usb_osd_ui_init(void)
{
    usb_osd_ui_theme_set();

    lv_osd_manager_t* mgr = ui_manager_get_instance();
    if (!mgr)
        return;

    usb_osd_set_alpha_mode(mgr->fb, AICFB_GLOBAL_ALPHA_MODE, 0);
    usb_osd_wakeup_key_register();

    usb_osd_ui_display_logo();
#ifdef LV_USB_OSD_LOGO_TYPE_IMAGE
    /* Only static Logo requests use the fast timer.  Generic OSD state
     * processing remains unchanged at 200 ms. */
    lv_timer_create(usb_osd_static_logo_timer_cb, 20, mgr);
#endif
    lv_timer_create(usb_osd_ui_callback, 200, mgr);
}

void ui_init(void)
{
    usb_osd_ui_init();
}
