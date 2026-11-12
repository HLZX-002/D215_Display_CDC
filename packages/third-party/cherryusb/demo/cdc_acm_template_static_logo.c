/*
 * Copyright (c) 2022-2025, sakumisu
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbd_core.h"
#include "usbd_cdc.h"
#include "flash_rw.h"
#include "msh.h"
#include "mpp_decoder.h"

#include <rtthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <aic_common.h>
#include <artinchip_fb.h>
#include <mpp_fb.h>

/* endpoint address */
#define CDC_IN_EP  0x82
#define CDC_OUT_EP 0x02
#define CDC_INT_EP 0x81

#define USBD_VID           0x33C3
#define USBD_PID           0xF101
#define USBD_MAX_POWER     100
#define USBD_LANGID_STRING 1033

/* config descriptor size */
#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)

#ifdef CONFIG_USB_HS
#define CDC_MAX_MPS 512
#else
#define CDC_MAX_MPS 64
#endif

#define CDC_RX_BUF_SIZE          2048
#define CDC_TX_BUF_SIZE          128
#define CDC_SN_MAX_LEN           64
#define CDC_MODE_TIMEOUT_MS      2000
#define CDC_MODE_TIMER_PERIOD_MS 100
#define CDC_RX_QUEUE_DEPTH       16
#define CDC_RX_THREAD_STACK      4096
#define CDC_RX_THREAD_PRIO       20

/*
 * Panel blanking watchdog: when neither the Display transport nor the CDC
 * stream delivered any payload for this long, the panel is switched off and
 * the next payload switches it back on.  It must not depend on the host
 * attach/awake bookkeeping: with LPKG_CHERRYUSB_DEVICE_PLUG_CHECK disabled a
 * data-cable unplug produces no USBD_EVENT_DISCONNECTED at all, so the mode
 * arbiter would otherwise keep the retained last host frame on screen.
 */
#define USB_MODE_SOURCE_IDLE_TIMEOUT_MS 2000
/*
 * A static Windows desktop does not continuously submit Display frames.
 * Missing payloads therefore cannot be used as evidence that the PC is
 * asleep or disconnected: doing so makes the backlight toggle whenever the
 * desktop alternates between idle and active.  Real sleep/hibernate remains
 * controlled by the USB suspend and Display power-state callbacks.
 */
#define USB_MODE_SOURCE_IDLE_BLANK_ENABLE 0

#define USB_MODE_THREAD_STACK 4096
#define USB_MODE_THREAD_PRIO  9
#define USB_MODE_THREAD_TICK  20

#define USB_MODE_EVENT_INIT           (1U << 0)
#define USB_MODE_EVENT_POWER          (1U << 1)
#define USB_MODE_EVENT_CDC_FRAME      (1U << 2)
#define USB_MODE_EVENT_DISPLAY_FRAME  (1U << 3)
#define USB_MODE_EVENT_WAKE_LOGO_VISIBLE (1U << 4)
#define USB_MODE_EVENT_WAKE_LOGO_RELEASE (1U << 5)
#define USB_MODE_EVENT_CDC_TIMEOUT    (1U << 6)
#define USB_MODE_EVENT_CDC_REQUEST    (1U << 7)
#define USB_MODE_EVENT_DISPLAY_REQUEST (1U << 8)
#define USB_MODE_EVENT_DTR_CLOSED     (1U << 9)
#define USB_MODE_EVENT_STOP           (1U << 10)
#define USB_MODE_EVENT_STATIC_LOGO_RELEASED (1U << 12)
#define USB_MODE_EVENT_TRANSITION_RETRY (1U << 11)
#define USB_MODE_EVENT_OUTPUT_READY   (1U << 13)
#define USB_MODE_EVENT_DISPLAY_RECOVER (1U << 14)
#define USB_MODE_EVENT_SOURCE_ACTIVITY (1U << 16)
#define USB_MODE_EVENT_SOURCE_IDLE  (1U << 17)
#define USB_MODE_EVENT_HOST_RESET   (1U << 18)
#define USB_MODE_EVENT_RESTART_RELIGHT (1U << 20)
#define USB_MODE_EVENT_CDC_RESTART_RESUME (1U << 21)

#define USB_MODE_QUIESCE_TIMEOUT_MS   300
#define USB_MODE_RETRY_DELAY_MS       10
#define USB_MODE_OUTPUT_SETTLE_MS     20
/*
 * mpp_decoder_mjpeg_decode_is_busy() covers only decoder->ops->decode().
 * get/put-frame and decoder destruction run after that counter reaches zero
 * and still touch the shared VE/CMA allocator.  Require an idle interval
 * before handing the decoder to the other USB source, otherwise a wake-time
 * Display -> CDC switch can overlap decoder destruction and corrupt memheap.
 */
#define USB_MODE_MJPEG_IDLE_STABLE_MS 30
#define USB_MODE_DISPLAY_RECOVER_MS   1200
#define USB_MODE_DISPLAY_WAKE_RELOAD_MS 200
#define USB_MODE_WAKE_LOGO_HOLD_MS    1000
/* Keep the power-on PNG visible long enough to avoid a one-frame flash when
 * USB enumeration completes immediately after panel bring-up. */
#define USB_MODE_BOOT_LOGO_HOLD_MS    2000
/* Reject the transient/partial Display frames produced during cold boot and
 * USB enumeration.  This gate is active only while a Logo owns the panel. */
#define USB_MODE_LOGO_DISPLAY_STABLE_FRAMES 6
#define USB_MODE_LOGO_DISPLAY_STABLE_MS     300
/*
 * PC restart (reboot) recovery.  A reboot only resets the USB bus; unlike
 * sleep/hibernate it does not deliver the suspend -> resume pair which drives
 * the normal wake arbitration.  These two bounds make sure a restarted host
 * always ends up with a lit panel and a running CDC stream, even when one of
 * the wake hand-shake steps (Logo painted, first host frame) never happens.
 */
#define USB_MODE_RESTART_RELIGHT_MS   2000
#define USB_MODE_CDC_RESTART_RESUME_MS 1500

#define CDC_MODE_CMD_LEN 5
#define CDC_MODE_CMD_HEAD0 0xCC
#define CDC_MODE_CMD_HEAD1 0xDD
#define CDC_MODE_CMD_TAIL0 0xEE
#define CDC_MODE_CMD_TAIL1 0xFF
#define CDC_MODE_VALUE_CDC     0x00
#define CDC_MODE_VALUE_DISPLAY 0x01

#define CDC_BRIGHTNESS_CMD_LEN   5
#define CDC_BRIGHTNESS_HEAD0     0xAA
#define CDC_BRIGHTNESS_HEAD1     0xBB
#define CDC_BRIGHTNESS_TAIL0     0xCC
#define CDC_BRIGHTNESS_TAIL1     0xDD

#define CDC_SN_UPDATE_CMD_LEN    (USB_CDC_SN_PAYLOAD_LEN + 2)
#define CDC_SN_UPDATE_HEAD       0xA1
#define CDC_SN_UPDATE_TAIL       0xB2

#define CDC_OTA_CMD_LEN          3
#define CDC_OTA_HEAD0            0xAA
#define CDC_OTA_HEAD1            0xBB
#define CDC_OTA_HEAD2            0xCC

#define CDC_LOGO_CMD             "SAVELOGO"
#define CDC_LOGO_CMD_LEN         8
#define CDC_LOGO_TIMEOUT_MS      500
#define CDC_LOGO_PATH            "/data/lvgl_data/logo/logo.png"

static const uint8_t g_sn_req_sig[] = { 0xF0, 0xA5, 0x5A, 0x0F };

/* global descriptor */
static const uint8_t cdc_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0150, 0x01),
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, CDC_MAX_MPS, 0x02),
    USB_LANGID_INIT(USBD_LANGID_STRING),
    0x10, USB_DESCRIPTOR_TYPE_STRING,
    'H', 0x00, 'L', 0x00, ' ', 0x00, 'V', 0x00, 'M', 0x00, 'A', 0x00, 'X', 0x00,
    0x22, USB_DESCRIPTOR_TYPE_STRING,
    'H', 0x00, 'L', 0x00, '-', 0x00, 'V', 0x00, 'M', 0x00, 'A', 0x00, 'X', 0x00,
    ' ', 0x00, 'C', 0x00, 'D', 0x00, 'C', 0x00, ' ', 0x00, 'D', 0x00, 'E', 0x00, 'M', 0x00, 'O', 0x00,
    0x12, USB_DESCRIPTOR_TYPE_STRING,
    '2', 0x00, '0', 0x00, '2', 0x00, '6', 0x00, '0', 0x00, '6', 0x00, '3', 0x00, '0', 0x00,
#ifdef CONFIG_USB_HS
    0x0a,
    USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00,
    0x02,
    0x02,
    0x02,
    0x01,
    0x40,
    0x01,
    0x00,
#endif
    0x00
};

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t read_buffer[CDC_RX_BUF_SIZE];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t write_buffer[CDC_TX_BUF_SIZE];

volatile bool ep_tx_busy_flag = false;
volatile uint8_t dtr_enable = 0;
extern uint8_t usb_disp_get_host_power_state(void);

/*
 * The closed Display library updates this byte synchronously when handling
 * D0/D3, before its suspend worker prints or calls the optional OSD hook.
 * Static-Logo arbitration must use it to reject the old session's final
 * DTR/frame callbacks during that short hook-latency window.
 */
static bool usb_mode_display_power_is_on(void)
{
    return usb_disp_get_host_power_state() != 0;
}
volatile bool touch_data_enabled = false;
volatile bool s_in_display_mode = true;

static volatile bool s_cdc_stream_active = false;
extern void usb_display_enable(void);
extern void usb_display_disable(void);
/* The official Display enable/disable pair owns decoder allocations and is
 * not a harmless visibility toggle.  Keep it balanced and idempotent. */
static bool s_display_source_enabled = true;

static void usb_mode_display_enable_owned(void)
{
    if (!s_display_source_enabled) {
        usb_display_enable();
        s_display_source_enabled = true;
    }
}

static void usb_mode_display_disable_owned(void)
{
    if (s_display_source_enabled) {
        usb_display_disable();
        s_display_source_enabled = false;
    }
}
/* A manual Display command owns the mode until an explicit CDC command. */
static volatile bool s_display_cmd_latched = false;
static volatile bool s_auto_switch_enabled = true;
static volatile bool s_ota_active = false;
static volatile rt_tick_t s_last_cdc_tick = 0;
/* Last received CDC payload or completed Display frame. */
static volatile rt_tick_t s_last_source_data_tick = 0;
static volatile uint32_t s_cdc_activity_generation = 0;
static volatile uint32_t s_timeout_generation = 0;
static volatile uint32_t s_dtr_close_generation = 0;
static volatile uint32_t s_power_generation = 0;
static volatile uint32_t s_dtr_close_power_generation = 0;
/*
 * A Windows sleep wake must retain the CDC owner even if the host application
 * needs longer than the normal two-second stream timeout to resume.
 */
static volatile bool s_cdc_resume_hold = false;
/*
 * usb_disp_suspend_exit() powers the Display framebuffer back on even when
 * CDC remains the logical owner.  Re-apply CDC output once that callback has
 * completely finished, from the serialized USB mode thread.
 */
static volatile bool s_cdc_output_restore_pending = false;
/* Backlight was blanked solely because both host image sources went quiet. */
static volatile bool s_source_idle_backlight_off = false;
static volatile bool s_source_wake_pending = false;
static rt_timer_t s_display_timer = RT_NULL;
static rt_timer_t s_logo_save_timer = RT_NULL;
static bool s_decoder_started = false;

enum cdc_rx_message_type {
    CDC_RX_MESSAGE_PACKET = 0,
    CDC_RX_MESSAGE_ABORT_LOGO,
};

struct cdc_rx_message {
    uint32_t generation;
    uint32_t len;
    uint8_t type;
    uint8_t data[CDC_RX_BUF_SIZE];
};

static rt_mq_t s_cdc_rx_mq = RT_NULL;
static rt_thread_t s_cdc_rx_thread = RT_NULL;
static volatile bool s_cdc_rx_service_enabled = false;
static volatile bool s_cdc_out_read_armed = false;
static volatile uint32_t s_usb_session_generation = 1;
static volatile uint32_t s_cdc_rx_drop_generation = 0;
static struct cdc_rx_message s_cdc_rx_enqueue_message;
static struct cdc_rx_message s_cdc_rx_worker_message;

enum usb_runtime_mode {
    USB_RUNTIME_STATIC_LOGO = 0,
    USB_RUNTIME_DISPLAY,
    USB_RUNTIME_CDC,
    USB_RUNTIME_SLEEP,
};

enum usb_transition_target {
    USB_TARGET_NONE = 0,
    USB_TARGET_DISPLAY,
    USB_TARGET_CDC,
};

static volatile enum usb_runtime_mode s_runtime_mode = USB_RUNTIME_STATIC_LOGO;
static volatile enum usb_transition_target s_transition_target = USB_TARGET_NONE;
static volatile enum usb_transition_target s_resume_target = USB_TARGET_NONE;
/* Last explicit host command; independent from automatically detected frames. */
static volatile enum usb_transition_target s_command_target = USB_TARGET_NONE;
static volatile bool s_host_attached = false;
static volatile bool s_usb_bus_awake = false;
static volatile bool s_host_awake = false;
static volatile bool s_wake_replay_pending = false;
static volatile bool s_cdc_frame_pending = false;
static volatile bool s_display_frame_pending = false;
/*
 * On a real D3 -> D0 transition the local static PNG covers the retained host
 * framebuffer.  The overlay is released only after it has been visible for a
 * minimum interval and a fresh host JPEG has completed rendering behind it.
 */
static volatile bool s_wake_logo_armed = false;
static volatile bool s_wake_logo_active = false;
static volatile bool s_wake_logo_show_requested = false;
static volatile bool s_wake_logo_visible = false;
static volatile bool s_wake_logo_min_elapsed = false;
static volatile bool s_wake_logo_host_frame_ready = false;
static volatile bool s_wake_logo_release_pending = false;
static volatile unsigned int s_wake_logo_epoch = 0;
static volatile unsigned int s_wake_logo_visible_epoch = 0;
static volatile rt_tick_t s_wake_logo_hold_deadline = 0;
/* The boot/static Logo remains the owner until the first host frame is ready. */
static volatile bool s_static_logo_owned = false;
static volatile bool s_static_logo_visible = false;
static volatile bool s_static_logo_min_elapsed = false;
static volatile rt_tick_t s_static_logo_hold_deadline = 0;
/* Only the first power-on Logo is guaranteed for two seconds.  Re-plug and
 * wake sessions keep their already validated hand-off timing unchanged. */
static volatile bool s_first_power_logo_pending = true;
static volatile bool s_static_logo_force_hold = false;
static volatile bool s_static_logo_host_frame_ready = false;
/* Hard PWM gate: false from Logo start until a stable host frame commits. */
static volatile bool s_host_output_committed = false;
static volatile bool s_static_logo_release_pending = false;

bool usb_osd_logo_backlight_is_owned(void)
{
    bool owned;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    owned = (s_wake_logo_active && s_wake_logo_visible) ||
        (s_static_logo_owned && s_static_logo_visible &&
         s_runtime_mode == USB_RUNTIME_STATIC_LOGO);
    rt_hw_interrupt_enable(level);
    return owned;
}

/*
 * The image Logo uses the LVGL/UI framebuffer, which is also managed by the
 * generic USB OSD state machine.  Keep that state machine out of the
 * framebuffer for the complete Logo transaction, not merely after the PNG is
 * visible.  In particular this closes the POWERON -> LVGL timer window on a
 * SATA-powered board and the hide -> released callback window during the
 * final host-frame hand-off.
 */
bool usb_osd_static_logo_ui_owned(void)
{
    bool owned;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    owned = s_static_logo_owned || s_wake_logo_active ||
        s_static_logo_release_pending;
    rt_hw_interrupt_enable(level);
    return owned;
}

