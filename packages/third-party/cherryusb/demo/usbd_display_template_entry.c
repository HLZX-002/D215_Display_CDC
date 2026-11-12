/*
 * Copyright (c) 2022-2025, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include "usbd_core.h"
#include "usbd_hid.h"
#include "aic_core.h"
#include "aic_drv.h"
#include "mpp_fb.h"
#include "mpp_mem.h"
#include "frame_allocator.h"
#include "mpp_decoder.h"
#include "mpp_mem.h"
#include "mpp_log.h"
#include "mpp_ge.h"
#include "msh.h"
#include <boot_param.h>

#define USB_DISP_ROTATE_FULLSCREEN_MASK       (0x1<<0)
#define USB_DISP_ROTATE_UI_MASK               (0x1<<1)
#define PIXEL_ENCODE_RGB565     0x00
#define PIXEL_ENCODE_ARGB8888   0x01
#define PIXEL_ENCODE_JPEG       0x10
#define PIXEL_ENCODE_H264       0x11
#define PIXEL_ENCODE_AUTO       0xFF
#define UPG_TYPE_ENTER_AICUPG   0x00
#define UPG_TYPE_OTA_START      0x01
#define UPG_TYPE_IMG_VERSION    0x02

#if defined(LPKG_CHERRYUSB_USB_TRANSFER_AUTO)
uint8_t usbdisp_encode_format = PIXEL_ENCODE_AUTO;
#elif defined(LPKG_CHERRYUSB_USB_TRANSFER_JPEG)
uint8_t usbdisp_encode_format = PIXEL_ENCODE_JPEG;
#elif defined(LPKG_CHERRYUSB_USB_TRANSFER_H264)
uint8_t usbdisp_encode_format = PIXEL_ENCODE_H264;
#elif defined(LPKG_CHERRYUSB_USB_TRANSFER_RGB565)
uint8_t usbdisp_encode_format = PIXEL_ENCODE_RGB565;
#elif defined(LPKG_CHERRYUSB_USB_TRANSFER_ARGB8888)
uint8_t usbdisp_encode_format = PIXEL_ENCODE_ARGB8888;
#endif

#define YUV_FORMAT_420P         0x00
#define YUV_FORMAT_444P         0x01
#ifdef AIC_USB_DISP_HIGH_QUALITY_PIC
uint8_t usbdisp_cap_yuv_format = YUV_FORMAT_420P | YUV_FORMAT_444P;
#else
uint8_t usbdisp_cap_yuv_format = YUV_FORMAT_420P;
#endif
uint8_t usbdisp_cur_yuv_format = YUV_FORMAT_420P;

#ifdef AIC_USB_DISP_JPEG_QUALITY_LOWTHRESH
uint8_t usbdisp_jpeg_quality_lowthresh = AIC_USB_DISP_JPEG_QUALITY_LOWTHRESH;
#else
uint8_t usbdisp_jpeg_quality_lowthresh = 0;
#endif

#ifdef AIC_USB_DISP_INIT_MIRROR_MOD_EN
uint8_t usbdisp_init_extension_mode = 1;
#else
uint8_t usbdisp_init_extension_mode = 0;
#endif

#ifdef AIC_USB_DISP_HOST_SCALE_MOD_EN
uint16_t usbdisp_host_scale_width = AIC_USB_DISP_HOST_SCALE_WIDTH;
uint16_t usbdisp_host_scale_height = AIC_USB_DISP_HOST_SCALE_HEIGHT;
#else
uint16_t usbdisp_host_scale_width = 0;
uint16_t usbdisp_host_scale_height = 0;
#endif

#ifdef LPKG_CHERRYUSB_DEVICE_DISPLAY_FPS
uint8_t fps_print = 1;
#else
uint8_t fps_print = 0;
#endif
#ifdef AIC_SCREEN_UNIFIED_ORIENTATION
/* Runtime SN tells the host its CDC direction; this is local scanout only. */
uint32_t usb_disp_rotate = AIC_FB_ROTATE_DEGREE;
uint32_t usb_fb_rotate = AIC_FB_ROTATE_DEGREE;
uint8_t usb_disp_lv_rotate_en = 0;
uint8_t usb_disp_fb_rotate_en = (AIC_FB_ROTATE_DEGREE != 0);
/* The host sends fullscreen frames in the logical resolution advertised by SN.
 * Only rotate the local UI here; rotating fullscreen data again corrupts the
 * 666x2560 stream before the framebuffer scanout rotation. */
