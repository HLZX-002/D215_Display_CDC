#include <rtconfig.h>
#include "flash_rw.h"
#include "screen_sn_config.h"

#include <fal.h>
#include <rtdevice.h>
#include <rtthread.h>
#include <string.h>
#include <aic_core.h>

#define USB_CDC_USER_PART_NAME      "user"
#define USB_CDC_BRIGHTNESS_OFFSET   USB_CDC_SN_FLASH_LEN
#define USB_CDC_STORAGE_MIN_LEN     (USB_CDC_BRIGHTNESS_OFFSET + 1)
#define USB_CDC_DEFAULT_BRIGHTNESS  100
#define USB_CDC_PWM_PERIOD_NS       1000000
#define USB_CDC_STORAGE_QUEUE_DEPTH 4
#define USB_CDC_STORAGE_THREAD_STK  2048
#define USB_CDC_STORAGE_THREAD_PRIO 20

enum usb_cdc_storage_req_type {
    USB_CDC_STORAGE_REQ_SN,
    USB_CDC_STORAGE_REQ_BRIGHTNESS,
};

struct usb_cdc_storage_req {
    enum usb_cdc_storage_req_type type;
    uint8_t data[USB_CDC_SN_FLASH_LEN];
    uint8_t brightness;
};

static const struct fal_partition *s_user_part = RT_NULL;
static rt_mq_t s_storage_mq = RT_NULL;
static rt_thread_t s_storage_thread = RT_NULL;
static uint8_t s_brightness = USB_CDC_DEFAULT_BRIGHTNESS;
/*
 * CDC OUT callbacks run in the USB interrupt context on this port.  The host
 * asks for the serial number as soon as its application opens the device, so a
 * flash read from that callback can deadlock if the interrupted video reader
 * already owns the NOR/filesystem lock.  Load the value once during startup
 * and serve all runtime requests from RAM.
 */
static uint8_t s_sn_cache[USB_CDC_SN_FLASH_LEN];
static volatile bool s_sn_cache_ready = false;

/* Fail the build if a product mapping no longer matches the CDC protocol. */
typedef char usb_cdc_sn_length_must_be_26[
    (sizeof(USB_CDC_DEFAULT_SN) - 1U == USB_CDC_SN_PAYLOAD_LEN) ? 1 : -1];

static bool user_part_init(void)
{
    if (s_user_part) {
        return true;
    }

    fal_init();
    s_user_part = fal_partition_find(USB_CDC_USER_PART_NAME);
    if (!s_user_part) {
        rt_kprintf("cdc storage: user partition not found\n");
        return false;
    }

    if (s_user_part->len < USB_CDC_STORAGE_MIN_LEN) {
        rt_kprintf("cdc storage: user partition too small: %lu\n",
                   (unsigned long)s_user_part->len);
        s_user_part = RT_NULL;
        return false;
    }

    return true;
}

static bool is_valid_sn_payload(const uint8_t *sn, size_t len)
{
    size_t i;

    if (!sn || len != USB_CDC_SN_PAYLOAD_LEN) {
        return false;
    }

    /*
     * SN describes the CDC image expected by the host and is intentionally
     * independent of the local framebuffer rotation.  Accept any developer
     * supplied direction/resolution which keeps the documented wire format:
     *
     *   VMAX< A-D ><2-digit model><4-digit W>*<4-digit H>S<9-digit batch>
     */
    if (memcmp(sn, "VMAX", 4) != 0 || sn[4] < 'A' || sn[4] > 'D' ||
        sn[11] != '*' || sn[16] != 'S') {
        return false;
    }

    for (i = 5; i < len; i++) {
        if (i == 11 || i == 16) {
            continue;
        }
        if (sn[i] < '0' || sn[i] > '9') {
            return false;
        }
    }

    return true;
}

static void fill_default_sn(char *sn, size_t sn_size)
{
    size_t copy_len;

    if (!sn || sn_size == 0) {
        return;
    }

    memset(sn, 0, sn_size);
    copy_len = strlen(USB_CDC_DEFAULT_SN);
    if (copy_len > USB_CDC_SN_PAYLOAD_LEN) {
        copy_len = USB_CDC_SN_PAYLOAD_LEN;
    }
    if (copy_len > sn_size) {
        copy_len = sn_size;
    }

    memcpy(sn, USB_CDC_DEFAULT_SN, copy_len);
    if (sn_size >= USB_CDC_SN_FLASH_LEN) {
        sn[USB_CDC_SN_FLASH_LEN - 1] = ' ';
    }
}