bool usb_osd_backlight_enable_allowed(void)
{
    bool allowed;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    /* A SATA-powered board keeps the old framebuffer alive while USB changes
     * state.  Once suspend/reset armed the wake Logo, the old host commit is
     * no longer permission to light PWM. */
    allowed = (!s_wake_logo_armed && !s_wake_logo_active &&
               s_host_output_committed) ||
        (s_wake_logo_active && s_wake_logo_visible) ||
        (s_static_logo_owned && s_static_logo_visible);
    rt_hw_interrupt_enable(level);
    return allowed;
}
static volatile uint32_t s_logo_display_stable_frames = 0;
static volatile rt_tick_t s_logo_display_first_tick = 0;
/* Keep the panel dark during host-source recovery until a JPEG is rendered. */
static volatile bool s_waiting_first_output = false;
/* Automatic Display->CDC keeps Display visible while the first captured CDC
 * JPEG is decoded into the disabled UI layer.  Reveal it only after the CDC
 * renderer reports pan-display/vsync, otherwise the resident PNG flashes. */
static volatile bool s_cdc_overlay_reveal_pending = false;
static volatile uint32_t s_display_frame_baseline = 0;
/*
 * The Display library can finish authentication after Windows resumes while
 * its EP1 receive state is still left at the pre-suspend transfer.  The same
 * stale state can remain when CDC owns the decoder across wake and Display is
 * selected afterwards.  Arm a delayed, cancelable library-level restart only
 * while a Display wake/transition has received no complete frame.
 */
static volatile bool s_display_wake_recovery_eligible = false;
static volatile bool s_display_wake_recovery_armed = false;
static volatile uint32_t s_display_wake_recovery_baseline = 0;
static volatile rt_tick_t s_display_wake_recovery_deadline = 0;
static volatile bool s_display_wake_reenum_armed = false;
static volatile uint32_t s_display_wake_reenum_baseline = 0;
static volatile rt_tick_t s_display_wake_reenum_deadline = 0;
/* Suppress normal reset handling during our short recovery reconnect. */
static volatile bool s_display_reenumeration_active = false;
static volatile bool s_host_reset_abort_logo_save_pending = false;
static volatile bool s_host_reset_blank_cdc_pending = false;
/*
 * Host restart (PC reboot) bookkeeping.
 *
 * s_host_restart_armed records that the arbiter has committed to a real host
 * session (Display or CDC).  A bus reset seen while it is armed means the PC
 * restarted: the normal wake path is not going to run because there is no
 * suspend -> resume pair, so the restart recovery below has to drive it.
 */
static volatile bool s_host_restart_armed = false;
/* Failsafe: relight the panel if the restart hand-off never completes. */
static volatile bool s_restart_relight_armed = false;
static volatile rt_tick_t s_restart_relight_deadline = 0;
/* Failsafe: re-prime the CDC pipeline if no frame is decoded after restart. */
static volatile bool s_cdc_restart_resume_armed = false;
static volatile rt_tick_t s_cdc_restart_resume_deadline = 0;
/* Bus reset without a preceding D3 suspend is a PC restart, not sleep/wake. */
static rt_event_t s_mode_event = RT_NULL;
static rt_thread_t s_mode_thread = RT_NULL;
static rt_sem_t s_mode_stop_sem = RT_NULL;
static volatile bool s_mode_service_enabled = false;

enum cdc_logo_save_state {
    CDC_LOGO_SAVE_IDLE = 0,
    CDC_LOGO_SAVE_RECEIVING,
};

static volatile enum cdc_logo_save_state s_logo_save_state = CDC_LOGO_SAVE_IDLE;
static int s_logo_fd = -1;

extern void cdc_image_rx(const uint8_t *data, uint32_t len);
extern int cdc_image_processor_init(void);
extern int cdc_image_decoder_start(void);
extern void cdc_image_processor_set_enabled(bool enabled);
extern void cdc_image_processor_set_output_enabled(bool enabled);
extern void cdc_image_processor_reset_rx(void);
extern void cdc_image_processor_reset_stream(void);
extern void cdc_image_processor_blank_output(void);
extern void cdc_image_processor_clear_frame(void);
extern void cdc_image_processor_ui_layer_enable(void);
extern bool cdc_image_processor_wait_idle(uint32_t timeout_ms);

extern void usb_display_enable(void);
extern void usb_display_disable(void);
extern uint32_t usb_display_recv_frame_count(void);
extern void usb_osd_static_logo_show(unsigned int epoch);
extern void usb_osd_static_logo_hide(unsigned int epoch);
extern void usb_osd_static_logo_release_cdc(unsigned int epoch);
extern void usb_osd_static_logo_released(unsigned int epoch);
extern void usbd_hid_touch_set_params(int params);
extern void aic_panel_backlight_enable(void);
extern void aic_panel_backlight_disable(void);
extern void touch_send_complete_callback(void);
extern void touch_bulk(void);
#ifdef AIC_OTA_INTERFACE
extern bool handle_ota_packet(uint8_t *data, uint32_t length);
#endif

extern void usbd_cdc_acm_bulk_out(uint8_t ep, uint32_t nbytes);
extern void usbd_cdc_acm_bulk_in(uint8_t ep, uint32_t nbytes);

/* endpoint call back */
struct usbd_endpoint cdc_out_ep = {
    .ep_addr = CDC_OUT_EP,
    .ep_cb = usbd_cdc_acm_bulk_out
};

struct usbd_endpoint cdc_in_ep = {
    .ep_addr = CDC_IN_EP,
    .ep_cb = usbd_cdc_acm_bulk_in
};

static uint8_t runtime_out_ep(void)
{
    return cdc_out_ep.ep_addr ? cdc_out_ep.ep_addr : CDC_OUT_EP;
}

uint8_t runtime_in_ep(void)
{
    return cdc_in_ep.ep_addr ? cdc_in_ep.ep_addr : CDC_IN_EP;
}

static void cdc_note_activity(void)
{
    s_last_cdc_tick = rt_tick_get();
}

static void usb_mode_post(uint32_t event);
static void logo_save_abort(void);
static void cdc_process_rx_packet(uint8_t *buf, uint32_t nbytes);

static void cdc_out_read_disarm(void)
{
    rt_base_t level = rt_hw_interrupt_disable();

    s_cdc_out_read_armed = false;
    rt_hw_interrupt_enable(level);
}

static void cdc_arm_out_read(void)
{
    bool was_armed;
    bool room;
    int ret;
    rt_base_t level;

    if (!s_cdc_rx_service_enabled || !s_usb_bus_awake ||
        !usb_device_is_configured())
        return;
    level = rt_hw_interrupt_disable();
    was_armed = s_cdc_out_read_armed;
    /* All re-arm paths (callback, worker, 100-ms retry) use the same
     * backpressure limit. The worker frees a slot before retrying. */
    room = !s_cdc_rx_mq ||
        s_cdc_rx_mq->entry < CDC_RX_QUEUE_DEPTH - 2;
    if (!was_armed && room) {
        s_cdc_out_read_armed = true;
    }
    rt_hw_interrupt_enable(level);
    if (was_armed || !room) {
        return;
    }

    ret = usbd_ep_start_read(runtime_out_ep(), read_buffer, CDC_RX_BUF_SIZE);
    if (ret != 0) {
        cdc_out_read_disarm();
        USB_LOG_WRN("CDC OUT arm failed: %d, retry later\r\n", ret);
    }
}

static bool cdc_rx_queue_message(enum cdc_rx_message_type type,
                                 const uint8_t *data, uint32_t len)
{
    if (!s_cdc_rx_service_enabled || !s_cdc_rx_mq ||
        len > CDC_RX_BUF_SIZE) {
        return false;
    }

    s_cdc_rx_enqueue_message.generation =
        __atomic_load_n(&s_usb_session_generation, __ATOMIC_ACQUIRE);
    s_cdc_rx_enqueue_message.len = len;
    s_cdc_rx_enqueue_message.type = (uint8_t)type;
    if (data && len > 0) {
        memcpy(s_cdc_rx_enqueue_message.data, data, len);
    }

    return rt_mq_send(s_cdc_rx_mq, &s_cdc_rx_enqueue_message,
                      sizeof(s_cdc_rx_enqueue_message)) == RT_EOK;
}

static void cdc_rx_thread_entry(void *parameter)
{
    uint32_t handled_drop_generation = 0;

    (void)parameter;
    while (1) {
        uint32_t current_generation;
        uint32_t drop_generation;

        if (rt_mq_recv(s_cdc_rx_mq, &s_cdc_rx_worker_message,
                       sizeof(s_cdc_rx_worker_message),
                       RT_WAITING_FOREVER) != RT_EOK)
            continue;

        current_generation =
            __atomic_load_n(&s_usb_session_generation, __ATOMIC_ACQUIRE);
        drop_generation =
            __atomic_load_n(&s_cdc_rx_drop_generation, __ATOMIC_ACQUIRE);
        if (drop_generation != handled_drop_generation) {
            handled_drop_generation = drop_generation;
            cdc_image_processor_reset_rx();
        }

        if (s_cdc_rx_worker_message.type == CDC_RX_MESSAGE_ABORT_LOGO) {
            logo_save_abort();
        } else if (s_cdc_rx_service_enabled &&
                   s_cdc_rx_worker_message.generation == current_generation &&
                   s_cdc_rx_worker_message.len > 0) {
            cdc_process_rx_packet(s_cdc_rx_worker_message.data,
                                  s_cdc_rx_worker_message.len);
        }
        cdc_arm_out_read();
    }
}

static int cdc_rx_service_init(void)
{
    if (s_cdc_rx_thread) {
        s_cdc_rx_service_enabled = true;
        return RT_EOK;
    }

    s_cdc_rx_mq = rt_mq_create("cdc_rx", sizeof(struct cdc_rx_message),
                               CDC_RX_QUEUE_DEPTH, RT_IPC_FLAG_FIFO);
    if (!s_cdc_rx_mq)
        return -RT_ENOMEM;
    s_cdc_rx_thread = rt_thread_create("cdc_rx", cdc_rx_thread_entry, RT_NULL,
                                       CDC_RX_THREAD_STACK,
                                       CDC_RX_THREAD_PRIO, 20);
    if (!s_cdc_rx_thread) {
        rt_mq_delete(s_cdc_rx_mq);
        s_cdc_rx_mq = RT_NULL;
        return -RT_ENOMEM;
    }
    s_cdc_rx_service_enabled = true;
    rt_thread_startup(s_cdc_rx_thread);
    return RT_EOK;
}

static void cdc_rx_drop_queued_packets(void)
{
    rt_base_t level;

    /*
     * A Display->CDC switch is triggered by a CDC frame which was assembled
     * while CDC output was disabled.  On a PC reboot path there can still be
     * old USB chunks queued behind that trigger frame.  Move to a new session
     * generation and flush the message queue so only packets received after the
     * CDC owner is re-armed can build the next JPEG for the hardware decoder.
     */
    level = rt_hw_interrupt_disable();
    __atomic_add_fetch(&s_usb_session_generation, 1, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&s_cdc_rx_drop_generation, 1, __ATOMIC_ACQ_REL);
    rt_hw_interrupt_enable(level);

    if (s_cdc_rx_mq) {
        rt_mq_control(s_cdc_rx_mq, RT_IPC_CMD_RESET, RT_NULL);
    }
}

/*
 * Keep this separate from cdc_note_activity(): arbitrary CDC chunks must wake
 * the panel, but only complete CDC JPEG frames may affect the existing
 * CDC-to-Display auto-switch timer.
 */
static void usb_mode_note_source_activity(void)
{
    bool wake = false;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    s_last_source_data_tick = rt_tick_get();
    if (s_source_idle_backlight_off) {
        s_source_idle_backlight_off = false;
        s_source_wake_pending = true;
        wake = true;
    }
    rt_hw_interrupt_enable(level);

    if (wake) {
        usb_mode_post(USB_MODE_EVENT_SOURCE_ACTIVITY);
    }
}

static uint32_t tick_to_ms(rt_tick_t tick)
{
    return (uint32_t)((tick * 1000) / RT_TICK_PER_SECOND);
}

static void usb_mode_post(uint32_t event)
{
    if (s_mode_event) {
        rt_event_send(s_mode_event, event);
    }
}

static void usb_mode_set_runtime(enum usb_runtime_mode mode)
{
    s_runtime_mode = mode;
    s_in_display_mode = (mode != USB_RUNTIME_CDC);
    if (mode != USB_RUNTIME_CDC) {
        s_cdc_output_restore_pending = false;
    }
}

static bool usb_mode_wake_logo_blocks_host(void)
{
    bool blocked;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    blocked = s_wake_logo_armed || s_wake_logo_active ||
        s_static_logo_owned || s_waiting_first_output ||
        s_runtime_mode == USB_RUNTIME_SLEEP;
    rt_hw_interrupt_enable(level);
    return blocked;
}

static void usb_mode_restore_saved_backlight(void)
{
    uint8_t brightness = usb_cdc_get_backlight();
    rt_base_t level;

    /*
     * Something is allowed to light the panel now (a real payload, or the
     * local Logo taking ownership), so the source-idle latch has done its job
     * of hiding the frame retained from the previous session.  Clearing it
     * here keeps every automatic transition from staying dark after a Logo
     * hand-off, and it is the single point which can still relight the panel.
     */
    level = rt_hw_interrupt_disable();
    s_source_idle_backlight_off = false;
    s_source_wake_pending = false;
    s_restart_relight_armed = false;
    rt_hw_interrupt_enable(level);

    /*
     * aic_panel_backlight_enable() only enables the PWM channel.  Display
     * suspend can lose its period/duty registers, which explains why sending
     * a brightness command after wake makes the panel light immediately.
     * Re-apply the saved level on every automatic wake instead of relying on
     * the pre-suspend PWM register contents.
     */
    aic_panel_backlight_enable();
    if (!usb_cdc_set_backlight(brightness, false)) {
        USB_LOG_WRN("USB mode: restore backlight %u%% failed\r\n",
                    brightness);
    }
}

static void usb_mode_enable_host_backlight(void)
{
    if (!usb_mode_wake_logo_blocks_host()) {
        usb_mode_restore_saved_backlight();
    }
}

static void usb_mode_disable_host_backlight(void)
{
    bool keep_logo_visible;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    keep_logo_visible =
        (s_wake_logo_active && s_wake_logo_visible) ||
        (s_static_logo_owned && s_static_logo_visible);
    rt_hw_interrupt_enable(level);

    if (!keep_logo_visible) {
        aic_panel_backlight_disable();
    }
}

/*
 * Backlight decision for every host-source (Display/CDC) transition.  A panel
 * blanked by the source-idle watchdog stays dark across a mode change; only a
 * real Display/CDC payload lights it again.  Without this the CDC auto-switch
 * timeout would relight the panel while no host image is being delivered.
 */
static void usb_mode_apply_host_backlight(void)
{
    if (s_source_idle_backlight_off) {
        usb_mode_disable_host_backlight();
    } else {
        usb_mode_enable_host_backlight();
    }
}

static void usb_mode_disable_sources(bool capture_cdc)
{
    /* Display disable tears down decoder state; do it only after VE is idle. */
    cdc_image_processor_set_output_enabled(false);
    usbd_hid_touch_set_params(0);
    s_cdc_stream_active = false;
    (void)capture_cdc;
}

static bool usb_mode_wait_mjpeg_idle(uint32_t timeout_ms);

static bool usb_mode_quiesce_mjpeg(bool capture_cdc, uint32_t timeout_ms)
{
    bool cdc_idle;
    bool mjpeg_idle;

    mpp_decoder_set_mjpeg_decode_enabled(0);
    usb_mode_disable_sources(capture_cdc);
    cdc_idle = cdc_image_processor_wait_idle(timeout_ms);
    mjpeg_idle = usb_mode_wait_mjpeg_idle(timeout_ms);

    if (cdc_idle && mjpeg_idle) {
        usb_mode_display_disable_owned();
        cdc_image_processor_set_enabled(capture_cdc);
    }

    return cdc_idle && mjpeg_idle;
}