uint8_t usb_disp_rotate_mode = USB_DISP_ROTATE_UI_MASK;
#elif defined(LPKG_USING_LVGL)
#if defined(AIC_FB_ROTATE_EN)
uint32_t usb_disp_rotate = AIC_FB_ROTATE_DEGREE;
uint32_t usb_fb_rotate = AIC_FB_ROTATE_DEGREE;
uint8_t usb_disp_lv_rotate_en = 0;
uint8_t usb_disp_fb_rotate_en = 1;
uint8_t usb_disp_rotate_mode = USB_DISP_ROTATE_UI_MASK;
#elif defined(LV_DISPLAY_ROTATE_EN)
uint32_t usb_disp_rotate = LV_ROTATE_DEGREE;
uint32_t usb_fb_rotate = 0;
uint8_t usb_disp_lv_rotate_en = 1;
uint8_t usb_disp_fb_rotate_en = 0;
uint8_t usb_disp_rotate_mode = USB_DISP_ROTATE_UI_MASK | USB_DISP_ROTATE_FULLSCREEN_MASK;
#else
uint32_t usb_disp_rotate = 0;
uint32_t usb_fb_rotate = 0;
uint8_t usb_disp_lv_rotate_en = 0;
uint8_t usb_disp_fb_rotate_en = 0;
uint8_t usb_disp_rotate_mode = USB_DISP_ROTATE_UI_MASK;
#endif
#else
#if defined(AIC_FB_ROTATE_EN)
uint32_t usb_disp_rotate = AIC_FB_ROTATE_DEGREE;
uint32_t usb_fb_rotate = AIC_FB_ROTATE_DEGREE;
uint8_t usb_disp_fb_rotate_en = 1;
#else
uint32_t usb_disp_rotate = 0;
uint32_t usb_fb_rotate = 0;
uint8_t usb_disp_fb_rotate_en = 0;
#endif
uint8_t usb_disp_lv_rotate_en = 0;
#ifdef AIC_USB_DISP_ROTATION_FULLSCREEN
uint8_t usb_disp_rotate_mode = USB_DISP_ROTATE_FULLSCREEN_MASK;
#else
uint8_t usb_disp_rotate_mode = 0;
#endif
#endif

/*
 * Keep USB Display stopped while the local boot video owns the display/VE.
 * The Display worker itself is initialized before the movie starts; only its
 * frame output remains disabled until the USB mode arbiter transfers ownership.
 */
#if defined(AIC_USB_DISP_DEF_DIS) || \
    (defined(AIC_LVGL_USB_OSD_DEMO) && \
     defined(LV_USB_OSD_LOGO_TYPE_VIDEO))
uint8_t usb_display_en = 0;
#else
uint8_t usb_display_en = 1;
#endif

/*
 * A fixed Display initialization delay races the real video EOS: moving the
 * delay merely moves the visible layer reconfiguration flash to another frame.
 * Initialize the Display layer immediately and let the EOS-driven arbiter
 * decide when it may render.  Static Logo must use the same ordering: a late
 * Display initialization otherwise reconfigures layers under an already lit PNG.
 * Products without the Logo arbiter keep the configured delay.
 */
#if defined(AIC_LVGL_USB_OSD_DEMO) && \
    (defined(LV_USB_OSD_LOGO_TYPE_VIDEO) || defined(LV_USB_OSD_LOGO_TYPE_IMAGE))
uint32_t usb_display_init_delay_ms = 0;
#elif defined(AIC_USB_DISP_INIT_DELAY_MS)
uint32_t usb_display_init_delay_ms = AIC_USB_DISP_INIT_DELAY_MS;
#else
uint32_t usb_display_init_delay_ms = 0;
#endif

static volatile bool s_usb_display_runtime_ready = false;

bool usb_display_runtime_is_ready(void)
{
    return __atomic_load_n(&s_usb_display_runtime_ready,
                           __ATOMIC_ACQUIRE);
}

#ifndef AIC_LVGL_USB_OSD_DEMO
uint8_t usb_disp_free_framebuffer = 1;
#else
uint8_t usb_disp_free_framebuffer = 0;
#endif

