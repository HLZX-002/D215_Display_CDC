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

/*
 * Panel blanking watchdog: when neither the Display transport nor the CDC
 * stream delivered any payload for this long, the panel is switched off and
 * the next payload switches it back on.  It must not depend on the host
 * attach/awake bookkeeping: with LPKG_CHERRYUSB_DEVICE_PLUG_CHECK disabled a
 * data-cable unplug produces no USBD_EVENT_DISCONNECTED at all, so the mode
 * arbiter would otherwise keep the retained last host frame on screen.
 * Keep the value identical to the static-Logo build so both products behave
 * the same.
 */
#define USB_MODE_SOURCE_IDLE_TIMEOUT_MS 2000
/*
 * A static Windows desktop does not continuously produce Display frames.
 * Treating the absence of new payloads as a disconnected/sleeping host makes
 * the panel go dark after two seconds and a mouse movement appear to wake it.
 * Host sleep/shutdown is already handled by the USB suspend/power callbacks,
 * so do not use frame inactivity as a backlight-off signal.
 */
#define USB_MODE_SOURCE_IDLE_BLANK_ENABLE 0

/*
 * CherryUSB invokes endpoint callbacks directly from the AIC USB ISR.  Keep
 * filesystem, NOR, PWM and decoder work out of that path: besides delaying
 * enumeration, taking a lock already owned by the interrupted video thread
 * deadlocks the whole device.  One queued packet at a time also provides the
 * same USB back-pressure as the old synchronous callback.
 */
#define CDC_RX_QUEUE_DEPTH       16
#define CDC_RX_THREAD_STACK      4096
#define CDC_RX_THREAD_PRIO       20

#define USB_MODE_THREAD_STACK 4096
#define USB_MODE_THREAD_PRIO  9
#define USB_MODE_THREAD_TICK  20

#define USB_MODE_EVENT_INIT           (1U << 0)
#define USB_MODE_EVENT_POWER          (1U << 1)
#define USB_MODE_EVENT_CDC_FRAME      (1U << 2)
#define USB_MODE_EVENT_DISPLAY_FRAME  (1U << 3)
#define USB_MODE_EVENT_LOGO_FINISHED  (1U << 4)
#define USB_MODE_EVENT_LOGO_RELEASED  (1U << 5)
#define USB_MODE_EVENT_CDC_TIMEOUT    (1U << 6)
#define USB_MODE_EVENT_CDC_REQUEST    (1U << 7)
#define USB_MODE_EVENT_DISPLAY_REQUEST (1U << 8)
#define USB_MODE_EVENT_DTR_CLOSED     (1U << 9)
#define USB_MODE_EVENT_STOP           (1U << 10)
#define USB_MODE_EVENT_TRANSITION_RETRY (1U << 11)
#define USB_MODE_EVENT_HOST_RESTART   (1U << 12)
#define USB_MODE_EVENT_OUTPUT_READY   (1U << 13)
#define USB_MODE_EVENT_DISPLAY_RECOVER (1U << 14)
#define USB_MODE_EVENT_DISPLAY_REENUM (1U << 15)
#define USB_MODE_EVENT_SOURCE_ACTIVITY (1U << 16)
#define USB_MODE_EVENT_SOURCE_IDLE (1U << 17)
#define USB_MODE_EVENT_LOGO_HANDOFF (1U << 18)
#define USB_MODE_EVENT_CDC_SESSION_RESET (1U << 19)
#define USB_MODE_EVENT_RESTART_RELIGHT (1U << 20)
#define USB_MODE_EVENT_CDC_RESTART_RESUME (1U << 21)

/*
 * PC restart (reboot) recovery bound.  A reboot only resets the USB bus; the
 * suspend -> resume pair which drives the normal wake arbitration never
 * arrives.  These bounds make sure a restarted host always ends up with a lit
 * panel and a running CDC stream, even when one step of the hand-off (movie
 * finished, first host frame) does not happen.
 */
#define USB_MODE_RESTART_RELIGHT_MS   2000
#define USB_MODE_CDC_RESTART_RESUME_MS 1500

#define USB_MODE_QUIESCE_TIMEOUT_MS   300
#define USB_MODE_RETRY_DELAY_MS       10
#define USB_MODE_OUTPUT_SETTLE_MS     20
/* Do not expose the PC's transient boot-mode frames behind the retained Logo. */
#define USB_MODE_LOGO_DISPLAY_STABLE_FRAMES 6
#define USB_MODE_LOGO_DISPLAY_STABLE_MS     300
/* Keep Display/CDC ownership apart until post-decode frame/CMA cleanup ends. */
#define USB_MODE_MJPEG_IDLE_STABLE_MS 30
#define USB_MODE_DISPLAY_RECOVER_MS   1200
#define USB_MODE_DISPLAY_WAKE_RELOAD_MS 200
#define USB_MODE_DISPLAY_REENUM_MS    1200
#define USB_MODE_REENUM_DISCONNECT_MS 250
/*
 * Upper bound for the seamless Logo hand-off below.  If the host does not
 * deliver a frame in time the release falls back to the previous behaviour
 * (blank the panel until the first frame arrives).
 */
#define USB_MODE_LOGO_HANDOFF_MS     800

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
volatile bool touch_data_enabled = false;
volatile bool s_in_display_mode = true;

static volatile bool s_cdc_stream_active = false;
extern void usb_display_enable(void);
extern void usb_display_disable(void);
/* Balance the official Display resource lifetime across mode transitions. */
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
static volatile bool s_cdc_session_reset_pending = false;
/* Last received CDC payload or completed Display frame. */
static volatile rt_tick_t s_last_source_data_tick = 0;
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
/* USB IRQ is not re-entrant; keep its large queue item off the IRQ stack. */
static struct cdc_rx_message s_cdc_rx_enqueue_message;
static struct cdc_rx_message s_cdc_rx_worker_message;

enum usb_runtime_mode {
    USB_RUNTIME_ADAPTER_LOGO = 0,
    USB_RUNTIME_HOST_LOGO,
    USB_RUNTIME_LOGO_HOLD,
    USB_RUNTIME_LOGO_RELEASING,
    USB_RUNTIME_DISPLAY,
    USB_RUNTIME_CDC,
    USB_RUNTIME_SLEEP,
};

enum usb_transition_target {
    USB_TARGET_NONE = 0,
    USB_TARGET_DISPLAY,
    USB_TARGET_CDC,
};

static volatile enum usb_runtime_mode s_runtime_mode = USB_RUNTIME_ADAPTER_LOGO;
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
static volatile bool s_startup_logo_active = false;
static volatile bool s_startup_logo_done = false;
static volatile bool s_restart_logo_active = false;
static volatile bool s_restart_logo_waiting_host = false;
/* Armed only after the device has entered CDC/Display at least once. */
static volatile bool s_host_restart_armed = false;
/* Keep the panel dark between logo release and the first rendered JPEG. */
static volatile bool s_waiting_first_output = false;
/* Set before any local-video transition, including the decoder quiesce window. */
static volatile bool s_waiting_logo_first_frame = false;
/* Display transport may receive frames while H.264 keeps exclusive VE use. */
static volatile bool s_logo_display_monitor_enabled = false;
/* True while the LVGL local-video object/frame still needs explicit release. */
static volatile bool s_logo_owned = false;
static volatile bool s_host_output_committed = false;

bool usb_osd_logo_backlight_is_owned(void)
{
    bool owned;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    owned = s_logo_owned && !s_waiting_logo_first_frame &&
        (s_runtime_mode == USB_RUNTIME_ADAPTER_LOGO ||
         s_runtime_mode == USB_RUNTIME_HOST_LOGO ||
         s_runtime_mode == USB_RUNTIME_LOGO_HOLD);
    rt_hw_interrupt_enable(level);
    return owned;
}

bool usb_osd_backlight_enable_allowed(void)
{
    bool allowed;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    allowed = s_host_output_committed ||
        (s_logo_owned && !s_waiting_logo_first_frame &&
         (s_runtime_mode == USB_RUNTIME_ADAPTER_LOGO ||
          s_runtime_mode == USB_RUNTIME_HOST_LOGO ||
          s_runtime_mode == USB_RUNTIME_LOGO_HOLD));
    rt_hw_interrupt_enable(level);
    return allowed;
}
/*
 * Seamless Logo hand-off: the local movie reached EOS, so the video engine is
 * free.  Decode one host JPEG while the retained last Logo frame still covers
 * the panel, then swap owners without the dark gap which used to show between
 * "logo released" and "first host frame decoded".
 */