/*
 * CDC is an overlay, not a replacement USB display device.  Keep the official
 * Display decoder, endpoint and video layer alive underneath it; only wait for
 * an older CDC transaction and then enable the UI overlay.  The MPP session
 * mutex serializes CDC frames with the Display decoder's keep-alive frames.
 */
static bool usb_mode_prepare_cdc_overlay(uint32_t timeout_ms)
{
    cdc_image_processor_set_output_enabled(false);
    usbd_hid_touch_set_params(0);
    s_cdc_stream_active = false;

    if (!cdc_image_processor_wait_idle(timeout_ms)) {
        return false;
    }

    usb_mode_display_enable_owned();
    cdc_image_processor_set_enabled(true);
    return true;
}

static void usb_mode_blank_stale_cdc_output(void)
{
    cdc_image_processor_set_output_enabled(false);
    if (!cdc_image_processor_wait_idle(USB_MODE_QUIESCE_TIMEOUT_MS)) {
        USB_LOG_WRN("USB mode: CDC decoder busy while blanking stale output\r\n");
    }
    cdc_image_processor_reset_stream();
    cdc_image_processor_blank_output();
}

/*
 * Restart the CDC pipeline for a brand new host session (PC restart / bus
 * reset) without taking the UI layer away.
 *
 * usb_mode_blank_stale_cdc_output() is the right tool when the host is really
 * gone: it also clears layer.enable so the retained image cannot be exposed.
 * After a restart the host is about to stream again, so the layer has to stay
 * (or be put back) on - otherwise every following JPEG is decoded into a
 * disabled layer and the panel stays frozen on the last visible frame until a
 * physical re-plug.
 */
static void usb_mode_reset_cdc_session_output(void)
{
    bool resume;
    rt_base_t level;

    cdc_image_processor_set_output_enabled(false);
    if (!cdc_image_processor_wait_idle(USB_MODE_QUIESCE_TIMEOUT_MS)) {
        USB_LOG_WRN("USB mode: CDC decoder busy during session reset\r\n");
    }
    /* Drop the JPEG half-frame and the complete frame of the old session. */
    cdc_image_processor_reset_stream();
    /* Wipe the retained pixels but keep the layer enabled. */
    cdc_image_processor_clear_frame();

    level = rt_hw_interrupt_disable();
    resume = s_host_attached &&
        s_runtime_mode == USB_RUNTIME_CDC &&
        s_resume_target == USB_TARGET_CDC;
    if (resume) {
        s_cdc_stream_active = true;
    }
    rt_hw_interrupt_enable(level);

    if (!resume) {
        return;
    }

    usb_mode_display_enable_owned();
    mpp_decoder_set_mjpeg_decode_enabled(1);
    cdc_image_processor_set_enabled(true);
    cdc_image_processor_set_output_enabled(true);
    cdc_image_processor_ui_layer_enable();
    usbd_hid_touch_set_params(0);
    USB_LOG_INFO("USB mode: CDC session restarted\r\n");
}

static void usb_mode_resume_cdc_output_if_current(void)
{
    bool resume;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    resume = s_host_attached &&
        s_runtime_mode == USB_RUNTIME_CDC &&
        s_resume_target == USB_TARGET_CDC;
    if (resume) {
        s_cdc_stream_active = true;
    }
    rt_hw_interrupt_enable(level);

    if (!resume) {
        return;
    }

    usb_mode_display_enable_owned();
    mpp_decoder_set_mjpeg_decode_enabled(1);
    cdc_image_processor_set_enabled(true);
    cdc_image_processor_set_output_enabled(true);
    cdc_image_processor_ui_layer_enable();
    usbd_hid_touch_set_params(0);
}

/*
 * USB suspend is not the same as a cable disconnect.  The Display library
 * arms its OUT endpoint at CONFIGURED and the Windows driver expects that
 * receive context to survive D3 -> D0.  In particular, usb_display_disable()
 * only disables the display layer/usb_display_en; it does not restart the
 * endpoint or ask the host to submit Display transfers again.
 *
 * Stop the JPEG producers and VE before sleep, but deliberately leave the
 * Display transport enabled.  This preserves the behavior of the original
 * V1.64 suspend path, which disables Display only for a real unplug.
 */
static bool usb_mode_quiesce_preserve_display(bool capture_cdc,
                                              uint32_t timeout_ms)
{
    bool cdc_idle;
    bool mjpeg_idle;

    mpp_decoder_set_mjpeg_decode_enabled(0);
    cdc_image_processor_set_output_enabled(false);
    usbd_hid_touch_set_params(0);
    s_cdc_stream_active = false;

    cdc_idle = cdc_image_processor_wait_idle(timeout_ms);
    mjpeg_idle = usb_mode_wait_mjpeg_idle(timeout_ms);
    if (cdc_idle && mjpeg_idle) {
        cdc_image_processor_set_enabled(capture_cdc);
    }

    return cdc_idle && mjpeg_idle;
}

static bool usb_mode_wait_mjpeg_idle(uint32_t timeout_ms)
{
    rt_tick_t deadline = rt_tick_get() + rt_tick_from_millisecond(timeout_ms);
    rt_tick_t idle_since = 0;

    for (;;) {
        if (mpp_decoder_mjpeg_decode_is_busy()) {
            idle_since = 0;
        } else if (idle_since == 0) {
            idle_since = rt_tick_get();
        } else if ((rt_int32_t)(rt_tick_get() - idle_since) >=
                   (rt_int32_t)rt_tick_from_millisecond(
                       USB_MODE_MJPEG_IDLE_STABLE_MS)) {
            /* Close the boundary race once more before changing ownership. */
            if (!mpp_decoder_mjpeg_decode_is_busy()) {
                return true;
            }
            idle_since = 0;
        }

        if ((rt_int32_t)(rt_tick_get() - deadline) >= 0) {
            return false;
        }
        rt_thread_mdelay(1);
    }
}

static void usb_mode_enter_static_logo(void)
{
    if (s_runtime_mode == USB_RUNTIME_STATIC_LOGO &&
        s_static_logo_owned) {
        /* Coalesce duplicate DISCONNECT/D3 notifications.  Repainting the
         * same LVGL buffer twice is visible as a blink on the large panel. */
        return;
    }

    if (!usb_mode_quiesce_mjpeg(false, 200)) {
        usb_mode_set_runtime(USB_RUNTIME_SLEEP);
        aic_panel_backlight_disable();
        USB_LOG_ERR("JPEG decoder busy; defer static logo\r\n");
        usb_mode_post(USB_MODE_EVENT_INIT);
        return;
    }

    s_transition_target = USB_TARGET_NONE;
    s_command_target = USB_TARGET_NONE;
    s_display_cmd_latched = false;
    s_waiting_first_output = false;
    s_cdc_overlay_reveal_pending = false;
    /* The PNG object is resident in V1.64, but every new local-logo session
     * must explicitly recommit its UI layer.  This covers first power-on,
     * SATA-powered USB removal, and the sleep/hibernate interval without ever
     * exposing the retained Display buffer. */
    s_static_logo_owned = true;
    s_static_logo_visible = false;
    s_static_logo_force_hold = s_first_power_logo_pending;
    s_static_logo_min_elapsed = !s_static_logo_force_hold;
    s_static_logo_host_frame_ready = false;
    s_static_logo_hold_deadline = 0;
    s_static_logo_release_pending = false;
    usb_mode_set_runtime(USB_RUNTIME_STATIC_LOGO);
    aic_panel_backlight_disable();
    usb_osd_static_logo_show(0);
    USB_LOG_INFO("USB mode: static logo\r\n");
}

/*
 * A configured host entering suspend is not the same as a cable unplug.
 * Stop CDC production once, discard the interrupted JPEG, and retain the
 * Display transport endpoint for D0/CONFIGURED wake.  In particular, never
 * enter/retry the local static-Logo path here: retrying that path while a CDC
 * decode is completing can livelock the mode thread and leave the last frame
 * plus backlight stuck on.
 */
static void usb_mode_enter_sleep(void)
{
    bool cdc_idle;

    if (s_runtime_mode == USB_RUNTIME_SLEEP) {
        aic_panel_backlight_disable();
        return;
    }

    /*
     * Never wait for the CDC/JPEG worker on the host power path.  A final CDC
     * packet can leave VE inside a decode while Windows freezes USB scheduling;
     * waiting for idle here prevents the mode thread from committing sleep and
     * later prevents it from processing wake.  Invalidate queued data and
     * close CDC output immediately; the Display transport remains untouched.
     */
    mpp_decoder_set_mjpeg_decode_enabled(0);
    cdc_rx_drop_queued_packets();
    cdc_image_processor_set_output_enabled(false);
    cdc_image_processor_reset_stream();
    cdc_idle = cdc_image_processor_wait_idle(USB_MODE_QUIESCE_TIMEOUT_MS);
    cdc_image_processor_set_enabled(false);
    s_cdc_stream_active = false;

    /*
     * V1.64 keeps the UI framebuffer and layer alive across D3.  Do the same
     * here: output and VE are already stopped above and PWM is switched off
     * below, so the retained CDC pixels cannot be exposed.  Destroying the UI
     * buffer with blank_output() also destroys the resident static PNG and is
     * why the later SHOW sequence produced backlight with no picture.
     */
    if (!cdc_idle) {
        USB_LOG_WRN("USB mode: CDC frame still retiring at suspend\r\n");
    }

    /*
     * Use the proven Display wake path for every computer suspend.  A CDC
     * session must not retain its last decoded frame/owner across D3: Windows
     * may close DTR and resume the Display interface before the CDC sender is
     * ready, which previously left the panel dark or restored stale output.
     * A new complete CDC frame will auto-select CDC again after wake.
     */
    s_resume_target = USB_TARGET_DISPLAY;
    s_cdc_resume_hold = false;
    s_cdc_output_restore_pending = false;
    s_cdc_frame_pending = false;

    s_transition_target = USB_TARGET_NONE;
    s_command_target = USB_TARGET_NONE;
    s_display_cmd_latched = false;
    s_waiting_first_output = false;
    s_cdc_overlay_reveal_pending = false;
    s_static_logo_release_pending = false;
    s_host_output_committed = false;
    usb_mode_set_runtime(USB_RUNTIME_SLEEP);
    aic_panel_backlight_disable();
    USB_LOG_INFO("USB mode: host sleep (Display path)\r\n");
}

static void usb_mode_enter_display(void)
{
    bool from_static_logo = s_runtime_mode == USB_RUNTIME_STATIC_LOGO;
    bool capture_cdc = !s_display_cmd_latched;
    bool from_cdc = s_runtime_mode == USB_RUNTIME_CDC;
    bool explicit_display =
        s_command_target == USB_TARGET_DISPLAY && s_display_cmd_latched;
    bool resume_with_display_armed =
        s_resume_target == USB_TARGET_DISPLAY &&
        (s_runtime_mode == USB_RUNTIME_SLEEP ||
         s_display_wake_recovery_armed);
    uint32_t display_count;
    rt_base_t level;

    s_transition_target = USB_TARGET_DISPLAY;
    /*
     * A manual Display command is latched just like the original implementation:
     * discard/reset CDC input until a CDC command arrives.  Automatic Display
     * mode keeps CDC capture enabled so a new stream can still auto-switch.
     */
    if (!((resume_with_display_armed || from_cdc) ?
          usb_mode_quiesce_preserve_display(capture_cdc,
                                            USB_MODE_QUIESCE_TIMEOUT_MS) :
          usb_mode_quiesce_mjpeg(capture_cdc,
                                 USB_MODE_QUIESCE_TIMEOUT_MS))) {
        USB_LOG_WRN("JPEG decoder busy; retry Display transition\r\n");
        rt_thread_mdelay(USB_MODE_RETRY_DELAY_MS);
        usb_mode_post(USB_MODE_EVENT_TRANSITION_RETRY);
        return;
    }

    if (from_cdc) {
        /* Reveal the continuously running Display video layer underneath. */
        cdc_image_processor_blank_output();
    }

    /* Capture the counter before reception is enabled; otherwise a very fast
     * first frame can become the new baseline and the resume watchdog misses
     * the only frame which would unblank the panel. */
    display_count = usb_display_recv_frame_count();
    level = rt_hw_interrupt_disable();
    s_cdc_stream_active = false;
    s_cdc_overlay_reveal_pending = false;
    if (!capture_cdc) {
        s_cdc_frame_pending = false;
    }
    s_display_frame_pending = false;
    s_display_frame_baseline = display_count;
    s_transition_target = USB_TARGET_NONE;
    s_resume_target = USB_TARGET_DISPLAY;
    s_cdc_resume_hold = false;
    if (explicit_display && !s_static_logo_owned && !s_wake_logo_active) {
        /*
         * Outside a Logo hand-off, preserve the established manual Display
         * behaviour.  While a Logo owns the panel, a mode command proves only
         * host intent, not that a valid scanout frame exists; it must never
         * bypass the stable-frame gate.
         */
        s_waiting_first_output = false;
        s_source_idle_backlight_off = false;
        s_source_wake_pending = false;
    }
    if ((from_static_logo && !s_wake_logo_active) || s_wake_logo_active) {
        s_waiting_first_output = true;
    }
    if (from_cdc && s_host_attached && s_host_awake && !explicit_display) {
        /*
         * Display was disabled while CDC owned the shared MJPEG decoder.  A
         * Windows wake can leave the Display driver without another full
         * frame to submit when the user switches modes.  Require a fresh frame
         * and reuse the same transfer-restart/re-enumeration watchdog used by
         * a Display-mode wake.
         */
        s_wake_logo_host_frame_ready = false;
        s_waiting_first_output = true;
        s_display_wake_recovery_eligible = false;
        s_display_wake_recovery_baseline = display_count;
        s_display_wake_recovery_deadline =
            rt_tick_get() +
            rt_tick_from_millisecond(USB_MODE_DISPLAY_RECOVER_MS);
        s_display_wake_recovery_armed = true;
        s_display_wake_reenum_armed = false;
    }
    /* A Display command during Logo ownership is not frame-ready evidence.
     * mpp_decoder_mjpeg_frame_ready() releases the Logo only after the stable
     * frame window has completed. */
    usb_mode_set_runtime(USB_RUNTIME_DISPLAY);
    /* A committed host session; the next bus reset means the PC restarted. */
    s_host_restart_armed = true;
    rt_hw_interrupt_enable(level);

    mpp_decoder_set_mjpeg_decode_enabled(1);
    usb_mode_display_enable_owned();
    usbd_hid_touch_set_params(1);

    if (s_waiting_first_output) {
        usb_mode_disable_host_backlight();
    } else {
        usb_mode_apply_host_backlight();
    }
    if (from_cdc) {
        USB_LOG_INFO("USB mode: wait for first Display frame after CDC\r\n");
    }
    USB_LOG_INFO("USB mode: display\r\n");
}