static void update_sn_cache(const uint8_t *sn)
{
    rt_base_t level;

    if (!sn) {
        return;
    }

    level = rt_hw_interrupt_disable();
    memcpy(s_sn_cache, sn, sizeof(s_sn_cache));
    s_sn_cache_ready = true;
    rt_hw_interrupt_enable(level);
}

static void init_sn_cache(void)
{
    uint8_t sn[USB_CDC_SN_FLASH_LEN];
    int read_len;

    if (s_sn_cache_ready) {
        return;
    }

    /*
     * A provisioned SN in the user partition must take precedence over the
     * firmware default.  Both the production flash tool and the CDC command
     * write the same 26-byte payload at offset 0 (byte 26 is padding).
     *
     * Keep all NOR access here, before USB endpoints are active.  Runtime USB
     * callbacks only read s_sn_cache and therefore never contend for the NOR
     * or filesystem lock.
     */
    memset(sn, 0, sizeof(sn));
    if (user_part_init()) {
        read_len = fal_partition_read(s_user_part, 0, sn, sizeof(sn));
        if (read_len == (int)sizeof(sn) &&
            is_valid_sn_payload(sn, USB_CDC_SN_PAYLOAD_LEN)) {
            /* Normalize the non-protocol padding byte for legacy images and
             * flash tools which only program the 26-byte SN payload. */
            sn[USB_CDC_SN_FLASH_LEN - 1] = ' ';
            update_sn_cache(sn);
            rt_kprintf("cdc storage: loaded SN from user partition: %.*s\n",
                       USB_CDC_SN_PAYLOAD_LEN, sn);
            return;
        }

        if (read_len != (int)sizeof(sn)) {
            rt_kprintf("cdc storage: read SN failed, use firmware default\n");
        } else {
            rt_kprintf("cdc storage: no valid saved SN, use firmware default\n");
        }
    }

    fill_default_sn((char *)sn, sizeof(sn));
    update_sn_cache(sn);
}

static size_t get_user_erase_block_size(void)
{
    const struct fal_flash_dev *flash_dev;

    if (!s_user_part) {
        return 0;
    }

    flash_dev = fal_flash_device_find(s_user_part->flash_name);
    if (!flash_dev || flash_dev->blk_size == 0) {
        return 4096;
    }

    return flash_dev->blk_size;
}

static bool write_user_bytes(uint32_t offset, const uint8_t *data, size_t len)
{
    size_t blk_size;
    uint32_t block_start;
    size_t block_len;
    uint8_t *block_buf;
    uint8_t verify[USB_CDC_SN_FLASH_LEN];
    bool ok = false;

    if (!data || len == 0) {
        return false;
    }

    if (!user_part_init()) {
        return false;
    }

    if ((size_t)offset + len > s_user_part->len) {
        return false;
    }

    blk_size = get_user_erase_block_size();
    if (blk_size == 0) {
        return false;
    }

    block_start = (offset / blk_size) * blk_size;
    block_len = blk_size;
    if ((size_t)block_start + block_len > s_user_part->len) {
        block_len = s_user_part->len - block_start;
    }

    if ((size_t)(offset - block_start) + len > block_len) {
        return false;
    }

    block_buf = (uint8_t *)rt_malloc(block_len);
    if (!block_buf) {
        rt_kprintf("cdc storage: no memory for flash block\n");
        return false;
    }

    if (fal_partition_read(s_user_part, block_start, block_buf, block_len) != (int)block_len) {
        rt_kprintf("cdc storage: read block failed\n");
        goto out;
    }

    memcpy(block_buf + (offset - block_start), data, len);

    if (fal_partition_erase(s_user_part, block_start, block_len) < 0) {
        rt_kprintf("cdc storage: erase block failed\n");
        goto out;
    }

    if (fal_partition_write(s_user_part, block_start, block_buf, block_len) != (int)block_len) {
        rt_kprintf("cdc storage: write block failed\n");
        goto out;
    }

    if (len > sizeof(verify)) {
        goto out;
    }

    if (fal_partition_read(s_user_part, offset, verify, len) != (int)len) {
        rt_kprintf("cdc storage: verify read failed\n");
        goto out;
    }

    ok = (memcmp(verify, data, len) == 0);

out:
    rt_free(block_buf);
    return ok;
}