static volatile bool s_logo_hold_handoff_armed = false;
static volatile bool s_logo_handoff_frame_ready = false;
static volatile uint32_t s_logo_handoff_frame_count = 0;
static volatile rt_tick_t s_logo_handoff_first_frame_tick = 0;
static volatile bool s_logo_waiting_natural_frame = false;
static volatile enum usb_transition_target s_logo_hold_target = USB_TARGET_NONE;
static volatile rt_tick_t s_logo_hold_handoff_deadline = 0;
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
/* Suppress the normal PC-restart movie during our short recovery reconnect. */
static volatile bool s_display_reenumeration_active = false;
/*
 * Failsafes for one PC restart.  USB power and enumeration bounce repeatedly
 * during a reboot, so the restart movie can be requested more than once while
 * the host is still gone.  Both deadlines are re-based until the host is
 * configured again and then guarantee, respectively, a lit panel and a running
 * CDC pipeline.
 */
static volatile bool s_restart_relight_armed = false;
static volatile rt_tick_t s_restart_relight_deadline = 0;
static volatile bool s_cdc_restart_resume_armed = false;
static volatile rt_tick_t s_cdc_restart_resume_deadline = 0;
static volatile unsigned int s_logo_epoch = 0;
static volatile unsigned int s_logo_finished_epoch = 0;
static volatile unsigned int s_logo_released_epoch = 0;
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
extern bool cdc_image_processor_wait_idle(uint32_t timeout_ms);
extern void cdc_image_processor_reset_rx(void);
extern void cdc_image_processor_reset_stream(void);
extern void cdc_image_processor_blank_output(void);
extern void cdc_image_processor_clear_frame(void);
extern void cdc_image_processor_ui_layer_enable(void);

extern void usb_display_enable(void);
extern void usb_display_disable(void);
extern uint32_t usb_display_recv_frame_count(void);
extern void usbd_hid_touch_set_params(int params);
extern void aic_panel_backlight_enable(void);
extern void aic_panel_backlight_disable(void);
extern void touch_send_complete_callback(void);
extern void touch_bulk(void);
extern void usb_osd_logo_play_loop(unsigned int epoch);
extern void usb_osd_logo_play_once_current(unsigned int epoch);
extern void usb_osd_logo_replay_once(unsigned int epoch);
extern void usb_osd_logo_suspend(unsigned int epoch);
extern void usb_osd_logo_release(unsigned int epoch);
#ifdef AIC_OTA_INTERFACE
extern bool handle_ota_packet(uint8_t *data, uint32_t length);
#endif

extern void usbd_cdc_acm_bulk_out(uint8_t ep, uint32_t nbytes);
extern void usbd_cdc_acm_bulk_in(uint8_t ep, uint32_t nbytes);

static void cdc_process_rx_packet(uint8_t *buf, uint32_t nbytes);
static void logo_save_abort(void);

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

static void cdc_out_read_disarm(void)
{
    rt_base_t level = rt_hw_interrupt_disable();

    s_cdc_out_read_armed = false;
    rt_hw_interrupt_enable(level);
}

static void cdc_arm_out_read(void)
{
    bool was_armed;
    int ret;
    rt_base_t level;

    if (!s_cdc_rx_service_enabled || !s_usb_bus_awake ||
        !usb_device_is_configured()) {
        return;
    }

    level = rt_hw_interrupt_disable();
    was_armed = s_cdc_out_read_armed;
    if (!was_armed) {
        s_cdc_out_read_armed = true;
    }
    rt_hw_interrupt_enable(level);
    if (was_armed) {
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
                       RT_WAITING_FOREVER) != RT_EOK) {
            continue;
        }

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

        /* Rearm from thread context only after the packet work has finished. */
        cdc_arm_out_read();
    }
}