static void usb_mode_enter_cdc(void)
{
    bool from_static_logo = s_runtime_mode == USB_RUNTIME_STATIC_LOGO;
    bool from_display = s_runtime_mode == USB_RUNTIME_DISPLAY;
    bool automatic_from_display = from_display &&
        s_command_target != USB_TARGET_CDC;
    bool reset_resumed_stream;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    reset_resumed_stream = s_cdc_resume_hold &&
        s_resume_target == USB_TARGET_CDC;
    rt_hw_interrupt_enable(level);

    s_transition_target = USB_TARGET_CDC;
    if (!usb_mode_prepare_cdc_overlay(USB_MODE_QUIESCE_TIMEOUT_MS)) {
        USB_LOG_WRN("CDC decoder busy; retry overlay transition\r\n");
        rt_thread_mdelay(USB_MODE_RETRY_DELAY_MS);
        usb_mode_post(USB_MODE_EVENT_TRANSITION_RETRY);
        return;
    }

    if (automatic_from_display) {
        /* The complete JPEG which triggered automatic switching is already in
         * s_ready_idx.  Keep it, hide the resident PNG/UI buffer, and decode
         * that frame immediately behind the still-visible Display layer. */
        cdc_image_processor_blank_output();
        s_cdc_overlay_reveal_pending = true;
        USB_LOG_INFO("USB mode: seamless Display -> CDC\r\n");
    } else if (reset_resumed_stream || from_display) {
        if (from_display) {
            cdc_rx_drop_queued_packets();
            USB_LOG_INFO("USB mode: drop Display-captured CDC frame\r\n");
        }
        cdc_image_processor_reset_stream();
    }
    mpp_decoder_set_mjpeg_decode_enabled(1);
    if (!automatic_from_display)
        cdc_image_processor_ui_layer_enable();
    cdc_image_processor_set_output_enabled(true);

    level = rt_hw_interrupt_disable();
    s_cdc_stream_active = true;
    s_cdc_frame_pending = false;
    s_display_frame_pending = false;
    s_display_wake_recovery_eligible = false;
    s_display_wake_recovery_armed = false;
    s_display_wake_reenum_armed = false;
    if (from_static_logo && !s_wake_logo_active) {
        s_waiting_first_output = true;
    }
    s_transition_target = USB_TARGET_NONE;
    s_resume_target = USB_TARGET_CDC;
    usb_mode_set_runtime(USB_RUNTIME_CDC);
    /* A committed host session; the next bus reset means the PC restarted. */
    s_host_restart_armed = true;
    s_last_cdc_tick = rt_tick_get();
    rt_hw_interrupt_enable(level);

    /* After a manual queue flush, or an automatic switch with no queued RX
     * work, do not wait for the worker or its 100-ms timer to re-arm OUT. */
    cdc_arm_out_read();
    if (!s_waiting_first_output) {
        usb_mode_apply_host_backlight();
    }
    USB_LOG_INFO("USB mode: CDC\r\n");
}

static void usb_mode_restore_cdc_output_after_wake(void)
{
    bool reset_resumed_stream = false;
    bool restore;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    restore = s_cdc_output_restore_pending &&
        s_host_attached && s_host_awake &&
        s_runtime_mode == USB_RUNTIME_CDC &&
        s_resume_target == USB_TARGET_CDC;
    if (restore) {
        s_cdc_output_restore_pending = false;
        s_cdc_stream_active = true;
        reset_resumed_stream = s_cdc_resume_hold;
    }
    rt_hw_interrupt_enable(level);

    if (!restore) {
        return;
    }

    /*
     * The suspend-exit UI path has just made the Display layer visible again.
     * Re-assert the same output ownership used by a normal CDC transition and
     * discard only the JPEG assembly interrupted by suspend.  USB endpoints
     * remain intact, so the host CDC application does not need a mode toggle.
     */
    cdc_image_processor_set_output_enabled(false);
    if (!cdc_image_processor_wait_idle(USB_MODE_QUIESCE_TIMEOUT_MS)) {
        USB_LOG_WRN("USB mode: CDC decoder busy during wake restore\r\n");
    }
    /*
     * re-enabling output below decodes s_ready_idx again when a complete
     * frame is still buffered.  After a re-plug that buffer holds the image
     * from before the cable was pulled, which is exactly the stale frame the
     * panel must not show.  Drop it together with the interrupted JPEG.
     */
    if (reset_resumed_stream || s_source_idle_backlight_off) {
        /*
         * Drop the retained frame and its pixels, but keep the UI layer
         * enabled: the host is streaming again and every following JPEG must
         * be visible.  blank_output() would disable the layer here and freeze
         * the panel on the last visible image (the first frame after a PC
         * restart) until the cable is pulled.
         */
        cdc_image_processor_reset_stream();
        cdc_image_processor_clear_frame();
    }
    usb_mode_display_enable_owned();
    mpp_decoder_set_mjpeg_decode_enabled(1);
    cdc_image_processor_set_enabled(true);
    cdc_image_processor_set_output_enabled(true);
    cdc_image_processor_ui_layer_enable();
    usbd_hid_touch_set_params(0);
    /*
     * Stay dark while the idle latch is held: the panel may only come back on
     * the first frame which the host actually delivers in this session.
     */
    usb_mode_apply_host_backlight();
    USB_LOG_INFO("USB mode: CDC output restored after wake\r\n");
}

static enum usb_transition_target usb_mode_pending_target(void)
{
    rt_base_t level;
    enum usb_transition_target target = USB_TARGET_NONE;

    level = rt_hw_interrupt_disable();
    if (!s_display_cmd_latched && s_cdc_frame_pending) {
        target = USB_TARGET_CDC;
    } else if (s_display_frame_pending) {
        target = USB_TARGET_DISPLAY;
    }
    rt_hw_interrupt_enable(level);
    return target;
}

static void usb_mode_activate_target(enum usb_transition_target target)
{
    if (target == USB_TARGET_NONE) {
        target = USB_TARGET_DISPLAY;
    }

    if (target == USB_TARGET_CDC) {
        usb_mode_enter_cdc();
    } else {
        usb_mode_enter_display();
    }
}

static void usb_mode_handle_source_event(uint32_t events)
{
    bool explicit_request =
        (events & (USB_MODE_EVENT_CDC_REQUEST |
                   USB_MODE_EVENT_DISPLAY_REQUEST)) != 0;
    enum usb_transition_target target = explicit_request ?
        s_command_target : usb_mode_pending_target();

    switch (s_runtime_mode) {
        case USB_RUNTIME_STATIC_LOGO:
        case USB_RUNTIME_SLEEP:
            if (!explicit_request && s_cdc_resume_hold &&
                s_resume_target == USB_TARGET_CDC) {
                /*
                 * Display traffic resumes earlier than the CDC application on
                 * Windows.  Preserve the pre-sleep CDC owner until a fresh CDC
                 * frame or an explicit Display command makes the decision.
                 */
                target = USB_TARGET_CDC;
            }
            if (s_host_awake && target != USB_TARGET_NONE) {
                usb_mode_activate_target(target);
            }
            break;
        case USB_RUNTIME_DISPLAY:
            if (target == USB_TARGET_CDC) {
                usb_mode_enter_cdc();
            } else if (target == USB_TARGET_DISPLAY) {
                if (explicit_request) {
                    /*
                     * Re-apply an idempotent manual Display command so CDC's
                     * partial/ready frame buffers are flushed and the Display
                     * producer receives a clean disable/enable edge.
                     */
                    usb_mode_enter_display();
                } else {
                    /* Cancel a stale CDC retry on renewed Display traffic. */
                    s_transition_target = USB_TARGET_NONE;
                }
            }
            break;
        case USB_RUNTIME_CDC:
            /*
             * Display traffic commonly resumes before CDC after system wake.
             * It must not steal an active CDC session; timeout, DTR close, or
             * an explicit Display command performs that transition.
             */
            if (explicit_request && target == USB_TARGET_DISPLAY) {
                usb_mode_enter_display();
            } else if (target == USB_TARGET_CDC) {
                /* Cancel a stale Display retry after renewed CDC activity. */
                s_transition_target = USB_TARGET_NONE;
            }
            break;
        default:
            break;
    }
}

static void usb_mode_update_effective_power_locked(uint32_t display_count)
{
    bool was_awake = s_host_awake;
    bool awake = s_host_attached && s_usb_bus_awake;

    s_host_awake = awake;
    if (awake != was_awake) {
        s_power_generation++;
    }
    if (!awake && was_awake) {
        /* Close the backlight gate synchronously.  On a dual-supply board the
         * mode thread may run after AICFB_POWERON on the following resume;
         * leaving this true exposes the retained Display frame in between. */
        s_host_output_committed = false;
        s_cdc_resume_hold = false;
        s_cdc_output_restore_pending = false;
        /* Arm one Logo for every real awake->off edge, including a data-cable
         * removal while SATA keeps the board alive.  It is consumed only when
         * USB/D0 returns; the off interval itself remains dark. */
        s_wake_logo_armed = true;
        s_wake_logo_active = false;
        s_wake_logo_show_requested = false;
        s_wake_logo_visible = false;
        s_wake_logo_min_elapsed = false;
        s_wake_logo_host_frame_ready = false;
        s_wake_logo_release_pending = false;
        s_wake_logo_hold_deadline = 0;
        if (!s_host_attached) {
            s_resume_target = USB_TARGET_NONE;
        } else if (s_display_cmd_latched) {
            /* An explicit Display command received just before suspend wins. */
            s_resume_target = USB_TARGET_DISPLAY;
        } else if (s_runtime_mode == USB_RUNTIME_CDC ||
                   s_transition_target == USB_TARGET_CDC ||
                   s_cdc_frame_pending) {
            /*
             * DTR commonly drops before USB suspend.  Preserve the actual
             * rendering owner so that a later stale DTR-close event cannot
             * turn a sleeping CDC session into Display mode.
             */
            s_resume_target = USB_TARGET_CDC;
        } else {
            s_resume_target = USB_TARGET_DISPLAY;
        }
        /* Match the original behavior: manual Display latch ends at sleep. */
        s_display_cmd_latched = false;
    }
    if (awake && !was_awake) {
        bool start_wake_logo = s_wake_logo_armed ||
            s_runtime_mode == USB_RUNTIME_SLEEP;

        /* A wake starts a fresh arbitration cycle; old host frames are stale. */
        s_cdc_frame_pending = false;
        s_display_frame_pending = false;
        s_display_frame_baseline = display_count;
        s_last_source_data_tick = rt_tick_get();
        /*
         * s_source_idle_backlight_off deliberately survives the wake
         * transition.  Re-plugging the data cable asserts the bus and wakes
         * the host long before the PC application delivers its first frame.
         * Clearing the latch here let usb_mode_restore_cdc_output_after_wake()
         * light the panel on the frame which was retained from before the
         * unplug.  Only a real Display/CDC payload, or the local Logo taking
         * ownership of the panel, may clear it from now on.
         */
        s_source_wake_pending = false;
        s_wake_replay_pending = true;
        /*
         * Rebuild the Logo session from the durable runtime boundary, not
         * only from wake_logo_armed.  USB RESET/DISCONNECT callbacks are
         * allowed to cancel an in-flight request while the board stays alive
         * on SATA; runtime=SLEEP survives that ordering and guarantees the
         * following D0/CONFIGURED edge creates a new epoch.
         */
        if (start_wake_logo) {
            s_wake_logo_armed = false;
            s_wake_logo_active = true;
            s_static_logo_owned = false;
            s_static_logo_visible = false;
            s_wake_logo_show_requested = false;
            s_wake_logo_visible = false;
            s_wake_logo_min_elapsed = false;
            s_wake_logo_host_frame_ready = false;
            s_wake_logo_release_pending = false;
            s_wake_logo_hold_deadline = 0;
            s_wake_logo_epoch++;
            if (s_wake_logo_epoch == 0) {
                s_wake_logo_epoch++;
            }
            /*
             * Keep the fresh host frame detection armed while LVGL draws the
             * static logo over the host framebuffer.
             */
            s_waiting_first_output = true;
        }
        if (s_resume_target == USB_TARGET_CDC) {
            /*
             * Do not reinterpret the host application's delayed resume as a
             * normal CDC stream timeout.  A fresh frame or an explicit mode
             * decision below releases this hold.
             */
            s_cdc_resume_hold = true;
            s_cdc_output_restore_pending =
                s_runtime_mode == USB_RUNTIME_CDC;
        }
    }
}

/*
 * Match the CartoonLogo project's host-restart timing without starting its
 * local video player.  A real PC resume/restart resets the USB bus before
 * CONFIGURED/D0; submit the static PNG at that reset so its first LVGL refresh
 * can light the panel while the PC screen is coming on.
 */
static void usb_mode_start_reset_static_logo(void)
{
    bool logo_armed = false;
    bool had_cdc_output = false;
    unsigned int logo_epoch = 0;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    /*
     * s_wake_logo_armed covers sleep/hibernate (the arbiter saw the suspend
     * edge).  A PC restart resets the bus without that edge when VBUS bounces
     * or when the suspend was already consumed, which used to leave the panel
     * dark and the CDC pipeline frozen until a re-plug.  s_host_restart_armed
     * is set as soon as the arbiter commits to a real host session, so every
     * bus reset of a running session now starts the same recovery.
     */
    if (s_host_attached && !s_host_awake &&
        (s_wake_logo_armed || s_host_restart_armed) &&
        !s_display_reenumeration_active) {
        had_cdc_output =
            s_runtime_mode == USB_RUNTIME_CDC ||
            s_resume_target == USB_TARGET_CDC ||
            s_cdc_stream_active ||
            s_cdc_frame_pending;
        s_wake_logo_armed = false;
        s_host_restart_armed = false;
        s_wake_logo_active = true;
        /*
         * The LVGL request powers the framebuffer before repainting the PNG.
         * Submit it at RESET through the V1.64 static-Logo path, so a
         * later Display D0/CONFIGURED callback can never expose the retained
         * host framebuffer first.
         */
        s_wake_logo_show_requested = true;
        s_wake_logo_visible = false;
        s_wake_logo_min_elapsed = false;
        s_wake_logo_host_frame_ready = false;
        s_wake_logo_release_pending = false;
        s_wake_logo_hold_deadline = 0;
        s_wake_logo_epoch++;
        if (s_wake_logo_epoch == 0) {
            s_wake_logo_epoch++;
        }
        logo_epoch = s_wake_logo_epoch;
        s_waiting_first_output = true;
        s_host_reset_blank_cdc_pending = had_cdc_output;
        /*
         * Arm the restart failsafes.  The Logo hand-off needs the UI to paint
         * the PNG and a fresh host frame; on a PC restart either can be
         * missing, and the panel would stay dark for good.  The deadlines are
         * re-based in the timer until the host is configured again.
         */
        s_restart_relight_armed = true;
        s_restart_relight_deadline =
            rt_tick_get() +
            rt_tick_from_millisecond(USB_MODE_RESTART_RELIGHT_MS);
        s_cdc_restart_resume_armed = had_cdc_output;
        s_cdc_restart_resume_deadline =
            rt_tick_get() +
            rt_tick_from_millisecond(USB_MODE_CDC_RESTART_RESUME_MS);
        logo_armed = true;
    }
    rt_hw_interrupt_enable(level);

    if (logo_armed) {
        /* Never expose the retained pre-hibernate Display frame. */
        aic_panel_backlight_disable();
        usb_osd_static_logo_show(logo_epoch);
        usb_mode_post(USB_MODE_EVENT_HOST_RESET);
        USB_LOG_INFO("USB mode: host reset -> repaint static logo\r\n");
    }
}

