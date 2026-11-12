/*
 * Copyright (c) 2024-2025, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:  Huahui Mai <huahui.mai@artinchip.com>
 *
 * V1.64: one OSD widget drives both LOGO products.  The build selects exactly
 * one owner through the LV_USB_OSD_LOGO_TYPE_* choice:
 *
 *   LV_USB_OSD_LOGO_TYPE_VIDEO - local H.264 clip played by the MPP/VE.  The
 *       widget exposes a request/epoch state machine (loop, once, suspend,
 *       release) plus first-frame/finished/released notifications so the USB
 *       mode arbiter can keep the panel dark across every layer hand-over.
 *
 *   LV_USB_OSD_LOGO_TYPE_IMAGE - static PNG drawn into the LVGL framebuffer.
 *       The widget exposes show/hide/release-cdc requests used to cover the
 *       retained host framebuffer while the host source is recovering.
 *
 * Both owners share the same OSD state machine, alpha handling, and USB
 * suspend/resume plumbing below.
 */

#ifndef LV_DEMO_USB_OSD_H
#define LV_DEMO_USB_OSD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "aic_ui.h"
#include "mpp_fb.h"
#include "settings/usb_osd_settings.h"

enum screen_lock_delay {
    SECONDS_15_LOCK,
    SECONDS_30_LOCK,

    MINUTES_1_LOCK,
    MINUTES_2_LOCK,
    MINUTES_5_LOCK,
    MINUTES_10_LOCK,

    SCREEN_NEVER_LOCK,
};

enum screen_blank_delay {
    MINUTES_5_BLANK,
    MINUTES_10_BLANK,
    MINUTES_15_BLANK,
    MINUTES_30_BLANK,

    SCREEN_NEVER_BLANK,
};

enum screen_lock_mode {
    DISPLAY_LOGO,
    DISPLAY_PICTURES,
#ifdef LV_USB_OSD_PLAY_VIDEO
    DISPLAY_VIDEO,
#endif
    BLANK_SCREEN,

    NEVER_LOCK_MODE,
};

#define USB_OSD_WAKEUP_KEY    LV_USB_OSD_SETTINGS_WAKEUP_KEY

/* Static LOGO placement.  Overridable so a panel/rotation change needs no
 * source edit.  A negative value centers the image on the active screen,
 * which matches the V1.64 image product for any rotation setting. */
#ifndef LV_USB_OSD_LOGO_IMAGE_X
#define LV_USB_OSD_LOGO_IMAGE_X (-1)
#endif
#ifndef LV_USB_OSD_LOGO_IMAGE_Y
#define LV_USB_OSD_LOGO_IMAGE_Y (-1)
#endif

#ifdef LV_USB_OSD_LOGO_TYPE_IMAGE
#define USB_OSD_UI_LOGO_IMAGE "logo/"LV_USB_OSD_LOGO_IMAGE
#define USB_OSD_UI_LOGO_ROTATE_IMAGE "logo_rotate/"LV_USB_OSD_LOGO_IMAGE
#endif

#ifdef LV_USB_OSD_LOGO_TYPE_VIDEO
#define USB_OSD_UI_LOGO_VIDEO_PATH  LVGL_STORAGE_PATH "/"
#define USB_OSD_UI_LOGO_VIDEO       LV_USB_OSD_LOGO_VIDEO

/* Kept so a video product can still fall back to the PNG asset. */
#define USB_OSD_UI_LOGO_IMAGE        "logo/logo.png"
#define USB_OSD_UI_LOGO_ROTATE_IMAGE "logo_rotate/logo.png"
#endif

bool lv_is_usb_disp_suspend(void);

#if defined(LV_USB_OSD_LOGO_TYPE_VIDEO)

typedef enum {
    USB_OSD_LOGO_REQUEST_LOOP = 0,
    USB_OSD_LOGO_REQUEST_ONCE_CURRENT,
    USB_OSD_LOGO_REQUEST_REPLAY_ONCE,
    USB_OSD_LOGO_REQUEST_SUSPEND,
    USB_OSD_LOGO_REQUEST_RELEASE,
} usb_osd_logo_request_t;