static int cdc_rx_service_init(void)
{
    if (s_cdc_rx_thread) {
        s_cdc_rx_service_enabled = true;
        return RT_EOK;
    }

    s_cdc_rx_mq = rt_mq_create("cdc_rx",
                               sizeof(struct cdc_rx_message),
                               CDC_RX_QUEUE_DEPTH,
                               RT_IPC_FLAG_FIFO);
    if (!s_cdc_rx_mq) {
        return -RT_ENOMEM;
    }

    s_cdc_rx_thread = rt_thread_create("cdc_rx", cdc_rx_thread_entry,
                                       RT_NULL, CDC_RX_THREAD_STACK,
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
    /*
     * Discard packets queued before a Display->CDC ownership switch.  The
     * trigger frame is only proof that CDC traffic exists; the first frame sent
     * to VE should be assembled after CDC output is enabled again.
     */
    __atomic_add_fetch(&s_usb_session_generation, 1, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&s_cdc_rx_drop_generation, 1, __ATOMIC_ACQ_REL);
    if (s_cdc_rx_mq) {
        rt_mq_control(s_cdc_rx_mq, RT_IPC_CMD_RESET, RT_NULL);
    }
}

static void cdc_note_activity(void)
{
    s_last_cdc_tick = rt_tick_get();
}

static void usb_mode_post(uint32_t event);

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

/*
 * aic_panel_backlight_enable() only enables the PWM channel.  Suspend/Logo
 * hand-off can lose its period/duty registers, so a bare enable can light the
 * panel at the wrong level and look like a flash.  Always re-apply the saved
 * brightness instead - this mirrors the static-Logo build.
 */
static void usb_mode_restore_host_backlight(void)
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
    /*
     * The panel is lit by a legitimate owner now, so the host-restart
     * failsafe has done its job and must not fire on top of it.
     */
    s_restart_relight_armed = false;
    rt_hw_interrupt_enable(level);

    aic_panel_backlight_enable();
    if (!usb_cdc_set_backlight(brightness, false)) {
        USB_LOG_WRN("USB mode: restore backlight %u%% failed\r\n",
                    brightness);
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
        aic_panel_backlight_disable();
    } else {
        usb_mode_restore_host_backlight();
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

static void usb_mode_wait_display_settle(void)
{
    struct mpp_fb *fb = mpp_fb_open();
    int i;

    if (!fb) {
        rt_thread_mdelay(USB_MODE_OUTPUT_SETTLE_MS);
        return;
    }

    /*
     * Removing the Logo video layer and restoring the rotated Display layer
     * are separate DE commits.  Keep PWM dark until several complete scanouts
     * have passed, otherwise the intermediate layer state appears as a short
     * burst of garbage on high-resolution panels.
     */
    for (i = 0; i < 6; i++)
        mpp_fb_ioctl(fb, AICFB_WAIT_FOR_VSYNC, 0);
    mpp_fb_close(fb);
}

static unsigned int usb_mode_next_logo_epoch(void)
{
    rt_base_t level;
    unsigned int epoch;

    level = rt_hw_interrupt_disable();
    s_logo_epoch++;
    if (s_logo_epoch == 0) {
        s_logo_epoch++;
    }
    epoch = s_logo_epoch;
    rt_hw_interrupt_enable(level);
    return epoch;
}

static void usb_mode_disable_sources(bool capture_cdc)
{
    /* Display disable tears down decoder state; do it only after VE is idle. */
    cdc_image_processor_set_output_enabled(false);
    s_logo_display_monitor_enabled = false;
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

/* Keep Display alive as the base layer; CDC owns only the UI overlay. */
static bool usb_mode_prepare_cdc_overlay(uint32_t timeout_ms)
{
    cdc_image_processor_set_output_enabled(false);
    s_logo_display_monitor_enabled = false;
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

static void usb_mode_reset_cdc_session_output(void)
{
    bool resume;
    rt_base_t level;

    cdc_image_processor_set_output_enabled(false);
    if (!cdc_image_processor_wait_idle(USB_MODE_QUIESCE_TIMEOUT_MS)) {
        USB_LOG_WRN("USB mode: CDC decoder busy during session reset\r\n");
    }
    /* Drop the interrupted JPEG and the complete frame of the old session. */
    cdc_image_processor_reset_stream();
    /*
     * Wipe the retained pixels but keep the UI layer enabled: the host is
     * about to stream again, and a disabled layer would freeze the panel on
     * the last visible frame until the cable is pulled.
     */
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

static void usb_mode_enable_logo_display_monitor(void)
{
    uint32_t display_count;
    rt_base_t level;

    if (s_logo_display_monitor_enabled || !s_host_attached ||
        !s_host_awake || !s_logo_owned ||
        (s_runtime_mode != USB_RUNTIME_HOST_LOGO &&
         s_runtime_mode != USB_RUNTIME_LOGO_HOLD)) {
        return;
    }

    /*
     * Set the baseline before enabling reception so the first complete host
     * frame cannot be mistaken for traffic from the previous USB session.
     * MJPEG hardware decode remains gated while the H.264 logo owns VE.
     */
    display_count = usb_display_recv_frame_count();
    level = rt_hw_interrupt_disable();
    s_display_frame_baseline = display_count;
    s_display_frame_pending = false;
    s_logo_display_monitor_enabled = true;
    rt_hw_interrupt_enable(level);
    /* Do not start/restore the Display decoder while H.264 still owns VE and
     * the visible layer.  V1.64's live-under-logo path allowed host frames to
     * be committed before EOS, producing the observed overlapping pictures.
     * The normal post-release Display transition enables it again. */
    USB_LOG_INFO("USB mode: wait for host display signal\r\n");
}

static void usb_mode_enter_adapter_logo(void)
{
    s_waiting_logo_first_frame = true;
    if (!usb_mode_quiesce_mjpeg(false, 200)) {
        usb_mode_set_runtime(USB_RUNTIME_SLEEP);
        aic_panel_backlight_disable();
        USB_LOG_ERR("JPEG decoder busy; defer adapter logo\r\n");
        usb_mode_post(USB_MODE_EVENT_INIT);
        return;
    }
    s_transition_target = USB_TARGET_NONE;
    s_command_target = USB_TARGET_NONE;
    s_display_cmd_latched = false;
    s_startup_logo_active = false;
    s_startup_logo_done = false;
    s_restart_logo_active = false;
    s_restart_logo_waiting_host = false;
    s_host_restart_armed = false;
    s_waiting_first_output = false;
    s_logo_display_monitor_enabled = false;
    s_logo_owned = true;
    usb_mode_set_runtime(USB_RUNTIME_ADAPTER_LOGO);
    /*
     * Adapter-only mode is the product's local playback mode.  Keep the panel
     * dark while the first frame is being prepared; the first-frame callback
     * below is the single point which turns the backlight on.
     */
    aic_panel_backlight_disable();
    usb_osd_logo_play_loop(usb_mode_next_logo_epoch());
    USB_LOG_INFO("USB mode: adapter logo loop\r\n");
}

/*
 * Follow the proven VMAX17 boot sequence: play exactly one complete local
 * video immediately.  USB enumeration/power events may arrive while it is
 * playing, but they must not pause, restart, or extend this startup video.
 * CDC reception remains enabled so the latest complete JPEG can be selected
 * as soon as the local video reaches EOS.
 */
static void usb_mode_start_startup_logo(void)
{
    if (s_startup_logo_active || s_startup_logo_done) {
        return;
    }

    s_waiting_logo_first_frame = true;
    if (!usb_mode_quiesce_mjpeg(true, 200)) {
        USB_LOG_ERR("CDC decoder busy; defer startup logo\r\n");
        usb_mode_post(USB_MODE_EVENT_INIT);
        return;
    }

    s_transition_target = USB_TARGET_NONE;
    s_startup_logo_active = true;
    s_restart_logo_active = false;
    s_restart_logo_waiting_host = false;
    s_host_restart_armed = false;
    s_waiting_first_output = false;
    s_logo_display_monitor_enabled = false;
    s_logo_owned = true;
    usb_mode_set_runtime(USB_RUNTIME_HOST_LOGO);
    aic_panel_backlight_disable();
    /*
     * Device power-on has already started the first local-video iteration.
     * Finish that iteration once instead of seeking to zero and showing it a
     * second time.  A real host restart uses replay_once() below.
     */
    usb_osd_logo_play_once_current(usb_mode_next_logo_epoch());
    USB_LOG_INFO("USB mode: startup logo once\r\n");
}

static void usb_mode_start_restart_logo(void)
{
    if (s_restart_logo_active || s_startup_logo_active) {
        return;
    }

    /* Hide the retained Display frame before waiting for the shared VE. */
    s_waiting_logo_first_frame = true;
    aic_panel_backlight_disable();
    if (!usb_mode_quiesce_mjpeg(true, USB_MODE_QUIESCE_TIMEOUT_MS)) {
        USB_LOG_WRN("JPEG decoder busy; retry host restart logo\r\n");
        rt_thread_mdelay(USB_MODE_RETRY_DELAY_MS);
        usb_mode_post(USB_MODE_EVENT_HOST_RESTART);
        return;
    }
    s_transition_target = USB_TARGET_NONE;
    s_restart_logo_active = true;
    s_restart_logo_waiting_host = false;
    s_waiting_first_output = false;
    s_logo_display_monitor_enabled = false;
    s_logo_owned = true;
    usb_mode_set_runtime(USB_RUNTIME_HOST_LOGO);
    /* A computer restart must replay a complete iteration from frame zero. */
    usb_osd_logo_replay_once(usb_mode_next_logo_epoch());
    USB_LOG_INFO("USB mode: host restart logo once\r\n");
}

static void usb_mode_enter_sleep(void)
{
    bool cdc_idle;

    if (s_runtime_mode == USB_RUNTIME_SLEEP) {
        aic_panel_backlight_disable();
        return;
    }

    /* Close VE immediately, then commit sleep without waiting for an
     * already in-flight frame to finish its harmless cleanup. */
    mpp_decoder_set_mjpeg_decode_enabled(0);
    cdc_rx_drop_queued_packets();
    cdc_image_processor_set_output_enabled(false);
    cdc_image_processor_reset_stream();
    cdc_idle = cdc_image_processor_wait_idle(USB_MODE_QUIESCE_TIMEOUT_MS);
    cdc_image_processor_set_enabled(false);
    s_cdc_stream_active = false;
    if (cdc_idle) {
        cdc_image_processor_blank_output();
    } else {
        USB_LOG_WRN("USB mode: CDC frame still retiring at suspend\r\n");
    }
    /*
     * Resume through the same Display path regardless of the pre-sleep
     * producer.  Do not replay the retained CDC frame; the first new complete
     * CDC frame after wake owns the panel again through normal arbitration.
     */
    s_resume_target = USB_TARGET_DISPLAY;
    s_cdc_resume_hold = false;
    s_cdc_output_restore_pending = false;
    s_cdc_frame_pending = false;
    s_transition_target = USB_TARGET_NONE;
    s_command_target = USB_TARGET_NONE;
    s_display_cmd_latched = false;
    s_waiting_first_output = false;
    s_logo_display_monitor_enabled = false;
    s_host_output_committed = false;
    usb_mode_set_runtime(USB_RUNTIME_SLEEP);
    usb_osd_logo_suspend(usb_mode_next_logo_epoch());
    aic_panel_backlight_disable();
    USB_LOG_INFO("USB mode: host sleep (Display path)\r\n");
}

static void usb_mode_enter_display(void)
{
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
        cdc_image_processor_blank_output();
    }

    /* Capture the counter before reception is enabled; otherwise a very fast
     * first frame can become the new baseline and the resume watchdog misses
     * the only frame which would unblank the panel. */
    display_count = usb_display_recv_frame_count();
    level = rt_hw_interrupt_disable();
    s_cdc_stream_active = false;
    if (!capture_cdc) {
        s_cdc_frame_pending = false;
    }
    s_display_frame_pending = false;
    s_display_frame_baseline = display_count;
    s_transition_target = USB_TARGET_NONE;
    s_resume_target = USB_TARGET_DISPLAY;
    s_cdc_resume_hold = false;
    if (explicit_display && !s_waiting_first_output) {
        /*
         * Outside a Logo hand-off, preserve the existing manual Display-mode
         * behaviour.  During a Logo hand-off s_waiting_first_output is already
         * set, and a command is not proof that a complete scanout frame exists:
         * keep the panel dark until the decoder callback confirms that frame.
         */
        s_waiting_first_output = false;
        s_source_idle_backlight_off = false;
        s_source_wake_pending = false;
        s_logo_handoff_frame_ready = true;
    }
    if (from_cdc && s_host_attached && s_host_awake && !explicit_display) {
        /*
         * Display was disabled while CDC owned the shared MJPEG decoder.  A
         * Windows wake can leave the Display driver without another full
         * frame to submit when the user switches modes.  Require a fresh frame
         * and reuse the same transfer-restart/re-enumeration watchdog used by
         * a Display-mode wake.
         */
        s_waiting_first_output = true;
        s_display_wake_recovery_eligible = false;
        s_display_wake_recovery_baseline = display_count;
        s_display_wake_recovery_deadline =
            rt_tick_get() +
            rt_tick_from_millisecond(USB_MODE_DISPLAY_RECOVER_MS);
        s_display_wake_recovery_armed = true;
        s_display_wake_reenum_armed = false;
    }
    usb_mode_set_runtime(USB_RUNTIME_DISPLAY);
    s_host_restart_armed = true;
    rt_hw_interrupt_enable(level);

    mpp_decoder_set_mjpeg_decode_enabled(1);
    usb_mode_display_enable_owned();
    usbd_hid_touch_set_params(1);

    if (s_waiting_first_output) {
        aic_panel_backlight_disable();
        if (s_logo_handoff_frame_ready) {
            /* Display now owns the layer; wait for stable scanout below. */
            usb_mode_post(USB_MODE_EVENT_OUTPUT_READY);
        }
    } else {
        usb_mode_apply_host_backlight();
    }
    if (from_cdc) {
        USB_LOG_INFO("USB mode: wait for first Display frame after CDC\r\n");
    }
    USB_LOG_INFO("USB mode: display\r\n");
}

/*
 * Display is already receiving underneath the retained Logo.  Once a stable
 * run of complete frames exists, commit that live receiver in place.  Calling
 * usb_mode_enter_display() here would quiesce and restart Display, discarding
 * the exact frame used to validate the hand-off and briefly exposing an
 * intermediate/no-signal scanout.
 */
static void usb_mode_commit_display_after_logo(void)
{
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    s_cdc_stream_active = false;
    s_cdc_frame_pending = false;
    s_display_frame_pending = false;
    s_transition_target = USB_TARGET_NONE;
    s_resume_target = USB_TARGET_DISPLAY;
    s_cdc_resume_hold = false;
    s_waiting_first_output = false;
    s_logo_display_monitor_enabled = false;
    s_logo_handoff_frame_ready = false;
    s_display_wake_recovery_eligible = false;
    s_display_wake_recovery_armed = false;
    s_display_wake_reenum_armed = false;
    s_source_idle_backlight_off = false;
    s_source_wake_pending = false;
    usb_mode_set_runtime(USB_RUNTIME_DISPLAY);
    s_host_restart_armed = true;
    s_last_source_data_tick = rt_tick_get();
    rt_hw_interrupt_enable(level);

    /* Already active; retained as idempotent ownership assertions. */
    mpp_decoder_set_mjpeg_decode_enabled(1);
    usb_mode_display_enable_owned();
    usbd_hid_touch_set_params(1);
    USB_LOG_INFO("USB mode: logo -> live Display (no pipeline restart)\r\n");
}

static void usb_mode_enter_cdc(void)
{
    bool from_display = s_runtime_mode == USB_RUNTIME_DISPLAY;
    rt_base_t level;

    s_transition_target = USB_TARGET_CDC;
    if (!usb_mode_prepare_cdc_overlay(USB_MODE_QUIESCE_TIMEOUT_MS)) {
        USB_LOG_WRN("CDC decoder busy; retry overlay transition\r\n");
        rt_thread_mdelay(USB_MODE_RETRY_DELAY_MS);
        usb_mode_post(USB_MODE_EVENT_TRANSITION_RETRY);
        return;
    }

    if (from_display) {
        cdc_rx_drop_queued_packets();
        cdc_image_processor_reset_stream();
        USB_LOG_INFO("USB mode: drop Display-captured CDC frame\r\n");
    }

    mpp_decoder_set_mjpeg_decode_enabled(1);
    cdc_image_processor_ui_layer_enable();
    cdc_image_processor_set_output_enabled(true);

    level = rt_hw_interrupt_disable();
    s_cdc_stream_active = true;
    s_cdc_frame_pending = false;
    s_display_frame_pending = false;
    s_display_wake_recovery_eligible = false;
    s_display_wake_recovery_armed = false;
    s_display_wake_reenum_armed = false;
    s_transition_target = USB_TARGET_NONE;
    s_resume_target = USB_TARGET_CDC;
    usb_mode_set_runtime(USB_RUNTIME_CDC);
    s_host_restart_armed = true;
    s_last_cdc_tick = rt_tick_get();
    rt_hw_interrupt_enable(level);

    if (!s_waiting_first_output || s_logo_handoff_frame_ready) {
        usb_mode_apply_host_backlight();
    }
    USB_LOG_INFO("USB mode: CDC\r\n");
}

static void usb_mode_restore_cdc_output_after_wake(void)
{
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
    }
    rt_hw_interrupt_enable(level);

    if (!restore) {
        return;
    }

    /*
     * The suspend-exit UI path has just made the Display layer visible again.
     * Re-assert the same output ownership used by a normal CDC transition,
     * without flushing the resumed CDC stream or restarting USB endpoints.
     */
    if (s_source_idle_backlight_off) {
        /*
         * A re-plug: re-enabling output below decodes the frame which is still
         * buffered from before the cable was pulled, and the panel would flash
         * that stale image before the Logo covers it.  Quiesce the decoder and
         * drop the retained frame first.
         */
        cdc_image_processor_set_output_enabled(false);
        if (!cdc_image_processor_wait_idle(USB_MODE_QUIESCE_TIMEOUT_MS)) {
            USB_LOG_WRN("USB mode: CDC decoder busy during wake restore\r\n");
        }
        /*
         * Drop the retained frame and its pixels but keep the UI layer
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

static void usb_mode_release_logo(enum usb_transition_target target)
{
    if (target == USB_TARGET_NONE) {
        return;
    }

    if (s_runtime_mode == USB_RUNTIME_LOGO_RELEASING) {
        if (target == USB_TARGET_CDC) {
            s_transition_target = USB_TARGET_CDC;
        }
        return;
    }

    s_transition_target = target;
    /*
     * The retained H.264 layer is about to disappear.  Do not expose the
     * cleared UI layer: the first successfully decoded host JPEG will turn
     * the backlight on again.
     */
    s_waiting_first_output = s_host_attached && s_host_awake;
    s_waiting_logo_first_frame = false;
    if (s_waiting_first_output) {
        aic_panel_backlight_disable();
    }
    usb_mode_set_runtime(USB_RUNTIME_LOGO_RELEASING);
    usb_osd_logo_release(usb_mode_next_logo_epoch());
}

/*
 * Enter the next owner without a visible interruption.  The local movie is at
 * EOS (LOGO_HOLD), so the video engine may already decode a host JPEG into the
 * UI layer underneath the retained last frame.  Once that frame exists the
 * owner swap only has to drop the video layer - no blanking is required.
 */
static void usb_mode_arm_logo_handoff(enum usb_transition_target target)
{
    /*
     * Only LOGO_HOLD (movie at EOS, video engine idle) may decode a host frame
     * behind the retained last frame.  Everywhere else release immediately so
     * the state machine can never be left waiting for a hand-off.
     */
    if (target != USB_TARGET_DISPLAY && target != USB_TARGET_CDC) {
        usb_mode_release_logo(target);
        return;
    }

    if (s_runtime_mode != USB_RUNTIME_LOGO_HOLD) {
        usb_mode_release_logo(target);
        return;
    }

    if (s_logo_hold_handoff_armed) {
        /* A later source event only refines the pending target. */
        s_logo_hold_target = target;
        return;
    }

    if (s_logo_handoff_frame_ready) {
        /* A frame is already available: swap immediately. */
        usb_mode_release_logo(target);
        return;
    }

    mpp_decoder_set_mjpeg_decode_enabled(1);
    if (target == USB_TARGET_CDC) {
        cdc_image_processor_set_output_enabled(true);
    }

    s_logo_hold_target = target;
    s_logo_hold_handoff_armed = true;
    s_logo_handoff_frame_count = 0;
    s_logo_handoff_first_frame_tick = 0;
    s_logo_hold_handoff_deadline =
        rt_tick_get() +
        rt_tick_from_millisecond(USB_MODE_LOGO_HANDOFF_MS);
    USB_LOG_INFO("USB mode: hold logo frame until first host frame\r\n");
}

static void usb_mode_activate_target(enum usb_transition_target target)
{
    if (target == USB_TARGET_NONE) {
        target = USB_TARGET_DISPLAY;
    }

    /*
     * A suspended/looping local video still owns both its retained layer and
     * the H.264 session.  Never start a JPEG owner until LVGL confirms release.
     */
    if (s_logo_owned) {
        usb_mode_release_logo(target);
    } else if (target == USB_TARGET_CDC) {
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
        case USB_RUNTIME_LOGO_HOLD:
            /*
             * LVGL reports completion only after retaining the final frame for
             * two seconds.  Once the host is awake, enter the selected source;
             * Display is the normal system fallback when no frame arrived.
             */
            if (!explicit_request && s_cdc_resume_hold &&
                s_resume_target == USB_TARGET_CDC) {
                target = USB_TARGET_CDC;
            }
            if (s_host_awake) {
                if (target == USB_TARGET_NONE) {
                    target = USB_TARGET_DISPLAY;
                }
                usb_mode_release_logo(target);
            }
            break;
        case USB_RUNTIME_LOGO_RELEASING:
            if (explicit_request && target != USB_TARGET_NONE) {
                /* The last explicit command always replaces the old target. */
                s_transition_target = target;
            } else if (target == USB_TARGET_CDC) {
                s_transition_target = USB_TARGET_CDC;
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
    if (!awake && (s_runtime_mode == USB_RUNTIME_HOST_LOGO ||
                   s_runtime_mode == USB_RUNTIME_LOGO_HOLD)) {
        /* Re-enable Display reception after the next CONFIGURED transition. */
        s_logo_display_monitor_enabled = false;
    }
    if (!awake && was_awake) {
        s_cdc_resume_hold = false;
        s_cdc_output_restore_pending = false;
        if (s_restart_logo_active || s_restart_logo_waiting_host) {
            /* USB power may bounce repeatedly during one PC restart. */
        } else if (!s_host_attached) {
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

static void usb_mode_usb_power_changed(bool attached, bool bus_awake)
{
    uint32_t display_count = usb_display_recv_frame_count();
    bool cdc_hold_before;
    bool cdc_hold_started;
    bool display_wake_recovery_started = false;
    bool keep_visible_logo;
    rt_base_t level;

    /*
     * RESET/CONNECTED briefly report an attached but not-yet-configured bus
     * during Windows enumeration.  If a committed Logo already owns the
     * panel, this is not a sleep transition: preserve the arbiter's effective
     * power state until CONFIGURED.  Real PC sleep still follows the existing
     * USBD_EVENT_SUSPEND / Display-D3 path below, and Display/CDC operation is
     * therefore unchanged.
     */
    keep_visible_logo = attached && !bus_awake &&
        usb_osd_logo_backlight_is_owned();
    level = rt_hw_interrupt_disable();
    cdc_hold_before = s_cdc_resume_hold;
    s_host_attached = attached;
    if (keep_visible_logo) {
        /* RESET/CONNECTED is not a power transition while a committed Logo
         * owns scanout.  Preserve both inputs to effective-power arbitration;
         * otherwise a later callback observes a synthetic awake->sleep edge. */
        rt_hw_interrupt_enable(level);
        USB_LOG_INFO("USB mode: keep Logo across USB enumeration\r\n");
        return;
    }
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
    }
    usb_mode_update_effective_power_locked(display_count);
    /*
     * A normal USB resume may report CONFIGURED without the Display library's
     * D0 callback.  Arm the same first-frame recovery here as the D0 path so
     * Display cannot remain powered but blank after sleep/hibernate.
     */
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
    cdc_hold_started = !cdc_hold_before && s_cdc_resume_hold;
    rt_hw_interrupt_enable(level);

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
    bool logo_controls_backlight = false;
    bool cdc_restore_pending = false;
    bool wait_for_display = false;
    bool was_awake;
    rt_base_t level;

    USB_LOG_INFO("USB mode: Display suspend %s\r\n",
                 awake ? "exit" : "enter");
    if (!awake && usb_osd_logo_backlight_is_owned()) {
        /*
         * Windows emits a transient D3 while enumerating/rebinding Display.
         * A committed Logo is the visual owner during that window; treating
         * this D3 as real sleep causes Logo -> black -> Logo and resets the
         * receiver which later produces the boot-time garbage frame.
         */
        USB_LOG_INFO("USB mode: ignore transient Display D3 under Logo\r\n");
        return;
    }
    level = rt_hw_interrupt_disable();
    was_awake = s_host_awake;
    rt_hw_interrupt_enable(level);
    usb_mode_note_suspend_state(awake);
    if (awake) {
        /*
         * A retained Display session may have a stale receive context after
         * wake.  Keep the panel dark until either a fresh frame arrives or the
         * short recovery timer performs the same clean reload as a Display
         * mode command.
         */
        level = rt_hw_interrupt_disable();
        wait_for_display = s_display_wake_recovery_armed &&
            s_host_awake &&
            s_runtime_mode == USB_RUNTIME_DISPLAY &&
            s_resume_target == USB_TARGET_DISPLAY;
        logo_controls_backlight = s_logo_owned ||
            s_waiting_logo_first_frame ||
            s_runtime_mode == USB_RUNTIME_ADAPTER_LOGO ||
            s_runtime_mode == USB_RUNTIME_HOST_LOGO ||
            s_runtime_mode == USB_RUNTIME_LOGO_HOLD ||
            s_runtime_mode == USB_RUNTIME_LOGO_RELEASING ||
            s_runtime_mode == USB_RUNTIME_SLEEP;
        cdc_restore_pending = s_runtime_mode == USB_RUNTIME_CDC &&
            s_resume_target == USB_TARGET_CDC &&
            s_cdc_output_restore_pending;
        if (wait_for_display) {
            s_waiting_first_output = true;
        }
        rt_hw_interrupt_enable(level);

        if (wait_for_display) {
            aic_panel_backlight_disable();
        } else if (s_source_idle_backlight_off) {
            /*
             * The Display library can report D0 (and power the framebuffer
             * back on) before the arbiter has seen any USB event for a
             * re-plug.  Without this gate that stale callback lights the
             * retained last host frame for a moment.  Only a real
             * Display/CDC payload may relight the panel.
             */
            aic_panel_backlight_disable();
        } else if (!was_awake && !logo_controls_backlight &&
                   !cdc_restore_pending) {
            /* Logo first-frame/CDC restore callbacks own these transitions. */
            usb_mode_restore_host_backlight();
        }
        /* A normal D3 -> D0 resume does not necessarily reconfigure USB. */
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

/* Decoder dequeue is not a visual commit.  Display Logo hand-off is completed
 * by usb_display_video_layer_committed() after GE/DE programming succeeds. */
void mpp_decoder_mjpeg_frame_ready(void)
{
    usb_mode_note_source_activity();
    if (s_waiting_first_output) {
        usb_mode_post(USB_MODE_EVENT_OUTPUT_READY);
    }
}

/*
 * Called by cdc_image_processor.c after the CDC JPEG has reached the display.
 * The static-Logo build uses this as the CDC counterpart of
 * mpp_decoder_mjpeg_frame_ready(); the H.264 build must do the same or a CDC
 * session can stay dark waiting for an output notification which never comes.
 */
void cdc_image_frame_rendered(void)
{
    /* The restarted CDC session is alive: the failsafe reload is not needed. */
    s_cdc_restart_resume_armed = false;

    if (s_waiting_first_output) {
        usb_mode_post(USB_MODE_EVENT_OUTPUT_READY);
    }
}

/* These callbacks are invoked by the LVGL-thread-owned logo state machine. */
void usb_osd_logo_playback_finished(unsigned int epoch)
{
    if (epoch == s_logo_epoch) {
        s_logo_finished_epoch = epoch;
        usb_mode_post(USB_MODE_EVENT_LOGO_FINISHED);
    }
}

void usb_osd_logo_released(unsigned int epoch)
{
    if (epoch == s_logo_epoch) {
        s_logo_released_epoch = epoch;
        usb_mode_post(USB_MODE_EVENT_LOGO_RELEASED);
    }
}

void usb_osd_logo_first_frame_ready(unsigned int epoch)
{
    /* Epoch zero is the UI's pre-arbiter request and must never light the panel. */
    if (epoch == 0 || epoch != s_logo_epoch) {
        return;
    }

    if (s_runtime_mode == USB_RUNTIME_ADAPTER_LOGO ||
        s_runtime_mode == USB_RUNTIME_HOST_LOGO) {
        s_waiting_logo_first_frame = false;
        /* Keep the V1.64 hand-off point: the Logo player owns PWM until its
         * first frame is committed, independently of host-output state. */
        aic_panel_backlight_enable();
    }
}

static void usb_mode_host_reset_detected(void)
{
    bool request_restart_logo = false;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    if (s_host_restart_armed && s_startup_logo_done &&
        !s_restart_logo_active && !s_restart_logo_waiting_host &&
        (s_runtime_mode == USB_RUNTIME_DISPLAY ||
         s_runtime_mode == USB_RUNTIME_CDC ||
         s_runtime_mode == USB_RUNTIME_SLEEP)) {
        /* Disarm now so multiple Reset notifications produce one movie only. */
        s_host_restart_armed = false;
        request_restart_logo = true;
    }
    rt_hw_interrupt_enable(level);

    if (request_restart_logo) {
        usb_mode_post(USB_MODE_EVENT_HOST_RESTART);
    }
}

static void usb_mode_thread_entry(void *parameter)
{
    (void)parameter;

    while (1) {
        uint32_t events = 0;
        bool awake;
        bool replay;
        bool attached;
        bool logo_restarted = false;
        rt_base_t level;

        if (rt_event_recv(s_mode_event, 0xFFFFFFFFU,
                          RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                          RT_WAITING_FOREVER, &events) != RT_EOK) {
            continue;
        }

        if (events & USB_MODE_EVENT_STOP) {
            s_startup_logo_active = false;
            usb_mode_enter_adapter_logo();
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

        if (events & USB_MODE_EVENT_CDC_SESSION_RESET) {
            bool reset_cdc_session;

            level = rt_hw_interrupt_disable();
            reset_cdc_session = s_cdc_session_reset_pending;
            s_cdc_session_reset_pending = false;
            rt_hw_interrupt_enable(level);

            if (reset_cdc_session) {
                usb_mode_reset_cdc_session_output();
            }
        }

        if ((events & USB_MODE_EVENT_HOST_RESTART) && attached &&
            s_startup_logo_done) {
            usb_mode_start_restart_logo();
        }

        if (events & USB_MODE_EVENT_RESTART_RELIGHT) {
            bool relight;

            level = rt_hw_interrupt_disable();
            /*
             * Only take over once the host source owns the panel again.  While
             * the local movie is still playing it legitimately owns the
             * backlight, so the failsafe keeps waiting.
             */
            relight = s_host_attached && s_host_awake &&
                (s_runtime_mode == USB_RUNTIME_DISPLAY ||
                 s_runtime_mode == USB_RUNTIME_CDC);
            if (relight) {
                s_restart_logo_active = false;
                s_restart_logo_waiting_host = false;
                s_waiting_first_output = false;
                s_waiting_logo_first_frame = false;
                s_source_idle_backlight_off = false;
                s_source_wake_pending = false;
                s_last_source_data_tick = rt_tick_get();
            }
            rt_hw_interrupt_enable(level);

            if (relight) {
                usb_mode_restore_host_backlight();
                USB_LOG_WRN("USB mode: host restart -> force backlight on\r\n");
            }
        }

        if (events & USB_MODE_EVENT_CDC_RESTART_RESUME) {
            /*
             * Failsafe for a restarted PC which never delivered a decodable
             * CDC frame: re-enter CDC once so decoder enable, output enable,
             * UI layer and stream bookkeeping are asserted again.
             */
            if (s_host_attached && s_host_awake &&
                s_runtime_mode == USB_RUNTIME_CDC &&
                !s_logo_owned) {
                USB_LOG_WRN("USB mode: host restart -> reload CDC stream\r\n");
                usb_mode_enter_cdc();
            }
        }

        if (events & USB_MODE_EVENT_INIT) {
            if (attached) {
                usb_mode_start_startup_logo();
            } else {
                usb_mode_enter_adapter_logo();
            }
        }

        if (events & USB_MODE_EVENT_POWER) {
            if (!attached) {
                /*
                 * The CPU can still be alive from the external adapter after
                 * USB is unplugged.  That state must always return to local
                 * loop playback instead of blanking the panel.
                 */
                usb_mode_enter_adapter_logo();
                continue;
            } else if (s_startup_logo_active || s_restart_logo_active ||
                s_runtime_mode == USB_RUNTIME_LOGO_RELEASING) {
                /*
                 * The boot movie owns the screen until its first complete EOS.
                 * Once configured, listen for host Display traffic without
                 * allowing its MJPEG decoder to take VE from the movie.
                 */
                if (awake) {
                    usb_mode_enable_logo_display_monitor();
                }
                level = rt_hw_interrupt_disable();
                s_wake_replay_pending = false;
                rt_hw_interrupt_enable(level);
            } else if (s_runtime_mode == USB_RUNTIME_LOGO_HOLD) {
                if (awake) {
                    enum usb_transition_target target =
                        usb_mode_pending_target();

                    s_restart_logo_waiting_host = false;
                    usb_mode_enable_logo_display_monitor();
                    if (target == USB_TARGET_NONE) {
                        target = USB_TARGET_DISPLAY;
                    }
                    usb_mode_release_logo(target);
                }
                level = rt_hw_interrupt_disable();
                s_wake_replay_pending = false;
                rt_hw_interrupt_enable(level);
            } else if (awake &&
                       s_runtime_mode == USB_RUNTIME_CDC &&
                       s_cdc_output_restore_pending) {
                usb_mode_restore_cdc_output_after_wake();
            } else if (!awake) {
                /*
                 * A newly attached USB host is not "awake" until enumeration
                 * completes.  Replace the adapter loop with the one-shot host
                 * startup video.  Once the boot movie has already completed,
                 * keep the adapter loop alive until CONFIGURED so it can be
                 * explicitly released instead of merely suspended.
                 */
                if (s_runtime_mode == USB_RUNTIME_ADAPTER_LOGO) {
                    if (!s_startup_logo_done) {
                        usb_mode_start_startup_logo();
                        logo_restarted = true;
                    }
                } else {
                    usb_mode_enter_sleep();
                }
                continue;
            } else if (replay || s_runtime_mode == USB_RUNTIME_SLEEP ||
                       s_runtime_mode == USB_RUNTIME_ADAPTER_LOGO) {
                enum usb_transition_target target = s_resume_target;

                /*
                 * This path is reached for Display only after the official D0
                 * callback.  Restore the pre-sleep owner without tearing down
                 * the Display library's pending EP1 receive request.
                 */
                if (s_startup_logo_done &&
                    (replay || s_runtime_mode == USB_RUNTIME_SLEEP)) {
                    /*
                     * A resumed panel must never expose the retained
                     * Display/CDC scanout while Windows is rebuilding its
                     * desktop.  Replay the dynamic Logo from frame zero and
                     * keep it as owner until the first fresh host frame is
                     * ready.  The wire protocol and final target selection
                     * remain unchanged.
                     */
                    usb_mode_start_restart_logo();
                    logo_restarted = true;
                } else if (s_startup_logo_done) {
                    if (target == USB_TARGET_NONE) {
                        target = usb_mode_pending_target();
                    }
                    if (target == USB_TARGET_NONE) {
                        target = USB_TARGET_DISPLAY;
                    }
                    if (target == USB_TARGET_DISPLAY) {
                        s_waiting_first_output = true;
                        aic_panel_backlight_disable();
                    }
                    usb_mode_activate_target(target);
                } else {
                    usb_mode_start_startup_logo();
                    logo_restarted = true;
                }
            }
        }

        if (events & USB_MODE_EVENT_DISPLAY_RECOVER) {
            uint32_t display_count = usb_display_recv_frame_count();
            bool display_path_active;
            bool recover;

            level = rt_hw_interrupt_disable();
            display_path_active =
                s_runtime_mode == USB_RUNTIME_DISPLAY ||
                s_logo_display_monitor_enabled ||
                (s_runtime_mode == USB_RUNTIME_LOGO_RELEASING &&
                 s_transition_target == USB_TARGET_DISPLAY);
            recover = s_host_attached && s_host_awake &&
                !s_logo_owned && s_runtime_mode == USB_RUNTIME_DISPLAY &&
                s_resume_target == USB_TARGET_DISPLAY &&
                s_runtime_mode != USB_RUNTIME_CDC &&
                s_command_target != USB_TARGET_CDC &&
                s_transition_target != USB_TARGET_CDC &&
                !s_cdc_frame_pending &&
                display_path_active &&
                display_count == s_display_wake_recovery_baseline;
            if (recover) {
                s_waiting_first_output = true;
            }
            rt_hw_interrupt_enable(level);

            if (recover) {
                uint32_t reloaded_count;

                USB_LOG_WRN("USB mode: no Display frame after wake/switch; "
                            "reload Display mode\r\n");
                /*
                 * Reuse the proven manual mode-switch path.  Its Display
                 * disable/enable edge resets the layer and receive state which
                 * can remain stale after system sleep.
                 */
                aic_panel_backlight_disable();
                usb_mode_enter_display();

                /*
                 * If the clean reload still produces no frame, retain the
                 * existing USB re-enumeration as a second-level fallback.
                 */
                reloaded_count = usb_display_recv_frame_count();
                level = rt_hw_interrupt_disable();
                if (s_host_attached && s_host_awake &&
                    s_resume_target == USB_TARGET_DISPLAY &&
                    s_runtime_mode == USB_RUNTIME_DISPLAY &&
                    s_command_target != USB_TARGET_CDC &&
                    s_transition_target == USB_TARGET_NONE &&
                    !s_cdc_frame_pending &&
                    s_waiting_first_output) {
                    s_display_frame_baseline = reloaded_count;
                    s_display_frame_pending = false;
                    s_display_wake_reenum_baseline = reloaded_count;
                    s_display_wake_reenum_deadline =
                        rt_tick_get() +
                        rt_tick_from_millisecond(
                            USB_MODE_DISPLAY_REENUM_MS);
                    s_display_wake_reenum_armed = true;
                }
                rt_hw_interrupt_enable(level);
            }
        }

        if (events & USB_MODE_EVENT_DISPLAY_REENUM) {
            uint32_t display_count = usb_display_recv_frame_count();
            bool display_path_active;
            bool reenumerate;

            level = rt_hw_interrupt_disable();
            display_path_active =
                s_runtime_mode == USB_RUNTIME_DISPLAY ||
                s_logo_display_monitor_enabled ||
                (s_runtime_mode == USB_RUNTIME_LOGO_RELEASING &&
                 s_transition_target == USB_TARGET_DISPLAY);
            reenumerate = s_host_attached && s_host_awake &&
                !s_logo_owned && s_runtime_mode == USB_RUNTIME_DISPLAY &&
                s_resume_target == USB_TARGET_DISPLAY &&
                s_runtime_mode != USB_RUNTIME_CDC &&
                s_command_target != USB_TARGET_CDC &&
                s_transition_target != USB_TARGET_CDC &&
                !s_cdc_frame_pending &&
                display_path_active &&
                display_count == s_display_wake_reenum_baseline;
            if (reenumerate) {
                s_display_reenumeration_active = true;
                s_display_wake_recovery_eligible = false;
                s_display_wake_recovery_armed = false;
                s_display_wake_reenum_armed = false;
            }
            rt_hw_interrupt_enable(level);

            if (reenumerate) {
                int ret;

                USB_LOG_WRN("USB mode: Display still has no frame; "
                            "re-enumerate USB\r\n");
                aic_panel_backlight_disable();
                usbd_deinitialize();
                rt_thread_mdelay(USB_MODE_REENUM_DISCONNECT_MS);
                ret = usbd_initialize();
                if (ret) {
                    USB_LOG_ERR("USB mode: USB re-enumeration failed: %d\r\n",
                                ret);
                }
            }
        }

        if ((events & USB_MODE_EVENT_OUTPUT_READY) &&
            s_waiting_first_output && s_host_awake &&
            (s_runtime_mode == USB_RUNTIME_DISPLAY ||
             s_runtime_mode == USB_RUNTIME_CDC)) {
            usb_mode_wait_display_settle();
            if (s_waiting_first_output && s_host_awake &&
                (s_runtime_mode == USB_RUNTIME_DISPLAY ||
                 s_runtime_mode == USB_RUNTIME_CDC)) {
                s_waiting_first_output = false;
                s_logo_handoff_frame_ready = false;
                s_host_output_committed = true;
                usb_mode_restore_host_backlight();
                USB_LOG_INFO("USB mode: first host frame visible\r\n");
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
                (s_runtime_mode == USB_RUNTIME_DISPLAY ||
                 s_runtime_mode == USB_RUNTIME_CDC) &&
                !s_waiting_first_output &&
                !s_waiting_logo_first_frame;
            s_source_wake_pending = false;
            rt_hw_interrupt_enable(level);

            if (wake_backlight) {
                usb_mode_restore_host_backlight();
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
                !s_waiting_logo_first_frame;
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

        if (!awake && attached && !s_startup_logo_active &&
            !s_restart_logo_active &&
            s_runtime_mode != USB_RUNTIME_LOGO_HOLD &&
            s_runtime_mode != USB_RUNTIME_LOGO_RELEASING) {
            continue;
        }

        if ((events & USB_MODE_EVENT_LOGO_HANDOFF) &&
            s_runtime_mode == USB_RUNTIME_LOGO_HOLD) {
            enum usb_transition_target target = s_logo_hold_target;
            s_logo_hold_handoff_armed = false;
            if (target == USB_TARGET_NONE) {
                target = USB_TARGET_DISPLAY;
            }
            if (!s_logo_handoff_frame_ready) {
                /*
                 * Never expose the retained Display buffer here: during boot
                 * it may still be uninitialised or contain only a partial
                 * frame.  Keep the Logo's final frame visible until Windows
                 * naturally submits a complete Display frame.  Toggling the
                 * Display transport here can make its first rotated update an
                 * incomplete/delta frame and caused probabilistic garbling.
                 */
                if (s_host_awake && !s_logo_waiting_natural_frame) {
                    s_logo_waiting_natural_frame = true;
                    s_logo_hold_handoff_armed = true;
                    USB_LOG_INFO("USB mode: retain final logo until a "
                                 "complete host frame arrives\r\n");
                } else {
                    USB_LOG_WRN("USB mode: no host frame behind logo; "
                                "keep final logo frame\r\n");
                }
            }
            if (s_logo_handoff_frame_ready) {
                usb_mode_release_logo(target);
            }
        }

        if ((events & USB_MODE_EVENT_LOGO_FINISHED) && !logo_restarted &&
            s_logo_finished_epoch == s_logo_epoch) {
            if (s_runtime_mode == USB_RUNTIME_HOST_LOGO) {
                enum usb_transition_target target;
                bool restart_logo = s_restart_logo_active;

                if (s_startup_logo_active) {
                    s_startup_logo_active = false;
                    s_startup_logo_done = true;
                }
                if (restart_logo) {
                    s_restart_logo_active = false;
                    s_restart_logo_waiting_host = !s_host_awake;
                }
                usb_mode_set_runtime(USB_RUNTIME_LOGO_HOLD);
                if (s_host_awake) {
                    usb_mode_enable_logo_display_monitor();
                }
                target = usb_mode_pending_target();
                /*
                 * usb_osd_logo_playback_finished() is emitted after the final
                 * frame has already been held for two seconds.  Enter the
                 * selected host source now; Display is the normal system mode
                 * when neither producer supplied a complete frame.
                 */
                if (s_host_awake) {
                    if (target == USB_TARGET_NONE &&
                        s_cdc_resume_hold &&
                        s_resume_target == USB_TARGET_CDC) {
                        target = USB_TARGET_CDC;
                    }
                    if (target == USB_TARGET_NONE) {
                        target = USB_TARGET_DISPLAY;
                    }
                    s_restart_logo_waiting_host = false;
                    usb_mode_release_logo(target);
                } else {
                    USB_LOG_INFO("USB mode: last frame held; wait for host\r\n");
                }
            }
        }

        if (events & (USB_MODE_EVENT_CDC_FRAME |
                      USB_MODE_EVENT_DISPLAY_FRAME |
                      USB_MODE_EVENT_CDC_REQUEST |
                      USB_MODE_EVENT_DISPLAY_REQUEST)) {
            usb_mode_handle_source_event(events);
        }

        if ((events & USB_MODE_EVENT_LOGO_RELEASED) &&
            s_logo_released_epoch == s_logo_epoch &&
            s_runtime_mode == USB_RUNTIME_LOGO_RELEASING) {
            enum usb_transition_target target = usb_mode_pending_target();

            s_logo_owned = false;
            if (!s_host_attached) {
                usb_mode_enter_adapter_logo();
                continue;
            }
            if (!s_host_awake) {
                usb_mode_enter_sleep();
                continue;
            }
            if (target == USB_TARGET_NONE) {
                target = s_transition_target;
            }
            if (target == USB_TARGET_CDC) {
                usb_mode_enter_cdc();
            } else {
                /* V1.64 performs a normal Display-owner transition only after
                 * the Logo layer has been released.  Reusing the live layer
                 * here can expose a host frame while the movie is still being
                 * retired and causes the two pictures to overlap. */
                usb_mode_enter_display();
            }
        }

        if (events & USB_MODE_EVENT_TRANSITION_RETRY) {
            enum usb_transition_target target = s_transition_target;

            if (!s_host_attached) {
                usb_mode_enter_adapter_logo();
            } else if (!s_host_awake) {
                usb_mode_enter_sleep();
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
            close_still_valid = s_host_awake && !dtr_enable &&
                !s_cdc_resume_hold &&
                s_dtr_close_power_generation == s_power_generation &&
                s_dtr_close_generation == s_cdc_activity_generation;
            if (close_still_valid) {
                s_cdc_frame_pending = false;
                s_cdc_resume_hold = false;
            }
            rt_hw_interrupt_enable(level);
            if (close_still_valid && s_runtime_mode == USB_RUNTIME_CDC) {
                USB_LOG_INFO("USB mode: CDC DTR closed -> Display\r\n");
                usb_mode_enter_display();
            } else if (close_still_valid &&
                       s_runtime_mode == USB_RUNTIME_LOGO_RELEASING &&
                       s_transition_target == USB_TARGET_CDC) {
                s_transition_target = USB_TARGET_DISPLAY;
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
    bool display_reenum_due = false;
    bool cdc_timed_out = false;
    bool source_idle_due = false;
    bool logo_handoff_due = false;
    bool restart_relight_due = false;
    bool cdc_restart_resume_due = false;
    rt_base_t level;

    (void)parameter;

    /* Recover a CDC transfer lost by interface-selective suspend without
     * resetting the composite controller or disturbing Display. */
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
            display_count != s_display_wake_recovery_baseline) {
            s_display_wake_recovery_armed = false;
        } else if ((rt_int32_t)(rt_tick_get() -
                               s_display_wake_recovery_deadline) >= 0) {
            s_display_wake_recovery_armed = false;
            display_recover_due = true;
        }
    }
    if (s_display_wake_reenum_armed) {
        if (!s_host_awake ||
            s_resume_target != USB_TARGET_DISPLAY ||
            s_runtime_mode == USB_RUNTIME_CDC ||
            s_command_target == USB_TARGET_CDC ||
            s_transition_target == USB_TARGET_CDC ||
            s_cdc_frame_pending ||
            display_count != s_display_wake_reenum_baseline) {
            s_display_wake_reenum_armed = false;
        } else if ((rt_int32_t)(rt_tick_get() -
                               s_display_wake_reenum_deadline) >= 0) {
            s_display_wake_reenum_armed = false;
            display_reenum_due = true;
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
     * gone" evidence is the absence of Display/CDC payloads.  Matches the
     * static-Logo build.
     */
    if (USB_MODE_SOURCE_IDLE_BLANK_ENABLE &&
        (s_runtime_mode == USB_RUNTIME_DISPLAY ||
         s_runtime_mode == USB_RUNTIME_CDC) &&
        !s_waiting_first_output &&
        !s_waiting_logo_first_frame &&
        !s_source_idle_backlight_off &&
        tick_to_ms(rt_tick_get() - s_last_source_data_tick) >=
            USB_MODE_SOURCE_IDLE_TIMEOUT_MS) {
        s_source_idle_backlight_off = true;
        source_idle_due = true;
    }
    /*
     * Host restart (PC reboot) failsafes.  USB power and enumeration bounce
     * for a while during one reboot, and the local movie legitimately owns the
     * panel until it has played once, so the grace period is re-based until
     * the host is configured and a host source owns the screen again.
     */
    if (s_restart_relight_armed) {
        if (!s_host_attached || !s_host_awake ||
            (s_runtime_mode != USB_RUNTIME_DISPLAY &&
             s_runtime_mode != USB_RUNTIME_CDC)) {
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
        if (s_runtime_mode != USB_RUNTIME_CDC && !s_logo_owned) {
            s_cdc_restart_resume_armed = false;
        } else if (!s_host_attached || !s_host_awake || s_logo_owned) {
            s_cdc_restart_resume_deadline =
                rt_tick_get() +
                rt_tick_from_millisecond(USB_MODE_CDC_RESTART_RESUME_MS);
        } else if ((rt_int32_t)(rt_tick_get() -
                                s_cdc_restart_resume_deadline) >= 0) {
            s_cdc_restart_resume_armed = false;
            cdc_restart_resume_due = true;
        }
    }
    if (s_logo_hold_handoff_armed && !s_logo_waiting_natural_frame) {
        if ((rt_int32_t)(rt_tick_get() -
                         s_logo_hold_handoff_deadline) >= 0) {
            /* Give up on the seamless swap; release with the old timing. */
            s_logo_hold_handoff_armed = false;
            logo_handoff_due = true;
        }
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
    if (display_reenum_due) {
        usb_mode_post(USB_MODE_EVENT_DISPLAY_REENUM);
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
    if (logo_handoff_due) {
        usb_mode_post(USB_MODE_EVENT_LOGO_HANDOFF);
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
            /* Hibernate resumes through USB re-enumeration.  Gate PWM before
             * the retained Display/CDC frame can be scanned; the dynamic
             * Logo first-frame callback restores it at the normal point. */
            if (!usb_osd_logo_backlight_is_owned())
                aic_panel_backlight_disable();
            dtr_enable = 0;
            ep_tx_busy_flag = false;
            s_ota_active = false;
            cdc_out_read_disarm();
            __atomic_add_fetch(&s_usb_session_generation, 1,
                               __ATOMIC_ACQ_REL);
            s_cdc_session_reset_pending = true;
            usb_mode_post(USB_MODE_EVENT_CDC_SESSION_RESET);
            cdc_rx_queue_message(CDC_RX_MESSAGE_ABORT_LOGO, RT_NULL, 0);
            if (!s_display_reenumeration_active) {
                usb_mode_host_reset_detected();
            }
            /* A host/bus reset starts a fresh wake cycle at CONFIGURED. */
            usb_mode_usb_power_changed(true, false);
            break;
        case USBD_EVENT_CONNECTED:
            /* Wait for CONFIGURED before treating a powered cable as a host. */
            cdc_out_read_disarm();
            usb_mode_usb_power_changed(true, false);
            break;
        case USBD_EVENT_DISCONNECTED:
            dtr_enable = 0;
            ep_tx_busy_flag = false;
            s_ota_active = false;
            cdc_out_read_disarm();
            __atomic_add_fetch(&s_usb_session_generation, 1,
                               __ATOMIC_ACQ_REL);
            cdc_rx_queue_message(CDC_RX_MESSAGE_ABORT_LOGO, RT_NULL, 0);
            usb_mode_usb_power_changed(false, false);
            break;
        case USBD_EVENT_RESUME:
            /* The official AIC UDC does not dispatch this event. */
            break;
        case USBD_EVENT_SUSPEND:
            dtr_enable = 0;
            ep_tx_busy_flag = false;
            cdc_out_read_disarm();
            __atomic_add_fetch(&s_usb_session_generation, 1,
                               __ATOMIC_ACQ_REL);
            cdc_rx_queue_message(CDC_RX_MESSAGE_ABORT_LOGO, RT_NULL, 0);
            /*
             * USB SUSPEND is the reliable final authority for PC sleep and
             * hibernate.  The Display D3 callback is occasionally absent or
             * arrives out of order, so it cannot be the only backlight-off
             * path.  Enter the arbiter's sleep state as well: that path keeps
             * the Display EP receive context intact for the following wake.
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
            /* Keep the panel dark until the replayed Logo has a real frame. */
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
    } else if (!s_display_cmd_latched) {
        /* A complete JPEG, not an arbitrary chunk, decides CDC activity. */
        cdc_image_rx(buf, nbytes);
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
        /* Recover JPEG framing after any dropped USB chunk and keep EP alive. */
        __atomic_add_fetch(&s_cdc_rx_drop_generation, 1,
                           __ATOMIC_ACQ_REL);
        cdc_arm_out_read();
    }
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
        s_power_generation++;
        s_startup_logo_active = false;
        s_startup_logo_done = false;
        s_restart_logo_active = false;
        s_restart_logo_waiting_host = false;
        s_host_restart_armed = false;
        s_waiting_first_output = false;
        s_waiting_logo_first_frame = false;
        s_logo_display_monitor_enabled = false;
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
        s_last_source_data_tick = rt_tick_get();
        s_source_idle_backlight_off = false;
        s_source_wake_pending = false;
        s_logo_hold_handoff_armed = false;
        s_logo_handoff_frame_ready = false;
        s_logo_handoff_frame_count = 0;
        s_logo_handoff_first_frame_tick = 0;
        s_logo_waiting_natural_frame = false;
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
    if (cdc_rx_service_init() != RT_EOK) {
        USB_LOG_ERR("CDC RX service init failed\r\n");
    }

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