static void usb_mode_usb_power_changed(bool attached, bool bus_awake)
{
    uint32_t display_count = usb_display_recv_frame_count();
    bool cdc_hold_before;
    bool cdc_hold_started;
    bool display_wake_recovery_started = false;
    bool request_wake_logo = false;
    unsigned int wake_logo_epoch = 0;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    cdc_hold_before = s_cdc_resume_hold;
    s_host_attached = attached;
    s_usb_bus_awake = attached && bus_awake;
    if (!attached) {
        s_command_target = USB_TARGET_NONE;
        s_display_cmd_latched = false;
        /* The host is really gone; a restart recovery is no longer pending. */
        s_host_restart_armed = false;
        s_restart_relight_armed = false;
        s_cdc_restart_resume_armed = false;
        s_cdc_frame_pending = false;
        s_display_frame_pending = false;
        s_wake_replay_pending = false;
        s_resume_target = USB_TARGET_NONE;
        s_display_wake_recovery_eligible = false;
        s_display_wake_recovery_armed = false;
        s_display_wake_reenum_armed = false;
        s_display_reenumeration_active = false;
        s_cdc_resume_hold = false;
        s_cdc_output_restore_pending = false;
        /* Keep the awake->disconnect wake-Logo arm set by
         * usb_mode_update_effective_power_locked(). */
        s_wake_logo_active = false;
        s_wake_logo_show_requested = false;
        s_wake_logo_visible = false;
        s_wake_logo_min_elapsed = false;
        s_wake_logo_host_frame_ready = false;
        s_wake_logo_release_pending = false;
        s_wake_logo_hold_deadline = 0;
    }
    usb_mode_update_effective_power_locked(display_count);
    /* Keep CONFIGURED-only wake behaviour identical to the video-Logo build. */
    if (attached && bus_awake && s_host_awake &&
        s_display_wake_recovery_eligible &&
        s_resume_target == USB_TARGET_DISPLAY &&
        s_runtime_mode != USB_RUNTIME_CDC) {
        s_display_wake_recovery_baseline = display_count;
        s_display_wake_recovery_deadline =
            rt_tick_get() +
            rt_tick_from_millisecond(USB_MODE_DISPLAY_WAKE_RELOAD_MS);
        s_display_wake_recovery_armed = true;
        s_display_wake_reenum_armed = false;
        s_display_wake_recovery_eligible = false;
        display_wake_recovery_started = true;
    }
    if (attached && bus_awake && s_wake_logo_active &&
        !s_wake_logo_show_requested) {
        s_wake_logo_show_requested = true;
        wake_logo_epoch = s_wake_logo_epoch;
        request_wake_logo = true;
    }
    cdc_hold_started = !cdc_hold_before && s_cdc_resume_hold;
    rt_hw_interrupt_enable(level);

    if (request_wake_logo) {
        /*
         * The LVGL handler performs framebuffer POWERON, repaint, and only then
         * reports visibility so the backlight can be restored safely.
         */
        aic_panel_backlight_disable();
        usb_osd_static_logo_show(wake_logo_epoch);
        USB_LOG_INFO("USB mode: configured -> repaint static logo\r\n");
    }
    if (cdc_hold_started) {
        USB_LOG_INFO("USB mode: retain CDC mode after wake\r\n");
    }
    if (display_wake_recovery_started) {
        USB_LOG_INFO("USB mode: arm Display recovery from USB resume\r\n");
    }
    usb_mode_post(USB_MODE_EVENT_POWER);
}

/*
 * Keep the active producer and the Display EP1 receive context intact across
 * computer sleep, as the official D215 demo does.  Suspend bookkeeping is
 * still updated so stale DTR/timeout events are rejected, but no mode POWER
 * event is posted on entry and therefore no decoder/source is torn down.
 */
static void usb_mode_note_suspend_state(bool awake)
{
    uint32_t display_count = usb_display_recv_frame_count();
    bool was_awake;
    bool cdc_hold_before;
    bool cdc_hold_started;
    bool notify_wake = false;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    was_awake = s_host_awake;
    cdc_hold_before = s_cdc_resume_hold;
    s_host_attached = true;
    s_usb_bus_awake = awake;
    usb_mode_update_effective_power_locked(display_count);
    if (!awake) {
        s_display_wake_recovery_armed = false;
        s_display_wake_reenum_armed = false;
        if (s_display_reenumeration_active) {
            /* usb_dc_deinit() reports suspend during the recovery reconnect. */
            s_display_wake_recovery_eligible = false;
        } else if (was_awake) {
            s_display_wake_recovery_eligible =
                s_resume_target == USB_TARGET_DISPLAY;
        }
    }
    if (awake) {
        /*
         * This callback can be generated by the authentication thread's
         * EXIT_SUSPEND_EVENT even when no host power-state 0x4 request exists.
         * Do not replay/re-enter the current mode; only reopen arbitration.
         */
        s_wake_replay_pending = false;
        s_last_cdc_tick = rt_tick_get();
        /*
         * D0 and authentication can each emit EXIT_SUSPEND_EVENT for the same
         * wake.  Every callback powers the Display framebuffer on, so every
         * completed callback must give a retained CDC owner the last word too.
         */
        if (s_runtime_mode == USB_RUNTIME_CDC &&
            s_resume_target == USB_TARGET_CDC) {
            s_cdc_output_restore_pending = true;
        }
        if (s_display_wake_recovery_eligible &&
            s_resume_target == USB_TARGET_DISPLAY &&
            s_runtime_mode != USB_RUNTIME_CDC) {
            s_display_wake_recovery_baseline = display_count;
            s_display_wake_recovery_deadline =
                rt_tick_get() +
                rt_tick_from_millisecond(
                    USB_MODE_DISPLAY_WAKE_RELOAD_MS);
            s_display_wake_recovery_armed = true;
            s_display_wake_reenum_armed = false;
        }
        s_display_wake_recovery_eligible = false;
        notify_wake = true;
    }
    cdc_hold_started = !cdc_hold_before && s_cdc_resume_hold;
    rt_hw_interrupt_enable(level);

    if (cdc_hold_started) {
        USB_LOG_INFO("USB mode: retain CDC mode after wake\r\n");
    }
    if (notify_wake) {
        usb_mode_post(USB_MODE_EVENT_POWER);
    }
}

void usb_osd_host_power_changed(bool awake)
{
    bool wait_for_display = false;
    bool wake_logo_active = false;
    bool wake_logo_visible = false;
    bool request_wake_logo = false;
    bool keep_visible_logo;
    bool first_wake;
    unsigned int wake_logo_epoch = 0;
    rt_base_t level;

    USB_LOG_INFO("USB mode: Display suspend %s\r\n",
                 awake ? "exit" : "enter");
    first_wake = awake && !s_host_awake;
    if (first_wake)
        aic_panel_backlight_disable();
    keep_visible_logo = usb_osd_logo_backlight_is_owned();
    /*
     * A real Display D3 must always close the host-awake gate, even when the
     * static Logo currently owns the UI layer.  Returning here used to leave
     * s_host_awake=true: the final CDC packet/DTR-close from the old session
     * could then switch Display -> CDC and release the Logo while the PC was
     * still asleep.  usb_mode_note_suspend_state(false) preserves/arms the
     * Logo session; it does not expose the retained host framebuffer.
     */
    usb_mode_note_suspend_state(awake);
    if (awake) {
        /*
         * A retained Display session may have a stale receive context after
         * wake.  Keep the panel dark until either a fresh frame arrives or the
         * short recovery timer performs the same clean reload as a Display
         * mode command.
         */
        level = rt_hw_interrupt_disable();
        /*
         * A decoded host frame is stronger evidence than a late duplicate D0
         * callback.  The latter can arrive after the wake logo was released;
         * never let its stale recovery state blank the already valid Display
         * output.
         */
        if (s_wake_logo_host_frame_ready) {
            s_display_wake_recovery_eligible = false;
            s_display_wake_recovery_armed = false;
            s_display_wake_reenum_armed = false;
        }
        wait_for_display = s_display_wake_recovery_armed &&
            s_host_awake &&
            s_runtime_mode == USB_RUNTIME_DISPLAY &&
            s_resume_target == USB_TARGET_DISPLAY;
        if (wait_for_display) {
            s_waiting_first_output = true;
        }
        wake_logo_active = s_wake_logo_active;
        if (wake_logo_active && (first_wake || !keep_visible_logo)) {
            /*
             * This callback runs only after usb_disp_suspend_exit() has issued
             * AICFB_POWERON.  Always repaint the PNG now, even if RESET or
             * CONFIGURED queued an earlier request while the framebuffer was
             * powered off.
             */
            s_wake_logo_show_requested = true;
            s_wake_logo_visible = false;
            s_wake_logo_min_elapsed = false;
            s_wake_logo_hold_deadline = 0;
            request_wake_logo = true;
            wake_logo_epoch = s_wake_logo_epoch;
        }
        wake_logo_visible = s_wake_logo_visible;
        rt_hw_interrupt_enable(level);

        if (request_wake_logo) {
            usb_osd_static_logo_show(wake_logo_epoch);
            USB_LOG_INFO("USB mode: request wake static logo\r\n");
        }

        if (wake_logo_active) {
            if (wake_logo_visible) {
                usb_mode_restore_saved_backlight();
            } else {
                aic_panel_backlight_disable();
            }
        } else if (wait_for_display) {
            aic_panel_backlight_disable();
        } else if (s_source_idle_backlight_off) {
            /*
             * The Display library can report D0 (and power the framebuffer
             * back on) before the arbiter has seen any USB event for the
             * re-plug.  Without this gate that stale callback lights the
             * retained last host frame for a moment before the Logo covers
             * it.  Only a real Display/CDC payload may relight the panel.
             */
            aic_panel_backlight_disable();
        } else {
            usb_mode_enable_host_backlight();
        }
        /* CONFIGURED is not repeated on every resume. Preserve an existing
         * CDC OUT request, or restore exactly one request if the last packet
         * completed immediately before SUSPEND. */
        cdc_arm_out_read();
    } else {
        aic_panel_backlight_disable();
        /* Some hosts report Display D3 without a USB bus SUSPEND event. */
        usb_mode_post(USB_MODE_EVENT_POWER);
    }
}

/* Called by cdc_image_processor.c only after a complete JPEG EOI is received. */
void cdc_image_frame_received(void)
{
    rt_base_t level;
    bool accepted = false;
    bool resume_hold_released = false;

    level = rt_hw_interrupt_disable();
    if (s_host_awake && !s_display_cmd_latched) {
        s_cdc_frame_pending = true;
        s_last_cdc_tick = rt_tick_get();
        s_cdc_activity_generation++;
        if (s_cdc_resume_hold) {
            s_cdc_resume_hold = false;
            resume_hold_released = true;
        }
        accepted = true;
    }
    rt_hw_interrupt_enable(level);

    if (accepted) {
        if (resume_hold_released) {
            USB_LOG_INFO("USB mode: fresh CDC frame after wake\r\n");
        }
        usb_mode_post(USB_MODE_EVENT_CDC_FRAME);
    }
}

/* Called when a decoded frame is available from the USB Display path. */
void mpp_decoder_mjpeg_frame_ready(void)
{
    /* CDC IMAGE has a later callback after pan-display/vsync. */
    if ((s_waiting_first_output && s_runtime_mode == USB_RUNTIME_DISPLAY) ||
        (s_wake_logo_active && s_resume_target == USB_TARGET_DISPLAY)) {
        usb_mode_post(USB_MODE_EVENT_OUTPUT_READY);
    }
}

/* Called by cdc_image_processor.c after the JPEG has reached the display. */
void cdc_image_frame_rendered(void)
{
    bool reveal_overlay = false;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    if (s_cdc_overlay_reveal_pending) {
        s_cdc_overlay_reveal_pending = false;
        reveal_overlay = true;
    }
    rt_hw_interrupt_enable(level);

    if (reveal_overlay) {
        /* decode_pic_cdc_persistent() has completed pan-display/vsync.  The
         * first valid CDC frame can now atomically cover Display. */
        cdc_image_processor_ui_layer_enable();
    }

    if (s_waiting_first_output || s_wake_logo_active) {
        usb_mode_post(USB_MODE_EVENT_OUTPUT_READY);
    }
}

/* Called from the LVGL timer after logo.png has reached the UI framebuffer. */
void usb_osd_static_logo_visible(unsigned int epoch)
{
    bool accepted;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    /* V1.64 rule: the boot Logo may become visible only while the
     * board is still in the local/no-host owner.  Enumeration must not turn a
     * late LVGL repaint into a second boot Logo or release it prematurely. */
    accepted = (s_wake_logo_active && s_host_awake &&
                epoch == s_wake_logo_epoch) ||
        (s_static_logo_owned && epoch == 0);
    if (accepted) {
        /*
         * Publish visibility before Display recovery can run.  Its reload and
         * re-enumeration paths must preserve the already visible logo.
         */
        if (s_wake_logo_active) {
            s_wake_logo_visible = true;
            s_wake_logo_min_elapsed = false;
            s_wake_logo_hold_deadline =
                rt_tick_get() +
                rt_tick_from_millisecond(USB_MODE_WAKE_LOGO_HOLD_MS);
            s_wake_logo_visible_epoch = epoch;
        } else if (s_static_logo_owned && epoch == 0) {
            /* V1.64 uses a hard PWM ownership gate.  Record only the accepted
             * local boot repaint; this preserves stable visibility timing
             * without admitting a late host repaint. */
            s_static_logo_visible = true;
            if (s_static_logo_force_hold) {
                s_static_logo_min_elapsed = false;
                /* Start the two-second guarantee only after the mode thread
                 * has actually restored PWM.  Panel/D0/authentication startup
                 * time must not consume the user's visible Logo interval. */
                s_static_logo_hold_deadline = 0;
            } else {
                s_static_logo_min_elapsed = true;
                s_static_logo_hold_deadline = 0;
            }
        }
    }
    rt_hw_interrupt_enable(level);

    if (accepted) {
        /*
         * lv_refr_now() only guarantees that LVGL submitted the UI update.  It
         * does not guarantee that a rotated high-resolution panel has scanned
         * the new frame.  Keep PWM dark and let the mode thread wait for real
         * VSYNCs before lighting either the boot or wake Logo.
         */
        usb_mode_post(USB_MODE_EVENT_WAKE_LOGO_VISIBLE);
    }
}

void usb_osd_static_logo_released(unsigned int epoch)
{
    (void)epoch;
    usb_mode_post(USB_MODE_EVENT_STATIC_LOGO_RELEASED);
}