#ifdef LPKG_CHERRYUSB_DEVICE_AUDIO
uint8_t usb_large_transfer = 0;
#else
uint8_t usb_large_transfer = 1;
#endif

#ifdef AIC_USB_DISP_ASYNC_MODE
uint8_t usb_disp_async_mode = 1;
#else
uint8_t usb_disp_async_mode = 0;
#endif

#ifdef AIC_USB_DISP_HEARTBEAT
uint8_t usb_disp_heartbeat_en = 1;
uint32_t usb_disp_heartbeat_timeout = 10000; //ms
#else
uint8_t usb_disp_heartbeat_en = 0;
uint32_t usb_disp_heartbeat_timeout = 0;
#endif

uint32_t usb_disp_cal_degree = AIC_USB_DISP_CALIBRATION_DEGREE;

#ifdef AIC_USB_DISP_INIT_CLOSE_BACKLIGHT
uint8_t usb_disp_init_close_backlight = 1;
#else
uint8_t usb_disp_init_close_backlight = 0;
#endif

uint8_t usb_disp_debug_en = 0;

/*
 * ABI prefix exported by libusbd_display_template_c906fd.a.  The library
 * does not publish this type, so access the counter through bytes and memcpy
 * rather than aliasing the library object with a private structure type.
 */
struct usb_display_debug_info_prefix {
    uint8_t display_en;
    uint8_t suspend_state;
    uint8_t rec_running;
    uint8_t power_state;
    uint32_t bulk_out_cnt;
    uint32_t recv_packet_cnt;
    uint32_t re_recv_packet_cnt;
    uint32_t recv_frame_cnt;
};

#define USB_DISPLAY_RECV_FRAME_CNT_OFFSET 16U
_Static_assert(offsetof(struct usb_display_debug_info_prefix, recv_frame_cnt) ==
               USB_DISPLAY_RECV_FRAME_CNT_OFFSET,
               "USB display debug-info ABI layout changed");

extern uint8_t g_display_debug_info[];

uint32_t usb_display_recv_frame_count(void)
{
    uint32_t recv_frame_count;

    memcpy(&recv_frame_count,
           g_display_debug_info + USB_DISPLAY_RECV_FRAME_CNT_OFFSET,
           sizeof(recv_frame_count));
    return recv_frame_count;
}

uint8_t usb_disp_host_power_state = 0;
uint8_t usb_disp_get_host_power_state(void)
{
    /*0 = power off,1= power on */
    return usb_disp_host_power_state;
}

#ifdef AIC_USB_DISP_MONITOR_NAME
char *usb_disp_monitor_name = AIC_USB_DISP_MONITOR_NAME;
#else
char *usb_disp_monitor_name = "USB Display";
#endif

#if defined(RT_USING_PM) && defined(AIC_USING_PM) && defined(AIC_USB_DISP_PM_SUSPEND)
uint8_t usb_pm_suspend_en = 1;
static uint8_t s_pm_suspended = 0;

void aic_panel_backlight_disable(void);
void aic_panel_backlight_enable(void);
#ifdef LPKG_CHERRYUSB_DEVICE_AUDIO
void drv_audio_en_pa(void);
void drv_audio_dis_pa(void);
#endif

void pm_suspend_enter(void)
{
    if (s_pm_suspended)
        return;

    s_pm_suspended = 1;

    aic_panel_backlight_disable();
#ifdef LPKG_CHERRYUSB_DEVICE_AUDIO
    drv_audio_dis_pa();
#endif
}

void pm_suspend_exit(void)
{
    if (!s_pm_suspended)
        return;

#ifdef LPKG_CHERRYUSB_DEVICE_AUDIO
    drv_audio_en_pa();
#endif
    aic_panel_backlight_enable();
    s_pm_suspended = 0;
}
#else
uint8_t usb_pm_suspend_en = 0;
#endif

extern int usb_display_rotate(unsigned int rotate_angle);
extern void usb_display_show_debug();

static void usage_fps(char * program)
{
    printf("\n");
    printf("%s - Turn fps print on/off.\n\n", program);
    printf("Usage: %s <on|off>\n", program);
    printf("For example:\n");
    printf("\t%s on\n", program);
    printf("\t%s off\n", program);
    printf("\n");
}