static bool write_sn_to_flash(const uint8_t *sn)
{
    if (!sn || !is_valid_sn_payload(sn, USB_CDC_SN_PAYLOAD_LEN)) {
        return false;
    }

    return write_user_bytes(0, sn, USB_CDC_SN_FLASH_LEN);
}

static uint8_t read_brightness_from_flash(void)
{
    uint8_t brightness;

    if (!user_part_init()) {
        return USB_CDC_DEFAULT_BRIGHTNESS;
    }

    if (fal_partition_read(s_user_part, USB_CDC_BRIGHTNESS_OFFSET,
                           &brightness, sizeof(brightness)) != (int)sizeof(brightness)) {
        return USB_CDC_DEFAULT_BRIGHTNESS;
    }

    if (brightness == 0xFF || brightness > 100) {
        return USB_CDC_DEFAULT_BRIGHTNESS;
    }

    return brightness;
}

static bool write_brightness_to_flash(uint8_t brightness)
{
    if (brightness > 100) {
        return false;
    }

    return write_user_bytes(USB_CDC_BRIGHTNESS_OFFSET, &brightness, sizeof(brightness));
}

static bool configure_backlight(uint8_t brightness, bool enable)
{
#if defined(RT_USING_PWM) && defined(AIC_PWM_BACKLIGHT_CHANNEL)
    extern bool usb_osd_backlight_enable_allowed(void);
    struct rt_device_pwm *pwm_dev;
    rt_uint32_t pulse;

    /* Brightness restore used to bypass aic_panel_backlight_enable() and
     * enable PWM directly.  Treat a denied enable as a deferred success; the
     * Logo-visible/host-frame event will apply the saved value again. */
    if (enable && !usb_osd_backlight_enable_allowed())
        return true;

    pwm_dev = (struct rt_device_pwm *)rt_device_find("pwm");
    if (!pwm_dev) {
        rt_kprintf("cdc storage: pwm device not found\n");
        return false;
    }

    pulse = (rt_uint32_t)brightness * (USB_CDC_PWM_PERIOD_NS / 100);
    if (rt_pwm_set(pwm_dev, AIC_PWM_BACKLIGHT_CHANNEL, USB_CDC_PWM_PERIOD_NS, pulse) != RT_EOK) {
        rt_kprintf("cdc storage: set pwm failed\n");
        return false;
    }

    if (enable) {
        if (rt_pwm_enable(pwm_dev, AIC_PWM_BACKLIGHT_CHANNEL) != RT_EOK) {
            rt_kprintf("cdc storage: enable pwm failed\n");
            return false;
        }
    } else if (rt_pwm_disable(pwm_dev, AIC_PWM_BACKLIGHT_CHANNEL) != RT_EOK) {
        rt_kprintf("cdc storage: disable pwm failed\n");
        return false;
    }

    return true;
#else
    (void)brightness;
    rt_kprintf("cdc storage: backlight pwm is not enabled\n");
    return false;
#endif
}

static bool apply_backlight(uint8_t brightness)
{
    return configure_backlight(brightness, true);
}

static bool do_storage_request(const struct usb_cdc_storage_req *req)
{
    if (!req) {
        return false;
    }

    if (req->type == USB_CDC_STORAGE_REQ_SN) {
        return write_sn_to_flash(req->data);
    }

    if (req->type == USB_CDC_STORAGE_REQ_BRIGHTNESS) {
        return write_brightness_to_flash(req->brightness);
    }

    return false;
}

static void storage_thread_entry(void *parameter)
{
    struct usb_cdc_storage_req req;

    (void)parameter;

    while (1) {
        if (rt_mq_recv(s_storage_mq, &req, sizeof(req), RT_WAITING_FOREVER) == RT_EOK) {
            do_storage_request(&req);
        }
    }
}

static bool queue_storage_request(const struct usb_cdc_storage_req *req)
{
    if (!req) {
        return false;
    }

    if (!user_part_init()) {
        return false;
    }

    if (!s_storage_mq) {
        return do_storage_request(req);
    }

    return rt_mq_send(s_storage_mq, req, sizeof(*req)) == RT_EOK;
}