static void usb_mode_thread_entry(void *parameter)
{
    (void)parameter;

    while (1) {
        uint32_t events = 0;
        bool awake;
        bool replay;
        bool attached;
        rt_base_t level;

        if (rt_event_recv(s_mode_event, 0xFFFFFFFFU,
                          RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                          RT_WAITING_FOREVER, &events) != RT_EOK) {
            continue;
        }

        if (events & USB_MODE_EVENT_STOP) {
            usb_mode_enter_static_logo();
            if (s_mode_stop_sem) {
                rt_sem_release(s_mode_stop_sem);
            }
            continue;
        }

        if (!s_mode_service_enabled) {
            continue;
        }

        level = rt_hw_interrupt_disable();
        awake = s_host_awake;
        attached = s_host_attached;
        replay = s_wake_replay_pending;
        if (awake && replay) {
            s_wake_replay_pending = false;
        }
        rt_hw_interrupt_enable(level);

        if (events & USB_MODE_EVENT_HOST_RESET) {
            bool abort_logo_save;
            bool blank_stale_cdc;

            level = rt_hw_interrupt_disable();
            abort_logo_save = s_host_reset_abort_logo_save_pending;
            blank_stale_cdc = s_host_reset_blank_cdc_pending;
            s_host_reset_abort_logo_save_pending = false;
            s_host_reset_blank_cdc_pending = false;
            rt_hw_interrupt_enable(level);

            if (abort_logo_save) {
                logo_save_abort();
            }
            /*
             * USB RESET only invalidates transport state.  Discard a JPEG
             * fragment left by the old host without touching the framebuffer,
             * panel power, backlight, or current Display/CDC ownership.
             */
            if (blank_stale_cdc) {
                /*
                 * A PC restart or a data-cable re-plug can leave a partial CDC
                 * JPEG and the previous complete frame in local buffers.
                 * Restart the session from the mode thread so USB reset can
                 * arm EP0 immediately.  The session variant keeps the UI layer
                 * enabled - the host is about to stream again, and a disabled
                 * layer would freeze the panel on the last visible frame.
                 */
                usb_mode_reset_cdc_session_output();
            } else {
                /* Also invalidate a complete frame captured behind Display. */
                cdc_image_processor_reset_stream();
            }
        }

        if (events & USB_MODE_EVENT_RESTART_RELIGHT) {
            bool relight;
            bool release_to_cdc = false;
            unsigned int logo_epoch;

            level = rt_hw_interrupt_disable();
            /*
             * Only take over when a configured host exists but the normal
             * hand-off never lit the panel (Logo not painted, or no host frame
             * was ever rendered).  A real cable unplug must stay dark.
             */
            relight = s_host_attached && s_host_awake &&
                (s_runtime_mode == USB_RUNTIME_DISPLAY ||
                 s_runtime_mode == USB_RUNTIME_CDC) &&
                (!s_wake_logo_active || s_wake_logo_host_frame_ready) &&
                !s_static_logo_owned && !s_static_logo_release_pending;
            if (relight) {
                /*
                 * Never release an active restart Logo on a timer alone.  A
                 * timeout is not proof that Display/CDC has produced a valid
                 * frame and exposing the retained buffer here caused the
                 * cold-boot/re-plug garbage flash.  Once a host frame is
                 * confirmed, this remains a harmless recovery fallback.
                 */
                s_wake_logo_active = false;
                s_wake_logo_show_requested = false;
                s_wake_logo_visible = false;
                s_wake_logo_min_elapsed = false;
                s_wake_logo_host_frame_ready = false;
                s_wake_logo_release_pending = false;
                s_wake_logo_hold_deadline = 0;
                s_waiting_first_output = false;
                s_source_idle_backlight_off = false;
                s_source_wake_pending = false;
                s_last_source_data_tick = rt_tick_get();
                logo_epoch = s_wake_logo_epoch;
                /*
                 * CDC and the static PNG share the UI layer, and CDC needs it
                 * opaque: use the same release variant the normal hand-off
                 * uses, or the panel would go transparent instead of lit.
                 */
                release_to_cdc = s_runtime_mode == USB_RUNTIME_CDC;
            }
            rt_hw_interrupt_enable(level);

            if (relight) {
                if (release_to_cdc) {
                    usb_osd_static_logo_release_cdc(logo_epoch);
                } else {
                    usb_osd_static_logo_hide(logo_epoch);
                }
                usb_mode_restore_saved_backlight();
                USB_LOG_WRN("USB mode: host restart -> force backlight on\r\n");
            }
        }

        if (events & USB_MODE_EVENT_CDC_RESTART_RESUME) {
            /*
             * Failsafe for a restarted PC which never delivered a decodable
             * CDC frame: re-enter CDC once so decoder enable, output enable,
             * UI layer and stream bookkeeping are asserted again.  Without
             * this the panel can stay on the first frame of the new session
             * until the cable is pulled.
             */
            if (s_host_attached && s_host_awake &&
                s_runtime_mode == USB_RUNTIME_CDC &&
                !s_wake_logo_active && !s_static_logo_owned) {
                USB_LOG_WRN("USB mode: host restart -> reload CDC stream\r\n");
                usb_mode_enter_cdc();
            }
        }

        if (events & USB_MODE_EVENT_WAKE_LOGO_VISIBLE) {
            bool logo_visible;

            level = rt_hw_interrupt_disable();
            logo_visible =
                (s_wake_logo_active && s_host_awake &&
                 s_wake_logo_visible &&
                 s_wake_logo_visible_epoch == s_wake_logo_epoch) ||
                (s_static_logo_owned && s_static_logo_visible);
            rt_hw_interrupt_enable(level);

            if (logo_visible) {
                /* lv_refr_now() in the UI callback has already committed the
                 * PNG.  Do not wait for extra VSYNC ioctls here: after first
                 * power-on or D3 they can wait on a controller whose scanout
                 * has not started yet, leaving a valid Logo permanently dark.
                 * Recheck ownership immediately before restoring PWM. */
                level = rt_hw_interrupt_disable();
                logo_visible =
                    (s_wake_logo_active && s_wake_logo_visible &&
                     s_wake_logo_visible_epoch == s_wake_logo_epoch) ||
                    (s_static_logo_owned && s_static_logo_visible);
                rt_hw_interrupt_enable(level);
                if (logo_visible) {
                    usb_mode_restore_saved_backlight();
                    level = rt_hw_interrupt_disable();
                    if (s_static_logo_owned && s_static_logo_visible &&
                        s_static_logo_force_hold &&
                        s_static_logo_hold_deadline == 0) {
                        s_static_logo_hold_deadline =
                            rt_tick_get() +
                            rt_tick_from_millisecond(
                                USB_MODE_BOOT_LOGO_HOLD_MS);
                    }
                    rt_hw_interrupt_enable(level);
                    USB_LOG_INFO("USB mode: static logo scanout visible\r\n");
                }
            }
        }

        if (events & USB_MODE_EVENT_INIT) {
            /* Always commit one local frame first.  Fast enumeration on the
             * large panel must not bypass the first-power Logo. */
            usb_mode_enter_static_logo();
        }

        if (events & USB_MODE_EVENT_POWER) {
            if (!attached) {
                /* SATA may keep DDR/display state alive after the USB data
                 * cable is removed.  Treat that exactly like host D3: clean
                 * the producers and keep PWM off until USB returns. */
                usb_mode_enter_sleep();
                continue;
            } else if (!awake) {
                usb_mode_enter_sleep();
                continue;
            }

            if (s_static_logo_owned &&
                (!s_static_logo_visible || !s_static_logo_min_elapsed) &&
                s_runtime_mode == USB_RUNTIME_STATIC_LOGO) {
                continue;
            }

            if (s_runtime_mode == USB_RUNTIME_CDC &&
                s_cdc_output_restore_pending) {
                usb_mode_restore_cdc_output_after_wake();
            } else if (replay ||
                       s_runtime_mode == USB_RUNTIME_SLEEP ||
                       s_runtime_mode == USB_RUNTIME_STATIC_LOGO) {
                enum usb_transition_target target = s_resume_target;

                if (target == USB_TARGET_NONE) {
                    target = usb_mode_pending_target();
                }
                usb_mode_activate_target(target);
            }
        }

        if (events & USB_MODE_EVENT_DISPLAY_RECOVER) {
            uint32_t display_count = usb_display_recv_frame_count();
            bool recover;

            level = rt_hw_interrupt_disable();
            recover = s_host_attached && s_host_awake &&
                s_resume_target == USB_TARGET_DISPLAY &&
                s_runtime_mode == USB_RUNTIME_DISPLAY &&
                s_command_target != USB_TARGET_CDC &&
                s_transition_target != USB_TARGET_CDC &&
                !s_cdc_frame_pending &&
                !s_wake_logo_host_frame_ready &&
                display_count == s_display_wake_recovery_baseline;
            if (recover) {
                s_waiting_first_output = true;
            }
            rt_hw_interrupt_enable(level);

            if (recover) {
                uint32_t reloaded_count;

                USB_LOG_WRN("USB mode: no Display frame after wake/switch; "
                            "reload Display mode\r\n");
                usb_mode_disable_host_backlight();
                usb_mode_enter_display();

                reloaded_count = usb_display_recv_frame_count();
                level = rt_hw_interrupt_disable();
                if (s_host_attached && s_host_awake &&
                    s_resume_target == USB_TARGET_DISPLAY &&
                    s_runtime_mode == USB_RUNTIME_DISPLAY &&
                    s_command_target != USB_TARGET_CDC &&
                    s_transition_target == USB_TARGET_NONE &&
                    !s_cdc_frame_pending &&
                    !s_wake_logo_host_frame_ready &&
                    s_waiting_first_output) {
                    s_display_frame_baseline = reloaded_count;
                    s_display_frame_pending = false;
                    /*
                     * Keep the recovered Display receiver running.  Do not
                     * reset/re-enumerate the USB controller: the official
                     * driver leaves controller ownership to the host's next
                     * RESET/CONFIGURED sequence.
                     */
                    s_display_wake_reenum_armed = false;
                }
                rt_hw_interrupt_enable(level);
            }
        }

        if (events & USB_MODE_EVENT_OUTPUT_READY) {
            bool host_output_ready;
            bool output_behind_logo = false;
            bool release_logo = false;
            bool release_static_logo = false;

            /* Let the renderer commit the decoded frame on the next VSYNC. */
            rt_thread_mdelay(USB_MODE_OUTPUT_SETTLE_MS);

            level = rt_hw_interrupt_disable();
            host_output_ready = s_host_awake &&
                usb_mode_display_power_is_on() &&
                (s_runtime_mode == USB_RUNTIME_DISPLAY ||
                 s_runtime_mode == USB_RUNTIME_CDC) &&
                (s_waiting_first_output || s_wake_logo_active ||
                 (s_static_logo_owned &&
                  s_static_logo_host_frame_ready &&
                  s_static_logo_min_elapsed));
            if (host_output_ready) {
                s_waiting_first_output = false;
                s_host_output_committed = true;
                /*
                 * Complete the Display/backlight hand-off atomically with the
                 * first decoded host frame.  A recovery event may already be
                 * pending when this callback runs; clearing both watchdogs
                 * here prevents that stale event from switching the PWM off
                 * after the static logo disappears.
                 */
                s_display_wake_recovery_eligible = false;
                s_display_wake_recovery_armed = false;
                s_display_wake_reenum_armed = false;
                if (s_wake_logo_active) {
                    s_wake_logo_host_frame_ready = true;
                    output_behind_logo = true;
                    if (s_wake_logo_visible &&
                        s_wake_logo_min_elapsed &&
                        !s_wake_logo_release_pending) {
                        s_wake_logo_release_pending = true;
                        release_logo = true;
                    }
                } else if (s_static_logo_owned) {
                    s_static_logo_host_frame_ready = true;
                    if (!s_static_logo_force_hold ||
                        s_static_logo_min_elapsed) {
                        s_static_logo_owned = false;
                        s_static_logo_visible = false;
                        s_static_logo_release_pending = true;
                        s_static_logo_host_frame_ready = false;
                        if (s_static_logo_force_hold) {
                            s_first_power_logo_pending = false;
                            s_static_logo_force_hold = false;
                        }
                        release_static_logo = true;
                    } else {
                        /* Keep the already committed host frame covered until
                         * the first-power Logo has been visible for 2 seconds. */
                        output_behind_logo = true;
                    }
                }
            }
            rt_hw_interrupt_enable(level);

            if (release_logo) {
                usb_mode_post(USB_MODE_EVENT_WAKE_LOGO_RELEASE);
            }
            if (release_static_logo) {
                if (s_runtime_mode == USB_RUNTIME_CDC) {
                    usb_osd_static_logo_release_cdc(0);
                } else {
                    usb_osd_static_logo_hide(0);
                }
            }
            if (host_output_ready && !output_behind_logo) {
                if (!release_static_logo)
                    usb_mode_enable_host_backlight();
                USB_LOG_INFO("USB mode: first host frame visible\r\n");
            } else if (output_behind_logo) {
                USB_LOG_INFO("USB mode: fresh host frame ready behind logo\r\n");
            }
        }

        if (events & USB_MODE_EVENT_WAKE_LOGO_RELEASE) {
            bool release_logo;
            bool release_to_cdc = false;
            unsigned int logo_epoch;

            level = rt_hw_interrupt_disable();
            release_logo = s_host_awake &&
                usb_mode_display_power_is_on() &&
                s_wake_logo_active &&
                s_wake_logo_visible &&
                s_wake_logo_min_elapsed &&
                s_wake_logo_host_frame_ready;
            logo_epoch = s_wake_logo_epoch;
            if (release_logo) {
                release_to_cdc =
                    s_runtime_mode == USB_RUNTIME_CDC &&
                    s_resume_target == USB_TARGET_CDC;
                s_wake_logo_active = false;
                s_wake_logo_show_requested = false;
                s_wake_logo_visible = false;
                s_wake_logo_release_pending = false;
                s_wake_logo_hold_deadline = 0;
                s_waiting_first_output = false;
                s_display_wake_recovery_eligible = false;
                s_display_wake_recovery_armed = false;
                s_display_wake_reenum_armed = false;
            } else {
                s_wake_logo_release_pending = false;
            }
            rt_hw_interrupt_enable(level);

            if (release_logo) {
                s_static_logo_release_pending = true;
                if (release_to_cdc) {
                    usb_osd_static_logo_release_cdc(logo_epoch);
                } else {
                    usb_osd_static_logo_hide(logo_epoch);
                }
                USB_LOG_INFO("USB mode: wake logo -> %s\r\n",
                             release_to_cdc ? "CDC" : "Display");
            }
        }

        if (events & USB_MODE_EVENT_STATIC_LOGO_RELEASED) {
            bool release_backlight;

            level = rt_hw_interrupt_disable();
            release_backlight = s_static_logo_release_pending &&
                s_host_awake &&
                usb_mode_display_power_is_on() &&
                (s_runtime_mode == USB_RUNTIME_DISPLAY ||
                 s_runtime_mode == USB_RUNTIME_CDC);
            if (release_backlight)
                s_static_logo_release_pending = false;
            rt_hw_interrupt_enable(level);

            if (release_backlight) {
                usb_mode_enable_host_backlight();
                USB_LOG_INFO("USB mode: static Logo released after host frame\r\n");
            }
        }

        if (events & USB_MODE_EVENT_SOURCE_ACTIVITY) {
            bool wake_backlight;

            level = rt_hw_interrupt_disable();
            /*
             * No host-awake gate: the panel must come back as soon as any
             * Display/CDC payload arrives, independently of the (unreliable)
             * attach state after a data-cable unplug.
             */
            wake_backlight = s_source_wake_pending &&
                usb_mode_display_power_is_on() &&
                (s_runtime_mode == USB_RUNTIME_DISPLAY ||
                 s_runtime_mode == USB_RUNTIME_CDC) &&
                !s_waiting_first_output &&
                !s_wake_logo_active;
            s_source_wake_pending = false;
            rt_hw_interrupt_enable(level);

            if (wake_backlight) {
                usb_mode_enable_host_backlight();
                USB_LOG_INFO("USB mode: source data -> backlight on\r\n");
            }
        }

        if (events & USB_MODE_EVENT_SOURCE_IDLE) {
            bool blank_backlight;

            level = rt_hw_interrupt_disable();
            /*
             * Same reasoning as the wake path above: blank on missing
             * Display/CDC payloads even when the host disappeared without
             * any USB event, so the retained last frame cannot stay lit.
             */
            blank_backlight = s_source_idle_backlight_off &&
                (s_runtime_mode == USB_RUNTIME_DISPLAY ||
                 s_runtime_mode == USB_RUNTIME_CDC) &&
                !s_waiting_first_output &&
                !s_wake_logo_active;
            rt_hw_interrupt_enable(level);

            if (blank_backlight) {
                aic_panel_backlight_disable();
                /*
                 * No payload for USB_MODE_SOURCE_IDLE_TIMEOUT_MS means the
                 * link is gone, so the JPEG which was in flight will never be
                 * completed.  Drop it now: decoding its tail after the cable
                 * comes back shows a partially corrupt image as the first
                 * frame of the new session.
                 */
                if (s_runtime_mode == USB_RUNTIME_CDC) {
                    usb_mode_blank_stale_cdc_output();
                    usb_mode_resume_cdc_output_if_current();
                } else {
                    cdc_image_processor_reset_stream();
                }
                USB_LOG_INFO("USB mode: source idle -> backlight off\r\n");
            }
        }

        if (!awake || !attached || !usb_mode_display_power_is_on()) {
            continue;
        }

        if (events & (USB_MODE_EVENT_CDC_FRAME |
                      USB_MODE_EVENT_DISPLAY_FRAME |
                      USB_MODE_EVENT_CDC_REQUEST |
                      USB_MODE_EVENT_DISPLAY_REQUEST)) {
            usb_mode_handle_source_event(events);
        }

        if (events & USB_MODE_EVENT_TRANSITION_RETRY) {
            enum usb_transition_target target = s_transition_target;

            if (!s_host_attached || !s_host_awake) {
                usb_mode_enter_static_logo();
            } else if (target == USB_TARGET_CDC) {
                usb_mode_enter_cdc();
            } else if (target == USB_TARGET_DISPLAY) {
                usb_mode_enter_display();
            }
        }

        if ((events & USB_MODE_EVENT_CDC_TIMEOUT) &&
            s_runtime_mode == USB_RUNTIME_CDC) {
            bool timeout_still_valid;

            level = rt_hw_interrupt_disable();
            timeout_still_valid = s_auto_switch_enabled &&
                s_cdc_stream_active &&
                !s_cdc_resume_hold &&
                s_timeout_generation == s_cdc_activity_generation &&
                tick_to_ms(rt_tick_get() - s_last_cdc_tick) >=
                    CDC_MODE_TIMEOUT_MS;
            if (timeout_still_valid) {
                s_cdc_frame_pending = false;
            }
            rt_hw_interrupt_enable(level);
            if (timeout_still_valid) {
                USB_LOG_INFO("USB mode: CDC timeout -> Display\r\n");
                usb_mode_enter_display();
            }
        }

        if (events & USB_MODE_EVENT_DTR_CLOSED) {
            bool close_still_valid;

            level = rt_hw_interrupt_disable();
            close_still_valid = s_host_awake &&
                usb_mode_display_power_is_on() && !dtr_enable &&
                !s_cdc_resume_hold &&
                s_dtr_close_power_generation == s_power_generation &&
                s_dtr_close_generation == s_cdc_activity_generation;
            if (close_still_valid) {
                s_cdc_frame_pending = false;
                s_cdc_resume_hold = false;
            }
            rt_hw_interrupt_enable(level);
            if (close_still_valid &&
                s_runtime_mode == USB_RUNTIME_CDC) {
                USB_LOG_INFO("USB mode: CDC DTR closed -> Display\r\n");
                usb_mode_enter_display();
            }
        }
    }
}
static int usb_mode_service_init(void)
{
    if (s_mode_thread) {
        s_mode_service_enabled = true;
        return RT_EOK;
    }

    s_mode_event = rt_event_create("usb_mode", RT_IPC_FLAG_PRIO);
    if (!s_mode_event) {
        return -RT_ENOMEM;
    }

    s_mode_stop_sem = rt_sem_create("usb_stp", 0, RT_IPC_FLAG_PRIO);
    if (!s_mode_stop_sem) {
        rt_event_delete(s_mode_event);
        s_mode_event = RT_NULL;
        return -RT_ENOMEM;
    }

    s_mode_thread = rt_thread_create("usb_mode", usb_mode_thread_entry,
                                     RT_NULL, USB_MODE_THREAD_STACK,
                                     USB_MODE_THREAD_PRIO,
                                     USB_MODE_THREAD_TICK);
    if (!s_mode_thread) {
        rt_sem_delete(s_mode_stop_sem);
        s_mode_stop_sem = RT_NULL;
        rt_event_delete(s_mode_event);
        s_mode_event = RT_NULL;
        return -RT_ENOMEM;
    }

    s_mode_service_enabled = true;
    rt_thread_startup(s_mode_thread);
    return RT_EOK;
}