static int test_usb_fps(int argc, char *argv[])
{
    if (argc != 2)
    {
        usage_fps(argv[0]);
        return -RT_EINVAL;
    }

    if (strcmp(argv[1], "on") == 0) {
        fps_print = 1;
    } else if (strcmp(argv[1], "off") == 0) {
        fps_print = 0;
    } else {
        usage_fps(argv[0]);
        return -RT_EINVAL;
    }

    return 0;
}

static void usage_display_dbg(char * program)
{
    printf("\n");
    printf("%s - Show debug print on/off.\n\n", program);
    printf("Usage: %s <on|off>\n", program);
    printf("For example:\n");
    printf("\t%s on\n", program);
    printf("\t%s off\n", program);
    printf("\n");
}

static int test_usb_display_dbg(int argc, char *argv[])
{
    if (argc != 2) {
        usage_display_dbg(argv[0]);
        return -RT_EINVAL;
    }

    if (strcmp(argv[1], "on") == 0) {
        usb_disp_debug_en = 1;
    } else if (strcmp(argv[1], "off") == 0) {
        usb_disp_debug_en = 0;
    } else if (strcmp(argv[1], "info") == 0) {
        usb_display_show_debug();
    } else {
        usage_display_dbg(argv[0]);
        return -RT_EINVAL;
    }

    return 0;
}

static void usage_display_rotate(char * program)
{
    printf("\n");
    printf("%s - Set usb display rotate 0/90/180/270.\n\n", program);
    printf("Usage: %s <0|90|180|270>\n", program);
    printf("For example:\n");
    printf("\t%s 90\n", program);
    printf("\n");
}

int usb_display_set_rotate(unsigned int rotate_angle)
{
    return usb_display_rotate(rotate_angle);
}

int usb_display_get_rotate()
{
    return usb_disp_rotate;
}

static int test_usb_display_rotate(int argc, char *argv[])
{
    if (argc != 2) {
        usage_display_rotate(argv[0]);
        return -RT_EINVAL;
    }

    if (strcmp(argv[1], "0") == 0) {
        usb_display_set_rotate(0);
    } else if (strcmp(argv[1], "90") == 0) {
        usb_display_set_rotate(90);
    } else if (strcmp(argv[1], "180") == 0) {
        usb_display_set_rotate(180);
    } else if (strcmp(argv[1], "270") == 0) {
        usb_display_set_rotate(270);
    } else {
        usage_display_rotate(argv[0]);
        return -RT_EINVAL;
    }

    return 0;
}

MSH_CMD_EXPORT_ALIAS(test_usb_fps, usb_fps, usb display fps);
MSH_CMD_EXPORT_ALIAS(test_usb_display_dbg, usb_display_dbg, usb display debug info);
MSH_CMD_EXPORT_ALIAS(test_usb_display_rotate, usb_display_rotate, usb display rotate);

RT_WEAK bool usb_osd_backlight_enable_allowed(void)
{
    return true;
}

void aic_panel_backlight_enable(void)
{
    /* D0/authentication and PM callbacks also call this helper.  Respect the
     * Logo/first-frame owner so those asynchronous paths cannot light a
     * retained or empty framebuffer before the static PNG is committed. */
    if (!usb_osd_backlight_enable_allowed())
        return;

#ifdef AIC_PANEL_ENABLE_GPIO
    unsigned int g, p;
    long pin;

    pin = hal_gpio_name2pin(AIC_PANEL_ENABLE_GPIO);

    g = GPIO_GROUP(pin);
    p = GPIO_GROUP_PIN(pin);

    hal_gpio_direction_output(g, p);

#ifndef AIC_PANEL_ENABLE_GPIO_LOW
    hal_gpio_set_output(g, p);
#else
    hal_gpio_clr_output(g, p);
#endif
#endif /* AIC_PANEL_ENABLE_GPIO */

#if defined(KERNEL_RTTHREAD) && defined(AIC_PWM_BACKLIGHT_CHANNEL)
    struct rt_device_pwm *pwm_dev;

    pwm_dev = (struct rt_device_pwm *)rt_device_find("pwm");
    if (pwm_dev)
        rt_pwm_enable(pwm_dev, AIC_PWM_BACKLIGHT_CHANNEL);
#endif
}