typedef enum {
    USB_OSD_LOGO_STATE_IDLE = 0,
    USB_OSD_LOGO_STATE_LOOPING,
    USB_OSD_LOGO_STATE_PLAYING_ONCE,
    USB_OSD_LOGO_STATE_HOLD_LAST,
    USB_OSD_LOGO_STATE_SUSPENDED,
    USB_OSD_LOGO_STATE_ERROR,
} usb_osd_logo_state_t;

/*
 * These functions are thread-safe request posters.  The request is consumed
 * asynchronously by the LVGL thread; callers must not operate on the player.
 */
void usb_osd_logo_post_request(usb_osd_logo_request_t request,
                               unsigned int epoch);
void usb_osd_logo_play_loop(unsigned int epoch);
void usb_osd_logo_play_once_current(unsigned int epoch);
void usb_osd_logo_replay_once(unsigned int epoch);
void usb_osd_logo_suspend(unsigned int epoch);
void usb_osd_logo_release(unsigned int epoch);
usb_osd_logo_state_t usb_osd_logo_get_state(void);

/* Weak notifications.  A product state machine may provide strong versions. */
void usb_osd_logo_playback_finished(unsigned int epoch);
void usb_osd_logo_released(unsigned int epoch);
void usb_osd_logo_first_frame_ready(unsigned int epoch);

#endif /* LV_USB_OSD_LOGO_TYPE_VIDEO */

#if defined(LV_USB_OSD_LOGO_TYPE_IMAGE)
/*
 * Thread-safe request posters for the static PNG owner.  The LVGL thread
 * performs the framebuffer power-on, repaint and backlight hand-off.
 */
void usb_osd_static_logo_show(unsigned int epoch);
void usb_osd_static_logo_hide(unsigned int epoch);
void usb_osd_static_logo_release_cdc(unsigned int epoch);

/* Weak notification emitted once logo.png has been committed to the panel. */
void usb_osd_static_logo_visible(unsigned int epoch);
/* Weak notification emitted after the static Logo has been removed. */
void usb_osd_static_logo_released(unsigned int epoch);
#endif /* LV_USB_OSD_LOGO_TYPE_IMAGE */

/* True only while a committed local Logo currently owns the visible panel.
 * Shared by IMAGE and VIDEO; do not change UI alpha while this is true. */
bool usb_osd_logo_backlight_is_owned(void);

/* Weak notification, shared by both LOGO owners. */
void usb_osd_host_power_changed(bool awake);

#if !defined(LV_USB_OSD_PLAY_VIDEO)
#define DISPLAY_VIDEO       (-1)
#endif

typedef enum {
    STATE_USB_SUSPEND = (0x1 << 0),
    STATE_USB_RESUME  = (0x1 << 1),
    STATE_LOGO        = (0x1 << 2),
    STATE_PICTURES    = (0x1 << 3),
    STATE_VIDEO       = (0x1 << 4),
    STATE_SETTINGS    = (0x1 << 5),
    STATE_BLANK       = (0x1 << 6),

    STATE_NUM,

} osd_state_t;


typedef struct osd_manager lv_osd_manager_t;
typedef void (*state_handler)(lv_osd_manager_t*);

struct state_info {
    osd_state_t osd_state;
    void (*handler)(lv_osd_manager_t* mgr);
};

#define VIDEO_SCREEN 0
#define SETTINGS_SCREEN 1

#define SCREEN_NUM  2

struct osd_manager {
    osd_state_t current_state;
    osd_state_t new_state;

    state_handler handlers[STATE_NUM];
    lv_obj_t * screens[SCREEN_NUM];

    struct mpp_fb *fb;
    rt_sem_t osd_sem;

    unsigned int current_alpha_mode;
    unsigned int current_alpha_value;

    unsigned int adjusted_alpha_mode;
    unsigned int adjusted_alpha_value;

    bool usb_suspend;
    bool screen_blank;

    unsigned int flags;
};

bool lv_get_ui_settings_state(void);

void lv_update_ui_settings_state(void);

void lv_blank_screen_enable(bool enable);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /*LV_DEMO_USB_OSD_H*/