int usb_cdc_storage_init(void)
{
    if (!s_storage_mq) {
        s_storage_mq = rt_mq_create("cdcstore",
                                    sizeof(struct usb_cdc_storage_req),
                                    USB_CDC_STORAGE_QUEUE_DEPTH,
                                    RT_IPC_FLAG_FIFO);
        if (!s_storage_mq) {
            rt_kprintf("cdc storage: create mq failed\n");
        }
    }

    if (s_storage_mq && !s_storage_thread) {
        s_storage_thread = rt_thread_create("cdcstore",
                                            storage_thread_entry,
                                            RT_NULL,
                                            USB_CDC_STORAGE_THREAD_STK,
                                            USB_CDC_STORAGE_THREAD_PRIO,
                                            20);
        if (s_storage_thread) {
            rt_thread_startup(s_storage_thread);
        } else {
            rt_kprintf("cdc storage: create thread failed\n");
            rt_mq_delete(s_storage_mq);
            s_storage_mq = RT_NULL;
        }
    }

    /* All NOR access is completed before USB endpoints can receive traffic. */
    init_sn_cache();
    s_brightness = read_brightness_from_flash();
    /*
     * Program the saved duty cycle, but keep PWM disabled.  Both Logo modes
     * own the panel during boot/wake and enable the backlight only after their
     * first complete frame has been committed.  Enabling it here races LVGL/
     * the Logo player and exposes an old or uninitialized framebuffer.
     */
    configure_backlight(s_brightness, false);

    return user_part_init() ? RT_EOK : -RT_ERROR;
}

size_t usb_cdc_load_sn(char *sn, size_t sn_size)
{
    bool cache_ready;
    rt_base_t level;

    if (!sn || sn_size < USB_CDC_SN_FLASH_LEN) {
        return 0;
    }

    level = rt_hw_interrupt_disable();
    cache_ready = s_sn_cache_ready;
    if (cache_ready) {
        memcpy(sn, s_sn_cache, USB_CDC_SN_FLASH_LEN);
    }
    rt_hw_interrupt_enable(level);

    /* Never fall back to a flash read here: callers may be in the USB ISR. */
    if (!cache_ready) {
        fill_default_sn(sn, sn_size);
    }
    return USB_CDC_SN_FLASH_LEN;
}

bool usb_cdc_queue_sn_update(const uint8_t *packet, uint32_t len)
{
    struct usb_cdc_storage_req req;
    bool queued;

    if (!packet || len != (USB_CDC_SN_PAYLOAD_LEN + 2)) {
        return false;
    }

    if (packet[0] != 0xA1 || packet[USB_CDC_SN_PAYLOAD_LEN + 1] != 0xB2) {
        return false;
    }

    if (!is_valid_sn_payload(&packet[1], USB_CDC_SN_PAYLOAD_LEN)) {
        return false;
    }

    memset(&req, 0, sizeof(req));
    req.type = USB_CDC_STORAGE_REQ_SN;
    memcpy(req.data, &packet[1], USB_CDC_SN_PAYLOAD_LEN);
    req.data[USB_CDC_SN_FLASH_LEN - 1] = ' ';

    queued = queue_storage_request(&req);
    if (queued) {
        update_sn_cache(req.data);
    }

    return queued;
}

bool usb_cdc_set_backlight(uint8_t brightness, bool save)
{
    struct usb_cdc_storage_req req;

    if (brightness > 100) {
        return false;
    }

    if (!apply_backlight(brightness)) {
        return false;
    }

    s_brightness = brightness;
    if (!save) {
        return true;
    }

    memset(&req, 0, sizeof(req));
    req.type = USB_CDC_STORAGE_REQ_BRIGHTNESS;
    req.brightness = brightness;

    return queue_storage_request(&req);
}

uint8_t usb_cdc_get_backlight(void)
{
    return s_brightness;
}

/*
 * CherryUSB uses this weak hook for the USB device iSerialNumber descriptor.
 * The storage API includes one trailing padding byte, while a USB string
 * descriptor must expose only the 26-character SN payload.
 */
int usbd_custom_serial_number(char *str)
{
    size_t len;

    if (!str) {
        return 0;
    }

    len = usb_cdc_load_sn(str, USB_CDC_SN_FLASH_LEN);
    if (len > USB_CDC_SN_PAYLOAD_LEN) {
        len = USB_CDC_SN_PAYLOAD_LEN;
    }

    return (int)len;
}