void aic_panel_backlight_disable(void)
{
#ifdef AIC_PANEL_ENABLE_GPIO
    unsigned int g, p;
    long pin;

    pin = hal_gpio_name2pin(AIC_PANEL_ENABLE_GPIO);

    g = GPIO_GROUP(pin);
    p = GPIO_GROUP_PIN(pin);

#ifndef AIC_PANEL_ENABLE_GPIO_LOW
    hal_gpio_clr_output(g, p);
#else
    hal_gpio_set_output(g, p);
#endif
#endif /* AIC_PANEL_ENABLE_GPIO */

#if defined(KERNEL_RTTHREAD) && defined(AIC_PWM_BACKLIGHT_CHANNEL)
    struct rt_device_pwm *pwm_dev;

    pwm_dev = (struct rt_device_pwm *)rt_device_find("pwm");
    if (pwm_dev)
        rt_pwm_disable(pwm_dev, AIC_PWM_BACKLIGHT_CHANNEL);
#endif
}

#ifdef LPKG_CHERRYUSB_DEVICE_COMPOSITE
#include "composite_template.h"
void usbd_comp_disp_event_handler(uint8_t event)
#else
void usbd_event_handler(uint8_t event)
#endif
{
    extern void usbd_display_event_handler(uint8_t event);
    usbd_display_event_handler(event);
}

void usb_display_init(void)
{
#ifdef LPKG_CHERRYUSB_DEVICE_COMPOSITE
    extern int usbd_comp_disp_init(uint8_t *ep_table, void *data);
    extern uint8_t graphic_vendor_descriptor[];
    usbd_comp_func_register(graphic_vendor_descriptor,
                            usbd_comp_disp_event_handler,
                            usbd_comp_disp_init, "disp");
#else
    extern void usb_nocomp_display_init(void);
    usb_nocomp_display_init();
#endif
    /*
     * The binary Display worker calls this only after opening/configuring the
     * framebuffer.  The local-video player waits for this edge, preventing the
     * Display layer setup from occurring in the middle of the movie.
     */
    __atomic_store_n(&s_usb_display_runtime_ready, true,
                     __ATOMIC_RELEASE);
}

#if defined(KERNEL_RTTHREAD)
extern int usbd_usb_display_init(void);
extern int usbd_usb_display_deinit(void);
int usbd_display_deinit(void)
{
    __atomic_store_n(&s_usb_display_runtime_ready, false,
                     __ATOMIC_RELEASE);
#ifdef LPKG_CHERRYUSB_DEVICE_COMPOSITE
    extern uint8_t graphic_vendor_descriptor[];
    usbd_comp_func_release(graphic_vendor_descriptor, "disp");
    return 0;
#endif
    usbd_usb_display_deinit();

    return 0;
}

int usbd_display_init(void)
{
    static int init_flg = 0;

    if (init_flg) {
        usb_display_init();
        return 0;
    }
    init_flg = 1;

    usbd_usb_display_init();

    return 0;
}


void disp_vendor_upgrade_get_version(uint8_t **data, uint32_t *len)
{
    struct boot_args *boot_args;

    if (*data == NULL || len == NULL) {
        USB_LOG_ERR("*data or len ptr is null.\n");
        return;
    }

    boot_args = aic_get_boot_args();
    memcpy(*data, boot_args->image_version, strlen(boot_args->image_version));
    *len = strlen(boot_args->image_version) + 1;

    USB_LOG_INFO("get image version: %s, len:%d.\n", *data, *len);
}

int disp_vendor_upgrade_handler_cb(int upg_type, uint8_t **data, uint32_t *len)
{
    switch (upg_type) {
        case UPG_TYPE_ENTER_AICUPG:
            msh_exec("aicupg", strlen("aicupg"));
            break;

        case UPG_TYPE_OTA_START:
            break;

        case UPG_TYPE_IMG_VERSION:
            disp_vendor_upgrade_get_version(data, len);
            break;

        default:
            USB_LOG_ERR("Unknown upgrade type %02x.\n", upg_type);
            return -1;
    }

    return 0;
}