static void display_mode_timer_cb(void *parameter)
{
    uint32_t display_count;
    bool display_changed = false;
    bool display_recover_due = false;
    bool cdc_timed_out = false;
    bool source_idle_due = false;
    bool wake_logo_release_due = false;
    bool boot_logo_release_due = false;
    bool restart_relight_due = false;
    bool cdc_restart_resume_due = false;
    rt_base_t level;

    (void)parameter;

    /* Long D3/hibernate may discard only the CDC transfer while the Display
     * interface remains alive.  Retry the CDC OUT request independently; the
     * software armed latch makes this a no-op during normal operation. */
    cdc_arm_out_read();

    display_count = usb_display_recv_frame_count();
    level = rt_hw_interrupt_disable();
    if (s_host_awake && display_count != s_display_frame_baseline) {
        s_display_frame_baseline = display_count;
        s_display_frame_pending = true;
        display_changed = true;
        s_display_wake_recovery_armed = false;
        s_display_wake_reenum_armed = false;
    }
    if (s_display_wake_recovery_armed) {
        if (!s_host_awake ||
            s_resume_target != USB_TARGET_DISPLAY ||
            s_runtime_mode == USB_RUNTIME_CDC ||
            s_command_target == USB_TARGET_CDC ||
            s_transition_target == USB_TARGET_CDC ||
            s_cdc_frame_pending ||
            s_wake_logo_host_frame_ready ||
            display_count != s_display_wake_recovery_baseline) {
            s_display_wake_recovery_armed = false;
        } else if ((rt_int32_t)(rt_tick_get() -
                               s_display_wake_recovery_deadline) >= 0) {
            s_display_wake_recovery_armed = false;
            display_recover_due = true;
        }
    }
    if (s_auto_switch_enabled && s_cdc_stream_active &&
        !s_cdc_resume_hold &&
        tick_to_ms(rt_tick_get() - s_last_cdc_tick) >= CDC_MODE_TIMEOUT_MS) {
        s_timeout_generation = s_cdc_activity_generation;
        cdc_timed_out = true;
    }
    /*
     * Deliberately not gated on s_host_awake: a data-cable unplug is invisible
     * to the arbiter (no PLUG_CHECK interrupt), so the only reliable "host is
     * gone" evidence is the absence of Display/CDC payloads.
     */
    if (USB_MODE_SOURCE_IDLE_BLANK_ENABLE &&
        (s_runtime_mode == USB_RUNTIME_DISPLAY ||
         s_runtime_mode == USB_RUNTIME_CDC) &&
        !s_waiting_first_output &&
        !s_wake_logo_active &&
        !s_source_idle_backlight_off &&
        tick_to_ms(rt_tick_get() - s_last_source_data_tick) >=
            USB_MODE_SOURCE_IDLE_TIMEOUT_MS) {
        s_source_idle_backlight_off = true;
        source_idle_due = true;
    }
    /*
     * Host restart (PC reboot) failsafes.  USB power and enumeration bounce
     * for a while during one reboot, so the grace period is re-based until the
     * host is configured and awake again; it then starts to count down.
     */
    if (s_restart_relight_armed) {
        if (!s_host_attached || !s_host_awake) {
            s_restart_relight_deadline =
                rt_tick_get() +
                rt_tick_from_millisecond(USB_MODE_RESTART_RELIGHT_MS);
        } else if ((rt_int32_t)(rt_tick_get() -
                                s_restart_relight_deadline) >= 0) {
            s_restart_relight_armed = false;
            restart_relight_due = true;
        }
    }
    if (s_cdc_restart_resume_armed) {
        if (s_runtime_mode != USB_RUNTIME_CDC &&
            !s_static_logo_owned && !s_wake_logo_active) {
            s_cdc_restart_resume_armed = false;
        } else if (!s_host_attached || !s_host_awake ||
                   s_static_logo_owned || s_wake_logo_active) {
            s_cdc_restart_resume_deadline =
                rt_tick_get() +
                rt_tick_from_millisecond(USB_MODE_CDC_RESTART_RESUME_MS);
        } else if ((rt_int32_t)(rt_tick_get() -
                                s_cdc_restart_resume_deadline) >= 0) {
            s_cdc_restart_resume_armed = false;
            cdc_restart_resume_due = true;
        }
    }
    if (s_wake_logo_active && s_wake_logo_visible) {
        if (!s_wake_logo_min_elapsed &&
            s_wake_logo_hold_deadline != 0 &&
            (rt_int32_t)(rt_tick_get() -
                         s_wake_logo_hold_deadline) >= 0) {
            s_wake_logo_min_elapsed = true;
        }
        if (s_wake_logo_min_elapsed &&
            s_wake_logo_host_frame_ready &&
            !s_wake_logo_release_pending) {
            s_wake_logo_release_pending = true;
            wake_logo_release_due = true;
        }
    }
    if (s_static_logo_owned && s_static_logo_visible &&
        !s_static_logo_min_elapsed &&
        s_static_logo_hold_deadline != 0 &&
        (rt_int32_t)(rt_tick_get() -
                     s_static_logo_hold_deadline) >= 0) {
        s_static_logo_min_elapsed = true;
        boot_logo_release_due = s_static_logo_force_hold &&
            s_static_logo_host_frame_ready &&
            s_host_attached && s_host_awake;
    }
    rt_hw_interrupt_enable(level);


    if (display_changed) {
        /* Counter polling is a fallback if the decoder callback is delayed. */
        usb_mode_note_source_activity();
        usb_mode_post(USB_MODE_EVENT_DISPLAY_FRAME);
    }
    if (display_recover_due) {
        usb_mode_post(USB_MODE_EVENT_DISPLAY_RECOVER);
    }
    if (cdc_timed_out) {
        usb_mode_post(USB_MODE_EVENT_CDC_TIMEOUT);
    }
    if (source_idle_due) {
        usb_mode_post(USB_MODE_EVENT_SOURCE_IDLE);
    }
    if (restart_relight_due) {
        usb_mode_post(USB_MODE_EVENT_RESTART_RELIGHT);
    }
    if (cdc_restart_resume_due) {
        usb_mode_post(USB_MODE_EVENT_CDC_RESTART_RESUME);
    }
    if (wake_logo_release_due) {
        usb_mode_post(USB_MODE_EVENT_WAKE_LOGO_RELEASE);
    }
    if (boot_logo_release_due) {
        /* Re-run the normal first-frame hand-off; OUTPUT_READY now sees both
         * prerequisites (valid host frame and elapsed two-second hold). */
        usb_mode_post(USB_MODE_EVENT_OUTPUT_READY);
    }
}

void usb_disable_auto_switch(void)
{
    s_auto_switch_enabled = false;
}

void usb_enable_auto_switch(void)
{
    s_auto_switch_enabled = true;
    cdc_note_activity();
}

static bool is_mode_switch_command(const uint8_t *buf, uint32_t len)
{
    return len == CDC_MODE_CMD_LEN &&
           buf[0] == CDC_MODE_CMD_HEAD0 &&
           buf[1] == CDC_MODE_CMD_HEAD1 &&
           buf[3] == CDC_MODE_CMD_TAIL0 &&
           buf[4] == CDC_MODE_CMD_TAIL1;
}

static bool is_sn_request_command(const uint8_t *buf, uint32_t len)
{
    return len >= sizeof(g_sn_req_sig) &&
           memcmp(buf, g_sn_req_sig, sizeof(g_sn_req_sig)) == 0;
}

static bool is_brightness_command(const uint8_t *buf, uint32_t len)
{
    return len == CDC_BRIGHTNESS_CMD_LEN &&
           buf[0] == CDC_BRIGHTNESS_HEAD0 &&
           buf[1] == CDC_BRIGHTNESS_HEAD1 &&
           buf[3] == CDC_BRIGHTNESS_TAIL0 &&
           buf[4] == CDC_BRIGHTNESS_TAIL1;
}

static bool is_sn_update_command(const uint8_t *buf, uint32_t len)
{
    return len == CDC_SN_UPDATE_CMD_LEN &&
           buf[0] == CDC_SN_UPDATE_HEAD &&
           buf[CDC_SN_UPDATE_CMD_LEN - 1] == CDC_SN_UPDATE_TAIL;
}

static bool is_ota_start_command(const uint8_t *buf, uint32_t len)
{
    return len >= CDC_OTA_CMD_LEN &&
           buf[0] == CDC_OTA_HEAD0 &&
           buf[1] == CDC_OTA_HEAD1 &&
           buf[2] == CDC_OTA_HEAD2;
}

static bool is_logo_save_command(const uint8_t *buf, uint32_t len)
{
    return len >= CDC_LOGO_CMD_LEN &&
           memcmp(buf, CDC_LOGO_CMD, CDC_LOGO_CMD_LEN) == 0;
}

static bool is_control_packet(const uint8_t *buf, uint32_t len)
{
    return is_mode_switch_command(buf, len) ||
           is_brightness_command(buf, len) ||
           is_sn_update_command(buf, len) ||
           is_sn_request_command(buf, len) ||
           is_logo_save_command(buf, len) ||
           is_ota_start_command(buf, len);
}

static bool cdc_send_to_host(const uint8_t *data, uint32_t len)
{
    int ret;

    if (!dtr_enable || ep_tx_busy_flag || !data || len == 0 || len > CDC_TX_BUF_SIZE) {
        return false;
    }

    memcpy(write_buffer, data, len);
    ep_tx_busy_flag = true;
    ret = usbd_ep_start_write(runtime_in_ep(), write_buffer, len);
    if (ret != 0) {
        ep_tx_busy_flag = false;
        USB_LOG_ERR("CDC write failed: %d\r\n", ret);
        return false;
    }

    return true;
}

static void cdc_send_text_response(const char *text)
{
    size_t len;

    if (!text) {
        return;
    }

    len = strlen(text);
    if (len > CDC_TX_BUF_SIZE) {
        len = CDC_TX_BUF_SIZE;
    }

    cdc_send_to_host((const uint8_t *)text, (uint32_t)len);
}

static void send_sn_to_host(void)
{
    char sn[CDC_SN_MAX_LEN];
    size_t len = usb_cdc_load_sn(sn, sizeof(sn));

    if (len == 0) {
        return;
    }

    if (len > CDC_TX_BUF_SIZE) {
        len = CDC_TX_BUF_SIZE;
    }

    cdc_send_to_host((const uint8_t *)sn, (uint32_t)len);
}

static void logo_save_close(bool success)
{
    if (s_logo_save_timer) {
        rt_timer_stop(s_logo_save_timer);
    }

    if (s_logo_fd >= 0) {
        close(s_logo_fd);
        s_logo_fd = -1;
    }

    s_logo_save_state = CDC_LOGO_SAVE_IDLE;
    usb_enable_auto_switch();
    cdc_send_text_response(success ? "LOGO Save Success" : "LOGO Save Failed");
}

static void logo_save_abort(void)
{
    if (s_logo_save_timer) {
        rt_timer_stop(s_logo_save_timer);
    }

    if (s_logo_fd >= 0) {
        close(s_logo_fd);
        s_logo_fd = -1;
    }

    s_logo_save_state = CDC_LOGO_SAVE_IDLE;
    usb_enable_auto_switch();
}

static void logo_save_timer_cb(void *parameter)
{
    (void)parameter;

    if (s_logo_save_state == CDC_LOGO_SAVE_RECEIVING) {
        logo_save_close(true);
    }
}