#if defined(LPKG_CHERRYUSB_AIC_DISP_DR)
#include <rtthread.h>
#include <rtdevice.h>
#define COMP_USING_DISP_UAC_TOUCH   1
#define COMP_USING_DISP_MSC         2
rt_thread_t usbd_comp_disp_tid = NULL;
rt_sem_t usbd_comp_disp_sem = NULL;

void usbd_disp_comp_udisk_mode(void *parameter)
{
retry:
    if (usb_device_is_configured() == false) {
        rt_thread_mdelay(500);
        goto retry;
    }

    if (rt_sem_take(usbd_comp_disp_sem, 13000) != -RT_ETIMEOUT)
        goto finish;

    usbd_compsite_device_start(COMP_USING_DISP_MSC);

    while(1) {
        rt_sem_take(usbd_comp_disp_sem, RT_WAITING_FOREVER);
        usbd_compsite_device_start(COMP_USING_DISP_UAC_TOUCH);

        break;
    }

finish:
    if (usbd_comp_disp_sem)
        rt_sem_delete(usbd_comp_disp_sem);

    usbd_comp_disp_sem = NULL;
    usbd_comp_disp_tid = NULL;
}

int disp_configured_event_cb(void)
{
    if (COMP_USING_DISP_MSC == usbd_composite_get_cur_func_table())
        return 0;

    if (usbd_comp_disp_tid == NULL) {
        usbd_comp_disp_sem = rt_sem_create("disp_driver_sem", 0, RT_IPC_FLAG_FIFO);
        if (usbd_comp_disp_sem == RT_NULL) {
            USB_LOG_ERR("create disp semaphore failed.\n");
            return -1;
        }
    }

    if (usbd_comp_disp_tid == NULL) {
        usbd_comp_disp_tid = rt_thread_create("usbd_disp_udsik_setup",
                                    usbd_disp_comp_udisk_mode, RT_NULL,
                                    4096, RT_THREAD_PRIORITY_MAX - 2, 20);
        if (usbd_comp_disp_tid != RT_NULL)
            rt_thread_startup(usbd_comp_disp_tid);
        else
            printf("create usbd_disp_comp_setup thread err!\n");
    }

    return 0;
}

int disp_vendor_handler_cb(void)
{
    if (usbd_comp_disp_sem) {
        rt_sem_release(usbd_comp_disp_sem);
    }

    return 0;
}
#else
USB_INIT_APP_EXPORT(usbd_usb_display_init);
#endif
#endif

#ifdef AIC_USB_DISP_SW_GPIO_EN
void lcd_sw_key_irq_callback(void *args)
{
    rt_base_t pin;
    unsigned int g, p, val;

    pin = rt_pin_get(AIC_USB_DISP_SW_GPIO_NAME);
    if (pin < 0)
        return;

    g = GPIO_GROUP(pin);
    p = GPIO_GROUP_PIN(pin);

    hal_gpio_get_value(g, p, &val);

#if defined(AIC_USB_DISP_SW_GPIO_BACKLIGHT)
    if (val ^ AIC_USB_DISP_SW_GPIO_NAME_POLARITY)
        aic_panel_backlight_enable();
    else
        aic_panel_backlight_disable();
#elif defined(AIC_USB_DISP_SW_GPIO_USBDISPLAY)
    extern void usb_display_enable(void);
    extern void usb_display_disable(void);
    if (val ^ AIC_USB_DISP_SW_GPIO_NAME_POLARITY)
        usb_display_enable();
    else
        usb_display_disable();
#endif
}

int lcd_sw_key_init(void)
{
    rt_base_t pin;
    unsigned int g, p;

    pin = rt_pin_get(AIC_USB_DISP_SW_GPIO_NAME);
    if (pin < 0)
        return -RT_EINVAL;

    g = GPIO_GROUP(pin);
    p = GPIO_GROUP_PIN(pin);
    hal_gpio_set_drive_strength(g, p, 3);
    hal_gpio_set_debounce(g, p, 0xFFF);

    rt_pin_mode(pin, PIN_MODE_INPUT_PULLUP);

    rt_pin_attach_irq(pin, PIN_IRQ_MODE_RISING_FALLING, lcd_sw_key_irq_callback, RT_NULL);
    rt_pin_irq_enable(pin, PIN_IRQ_ENABLE);

    return 0;
}

INIT_APP_EXPORT(lcd_sw_key_init);
#endif