static void logo_save_restart_timer(void)
{
    if (!s_logo_save_timer) {
        s_logo_save_timer = rt_timer_create("cdc_logo",
                                            logo_save_timer_cb,
                                            RT_NULL,
                                            rt_tick_from_millisecond(CDC_LOGO_TIMEOUT_MS),
                                            RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    }

    if (s_logo_save_timer) {
        rt_timer_stop(s_logo_save_timer);
        rt_timer_start(s_logo_save_timer);
    }
}

static void logo_make_dirs(void)
{
    mkdir("/data", 0755);
    mkdir("/data/lvgl_data", 0755);
    mkdir("/data/lvgl_data/logo", 0755);
}

static void logo_save_data(const uint8_t *buf, uint32_t len)
{
    ssize_t written;

    if (s_logo_save_state != CDC_LOGO_SAVE_RECEIVING || s_logo_fd < 0) {
        return;
    }

    if (len > 0) {
        written = write(s_logo_fd, buf, len);
        if (written != (ssize_t)len) {
            logo_save_close(false);
            return;
        }
    }

    logo_save_restart_timer();
}

static void logo_save_start(const uint8_t *buf, uint32_t len)
{
    if (s_logo_save_state == CDC_LOGO_SAVE_RECEIVING) {
        logo_save_close(false);
    }

    usb_disable_auto_switch();
    logo_make_dirs();
    s_logo_fd = open(CDC_LOGO_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (s_logo_fd < 0) {
        s_logo_save_state = CDC_LOGO_SAVE_IDLE;
        usb_enable_auto_switch();
        cdc_send_text_response("LOGO Open Failed");
        return;
    }

    s_logo_save_state = CDC_LOGO_SAVE_RECEIVING;
    cdc_send_text_response("LOGO Save Start");

    if (len > CDC_LOGO_CMD_LEN) {
        logo_save_data(buf + CDC_LOGO_CMD_LEN, len - CDC_LOGO_CMD_LEN);
    } else {
        logo_save_restart_timer();
    }
}


static void handle_control_packet(const uint8_t *buf, uint32_t len)
{
    if (is_mode_switch_command(buf, len)) {
        if (buf[2] == CDC_MODE_VALUE_CDC) {
            rt_base_t level = rt_hw_interrupt_disable();
            s_command_target = USB_TARGET_CDC;
            s_resume_target = USB_TARGET_CDC;
            s_cdc_resume_hold = false;
            s_display_cmd_latched = false;
            s_cdc_frame_pending = true;
            s_display_frame_pending = false;
            s_last_cdc_tick = rt_tick_get();
            s_cdc_activity_generation++;
            rt_hw_interrupt_enable(level);
            usb_mode_post(USB_MODE_EVENT_CDC_REQUEST);
        } else if (buf[2] == CDC_MODE_VALUE_DISPLAY) {
            rt_base_t level = rt_hw_interrupt_disable();
            /*
             * Restore the original manual-mode contract: queued CDC payload
             * cannot override Display until a CDC command clears this latch.
             */
            s_command_target = USB_TARGET_DISPLAY;
            s_resume_target = USB_TARGET_DISPLAY;
            s_cdc_resume_hold = false;
            s_cdc_output_restore_pending = false;
            s_display_cmd_latched = true;
            s_cdc_frame_pending = false;
            s_display_frame_pending = true;
            rt_hw_interrupt_enable(level);
            usb_mode_post(USB_MODE_EVENT_DISPLAY_REQUEST);
        }
        return;
    }

    if (is_brightness_command(buf, len)) {
        if (usb_cdc_set_backlight(buf[2], true)) {
            cdc_send_text_response("Brightness Set Success");
        } else {
            cdc_send_text_response("Brightness Set Failed");
        }
        return;
    }

    if (is_sn_update_command(buf, len)) {
        if (usb_cdc_queue_sn_update(buf, len)) {
            cdc_send_text_response("SN Update Success");
        } else {
            cdc_send_text_response("SN Update Failed");
        }
        return;
    }

    if (is_sn_request_command(buf, len)) {
        send_sn_to_host();
        return;
    }

    if (is_logo_save_command(buf, len)) {
        logo_save_start(buf, len);
        return;
    }

}

#ifdef LPKG_CHERRYUSB_DEVICE_COMPOSITE
void usbd_comp_acm_event_handler(uint8_t event)
#else
void usbd_event_handler(uint8_t event)
#endif
{
    switch (event) {
        case USBD_EVENT_RESET:
        {
            /* Hibernate resumes through USB re-enumeration.  Gate PWM before
             * the Display library can expose its retained framebuffer; the
             * static Logo visible callback restores it after repaint+VSYNC. */
            if (!usb_osd_logo_backlight_is_owned())
                aic_panel_backlight_disable();
            dtr_enable = 0;
            ep_tx_busy_flag = false;
            s_ota_active = false;
            cdc_out_read_disarm();
            __atomic_add_fetch(&s_usb_session_generation, 1,
                               __ATOMIC_ACQ_REL);
            __atomic_add_fetch(&s_cdc_rx_drop_generation, 1,
                               __ATOMIC_ACQ_REL);
            s_host_reset_abort_logo_save_pending = true;
            s_host_reset_blank_cdc_pending =
                s_runtime_mode == USB_RUNTIME_CDC ||
                s_resume_target == USB_TARGET_CDC ||
                s_cdc_stream_active ||
                s_cdc_frame_pending;
            /*
             * Match the official controller policy: a bus RESET resets USB
             * protocol state only.  Keep panel power/PWM untouched, but reset
             * stale CDC image buffers from the mode thread when the old session
             * was streaming into the MJPEG path.
             */
            usb_mode_post(USB_MODE_EVENT_HOST_RESET);
            /* V1.64 arms the static Logo on a real host restart at RESET;
             * keep the V1.64 protocol-only reset for initial enumeration and
             * Display's own recovery re-enumeration. */
            if ((s_wake_logo_armed || s_host_restart_armed) &&
                !s_display_reenumeration_active) {
                usb_mode_usb_power_changed(true, false);
                usb_mode_start_reset_static_logo();
            }
            break;
        }
        case USBD_EVENT_CONNECTED:
            /* SATA keeps the previous scanout alive across a data-cable
             * re-plug.  Hide it at the earliest USB indication; CONFIGURED
             * will repaint the Logo before PWM is restored.  Initial
             * enumeration on a single-USB board has no committed host frame
             * and must not interrupt its normal power-on Logo. */
            if (s_host_output_committed &&
                !usb_osd_logo_backlight_is_owned()) {
                s_host_output_committed = false;
                aic_panel_backlight_disable();
            }
            break;
        case USBD_EVENT_DISCONNECTED:
            /* Do this before queuing mode work: otherwise the retained host
             * frame is visible until the static Logo reaches the LVGL thread. */
            s_host_output_committed = false;
            aic_panel_backlight_disable();
            dtr_enable = 0;
            ep_tx_busy_flag = false;
            s_ota_active = false;
            cdc_out_read_disarm();
            __atomic_add_fetch(&s_usb_session_generation, 1,
                               __ATOMIC_ACQ_REL);
            __atomic_add_fetch(&s_cdc_rx_drop_generation, 1,
                               __ATOMIC_ACQ_REL);
            cdc_rx_queue_message(CDC_RX_MESSAGE_ABORT_LOGO, RT_NULL, 0);
            usb_mode_usb_power_changed(false, false);
            break;
        case USBD_EVENT_RESUME:
            /* The official AIC UDC does not dispatch this event. */
            break;
        case USBD_EVENT_SUSPEND:
            /* Hard gate the retained frame before any asynchronous cleanup. */
            s_host_output_committed = false;
            dtr_enable = 0;
            ep_tx_busy_flag = false;
            /* The host may discard the CDC transfer during a long D3 while
             * retaining Display.  Invalidate only the CDC software session;
             * the Display endpoint/controller are deliberately untouched. */
            cdc_out_read_disarm();
            __atomic_add_fetch(&s_usb_session_generation, 1,
                               __ATOMIC_ACQ_REL);
            __atomic_add_fetch(&s_cdc_rx_drop_generation, 1,
                               __ATOMIC_ACQ_REL);
            /* Filesystem/timer cleanup cannot run in the USB ISR. */
            s_host_reset_abort_logo_save_pending = true;
            usb_mode_post(USB_MODE_EVENT_HOST_RESET);
            /*
             * Match the video-Logo build: USB suspend is the reliable final
             * authority for sleep/hibernate even if Display D3 is missing.
             * The sleep path preserves the Display receive context for wake.
             */
            if (usb_osd_logo_backlight_is_owned()) {
                USB_LOG_INFO("USB mode: ignore enumeration suspend under Logo\r\n");
            } else {
                usb_mode_note_suspend_state(false);
                aic_panel_backlight_disable();
                usb_mode_post(USB_MODE_EVENT_POWER);
            }
            break;
        case USBD_EVENT_CONFIGURED:
            /* CONFIGURED may precede the Logo repaint on hibernate wake. */
            if (!usb_osd_logo_backlight_is_owned())
                aic_panel_backlight_disable();
            usb_mode_usb_power_changed(true, true);
            cdc_out_read_disarm();
            cdc_arm_out_read();
            if (s_display_reenumeration_active) {
                s_display_reenumeration_active = false;
                USB_LOG_INFO("USB mode: Display USB re-enumerated\r\n");
            }
            break;
        case USBD_EVENT_SET_REMOTE_WAKEUP:
            break;
        case USBD_EVENT_CLR_REMOTE_WAKEUP:
            break;
        default:
            break;
    }
}

static void cdc_process_rx_packet(uint8_t *buf, uint32_t nbytes)
{
    /* Any received CDC payload wakes a panel blanked by source inactivity. */
    usb_mode_note_source_activity();

#ifdef AIC_OTA_INTERFACE
    if (handle_ota_packet(buf, nbytes)) {
        return;
    }
#endif

    if (s_logo_save_state == CDC_LOGO_SAVE_RECEIVING) {
        logo_save_data(buf, nbytes);
        return;
    }

    if (is_control_packet(buf, nbytes)) {
        handle_control_packet(buf, nbytes);
    } else {
        /*
         * A manual Display command may be followed by already-queued CDC USB
         * chunks.  Drop them exactly as the former display_cmd_latched path did;
         * otherwise their JPEG EOI races and reverses the requested mode.
         */
        if (!s_display_cmd_latched) {
            /* A complete JPEG, not an arbitrary chunk, decides CDC activity. */
            cdc_image_rx(buf, nbytes);
        }
    }
}

void usbd_cdc_acm_bulk_out(uint8_t ep, uint32_t nbytes)
{
    (void)ep;

    cdc_out_read_disarm();

    if (nbytes == 0) {
        cdc_arm_out_read();
        return;
    }

    if (nbytes > CDC_RX_BUF_SIZE ||
        !cdc_rx_queue_message(CDC_RX_MESSAGE_PACKET,
                              read_buffer, nbytes)) {
        /* A dropped USB chunk invalidates the JPEG currently being assembled. */
        __atomic_add_fetch(&s_cdc_rx_drop_generation, 1,
                           __ATOMIC_ACQ_REL);
    }
    /* rt_mq_send has copied the message; read_buffer can now be reused.
     * The common arm helper pauses if the queue approaches capacity. */
    cdc_arm_out_read();
}

void usbd_cdc_acm_bulk_in(uint8_t ep, uint32_t nbytes)
{
    (void)ep;

    if ((nbytes % CDC_MAX_MPS) == 0 && nbytes) {
        usbd_ep_start_write(runtime_in_ep(), NULL, 0);
    } else {
        ep_tx_busy_flag = false;
        touch_send_complete_callback();    
        touch_bulk();
    }
}

static struct usbd_interface intf0;
static struct usbd_interface intf1;

#ifdef LPKG_CHERRYUSB_DEVICE_COMPOSITE
#include "composite_template.h"
int usbd_comp_acm_init(uint8_t *ep_table, void *data)
{
    (void)data;

    cdc_out_ep.ep_addr = ep_table[1];
    cdc_in_ep.ep_addr = ep_table[2];

    usbd_add_interface(usbd_cdc_acm_init_intf(&intf0));
    usbd_add_interface(usbd_cdc_acm_init_intf(&intf1));
    usbd_add_endpoint(&cdc_out_ep);
    usbd_add_endpoint(&cdc_in_ep);
    return 0;
}
#endif

int cdc_acm_deinit(void)
{
    s_cdc_rx_service_enabled = false;
    cdc_out_read_disarm();
    __atomic_add_fetch(&s_usb_session_generation, 1, __ATOMIC_ACQ_REL);
    if (s_mode_thread && s_mode_event) {
        s_mode_service_enabled = false;
        while (s_mode_stop_sem &&
               rt_sem_take(s_mode_stop_sem, RT_WAITING_NO) == RT_EOK) {
        }
        usb_mode_post(USB_MODE_EVENT_STOP);
        if (!s_mode_stop_sem ||
            rt_sem_take(s_mode_stop_sem,
                        rt_tick_from_millisecond(1000)) != RT_EOK) {
            USB_LOG_ERR("USB mode service stop timeout\r\n");
        }
    }

#ifndef LPKG_CHERRYUSB_DEVICE_COMPOSITE
    usbd_deinitialize();
#else
    usbd_comp_func_release(cdc_descriptor, "cdc_acm");
#endif

    dtr_enable = 0;
    ep_tx_busy_flag = false;
    s_cdc_stream_active = false;
    s_auto_switch_enabled = true;
    s_ota_active = false;
    logo_save_abort();
    cdc_image_processor_set_output_enabled(false);
    cdc_image_processor_set_enabled(false);

    {
        rt_base_t level = rt_hw_interrupt_disable();
        s_host_attached = false;
        s_usb_bus_awake = false;
        s_host_awake = false;
        s_wake_replay_pending = false;
        s_command_target = USB_TARGET_NONE;
        s_display_cmd_latched = false;
        s_cdc_frame_pending = false;
        s_display_frame_pending = false;
        s_resume_target = USB_TARGET_NONE;
        s_transition_target = USB_TARGET_NONE;
        s_runtime_mode = USB_RUNTIME_STATIC_LOGO;
        s_power_generation++;
        s_waiting_first_output = false;
        s_display_frame_baseline = usb_display_recv_frame_count();
        s_display_wake_recovery_eligible = false;
        s_display_wake_recovery_armed = false;
        s_display_wake_recovery_baseline = s_display_frame_baseline;
        s_display_wake_recovery_deadline = 0;
        s_display_wake_reenum_armed = false;
        s_display_wake_reenum_baseline = s_display_frame_baseline;
        s_display_wake_reenum_deadline = 0;
        s_display_reenumeration_active = false;
        s_cdc_resume_hold = false;
        s_cdc_output_restore_pending = false;
        s_wake_logo_armed = false;
        s_wake_logo_active = false;
        s_wake_logo_show_requested = false;
        s_wake_logo_visible = false;
        s_wake_logo_min_elapsed = false;
        s_wake_logo_host_frame_ready = false;
        s_wake_logo_release_pending = false;
        s_wake_logo_hold_deadline = 0;
        s_static_logo_owned = false;
        s_static_logo_visible = false;
        s_static_logo_release_pending = false;
        s_last_source_data_tick = rt_tick_get();
        s_source_idle_backlight_off = false;
        s_source_wake_pending = false;
        s_host_restart_armed = false;
        s_restart_relight_armed = false;
        s_restart_relight_deadline = 0;
        s_cdc_restart_resume_armed = false;
        s_cdc_restart_resume_deadline = 0;
        rt_hw_interrupt_enable(level);
    }

    if (s_display_timer) {
        rt_timer_stop(s_display_timer);
        rt_timer_delete(s_display_timer);
        s_display_timer = RT_NULL;
    }

    if (s_logo_save_timer) {
        rt_timer_stop(s_logo_save_timer);
        rt_timer_delete(s_logo_save_timer);
        s_logo_save_timer = RT_NULL;
    }

    return 0;
}

int cdc_acm_init(void)
{
    if (cdc_rx_service_init() != RT_EOK)
        USB_LOG_ERR("CDC RX service init failed\r\n");

    if (cdc_image_processor_init() != RT_EOK) {
        USB_LOG_ERR("CDC image processor init failed\r\n");
    } else if (!s_decoder_started && cdc_image_decoder_start() == RT_EOK) {
        s_decoder_started = true;
    }

    if (usb_cdc_storage_init() != RT_EOK) {
        USB_LOG_ERR("CDC storage init failed\r\n");
    }

    if (usb_mode_service_init() != RT_EOK) {
        USB_LOG_ERR("USB mode service init failed\r\n");
    }

#ifndef LPKG_CHERRYUSB_DEVICE_COMPOSITE
    usbd_desc_register(cdc_descriptor);
    usbd_add_interface(usbd_cdc_acm_init_intf(&intf0));
    usbd_add_interface(usbd_cdc_acm_init_intf(&intf1));
    usbd_add_endpoint(&cdc_out_ep);
    usbd_add_endpoint(&cdc_in_ep);
    usbd_initialize();
#else
    usbd_comp_func_register(cdc_descriptor,
                            usbd_comp_acm_event_handler,
                            usbd_comp_acm_init, "cdc_acm");
#endif

    if (!s_display_timer) {
        s_display_timer = rt_timer_create("cdc_disp",
                                          display_mode_timer_cb,
                                          RT_NULL,
                                          rt_tick_from_millisecond(CDC_MODE_TIMER_PERIOD_MS),
                                          RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
        if (s_display_timer) {
            rt_timer_start(s_display_timer);
        }
    }

    usb_mode_post(USB_MODE_EVENT_INIT);
    return 0;
}
USB_INIT_APP_EXPORT(cdc_acm_init);

void usbd_cdc_acm_set_dtr(uint8_t intf, bool dtr)
{
    (void)intf;

    dtr_enable = dtr ? 1 : 0;
    ep_tx_busy_flag = false;

    if (!dtr_enable) {
        rt_base_t level = rt_hw_interrupt_disable();
        s_dtr_close_generation = s_cdc_activity_generation;
        s_dtr_close_power_generation = s_power_generation;
        rt_hw_interrupt_enable(level);
        usb_mode_post(USB_MODE_EVENT_DTR_CLOSED);
    }
}
