/*
 * Minimal CDC JPEG receiver and decoder for the rt-thread helloworld app.
 */
#include <rtthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <aic_core.h>
#include <aic_common.h>
#include <artinchip_fb.h>
#include <mpp_fb.h>

#ifndef CDC_IMAGE_MIN_SIZE
#define CDC_IMAGE_MIN_SIZE (1024U * 1024U)
#endif

#ifndef CDC_IMAGE_LARGE_SIZE
#define CDC_IMAGE_LARGE_SIZE (2U * 1024U * 1024U)
#endif

/*
 * These buffers contain the compressed JPEG stream, not an uncompressed LCD
 * frame.  Sizing each one as width * height * 2 permanently consumed almost
 * 8 MiB of the 48 MiB CMA pool on the 2560x666 panel.  After Display resumed,
 * that left too little contiguous CMA for the MJPEG decoder and pm_create()
 * failed even for a 130 KiB request.  The 9.16-inch profile did not cross that
 * limit because its Display/GE buffers are smaller.
 *
 * Keep the original 1 MiB capacity for the reference products and use a 2 MiB
 * compressed-frame ceiling for large panels.  This preserves double buffering
 * and the wire protocol while returning about 3.9 MiB of CMA on 2560x666.
 */
#ifndef CDC_IMAGE_MAX_SIZE
#if defined(AIC_SCREEN_LOGICAL_WIDTH) && defined(AIC_SCREEN_LOGICAL_HEIGHT)
#define CDC_IMAGE_SCREEN_PIXELS \
    (AIC_SCREEN_LOGICAL_WIDTH * AIC_SCREEN_LOGICAL_HEIGHT)
#if CDC_IMAGE_SCREEN_PIXELS > (1920U * 462U)
#define CDC_IMAGE_MAX_SIZE CDC_IMAGE_LARGE_SIZE
#else
#define CDC_IMAGE_MAX_SIZE CDC_IMAGE_MIN_SIZE
#endif
#else
#define CDC_IMAGE_MAX_SIZE CDC_IMAGE_MIN_SIZE
#endif
#endif

#ifndef CDC_IMAGE_MAX_DIMENSION
#define CDC_IMAGE_MAX_DIMENSION 4096U
#endif

#define CDC_IMAGE_BUF_NUM 2
#define CDC_IMAGE_THREAD_STACK 8192
#define CDC_IMAGE_THREAD_PRIO 10
#define CDC_IMAGE_THREAD_TICK 20

extern int decode_pic(uint8_t *pic, int len, u32 offset_x, u32 offset_y,
                      u32 width, u32 height, u32 layer_id);
extern int decode_pic_cdc_persistent(uint8_t *pic, int len, u32 layer_id);

static uint8_t *s_img_buf[CDC_IMAGE_BUF_NUM];
static uint32_t s_img_size[CDC_IMAGE_BUF_NUM];
static rt_sem_t s_frame_sem = RT_NULL;
static rt_thread_t s_decoder_tid = RT_NULL;

static volatile int s_rx_idx = 0;
static volatile int s_ready_idx = -1;
static volatile int s_decoding_idx = -1;
/*
 * A self-powered board can keep DDR and the decoder thread alive while the
 * host tears down and recreates only the USB session.  Tag every completed
 * JPEG with the receiver generation so a pre-suspend frame can never be
 * decoded or reported as visible in the next host session.
 */
static volatile uint32_t s_stream_generation = 1;
static uint32_t s_ready_generation = 0;
static volatile bool s_enabled = false;
/*
 * Receiving and displaying are deliberately controlled separately.  While the
 * local H.264 logo owns the video engine, CDC is still allowed to assemble the
 * newest JPEG frame, but the decoder must not touch the shared hardware.
 */
static volatile bool s_output_enabled = false;
static bool s_receiving = false;
static bool s_drop_frame = false;
static uint8_t s_soi_state = 0;
static uint8_t s_prev_byte = 0;
static uint32_t s_frame_len = 0;
static bool s_initialized = false;

static uint16_t cdc_image_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static bool cdc_image_jpeg_sane(const uint8_t *data, uint32_t len)
{
    uint32_t pos = 2;
    bool saw_sof = false;

    if (!data || len < 8) {
        return false;
    }
    if (data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }
    if (data[len - 2] != 0xFF || data[len - 1] != 0xD9) {
        return false;
    }

    while (pos + 1 < len) {
        uint8_t marker;
        uint16_t seg_len;

        while (pos < len && data[pos] != 0xFF) {
            pos++;
        }
        if (pos + 1 >= len) {
            return false;
        }
        while (pos < len && data[pos] == 0xFF) {
            pos++;
        }
        if (pos >= len) {
            return false;
        }

        marker = data[pos++];
        if (marker == 0x00) {
            continue;
        }
        if (marker == 0xD9) {
            return saw_sof;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        if (pos + 2 > len) {
            return false;
        }

        seg_len = cdc_image_be16(&data[pos]);
        if (seg_len < 2 || pos + seg_len > len) {
            return false;
        }

        if (marker == 0xC2) {
            return false;
        }
        if (marker == 0xC0 || marker == 0xC1) {
            uint16_t height;
            uint16_t width;

            if (seg_len < 8) {
                return false;
            }
            height = cdc_image_be16(&data[pos + 3]);
            width = cdc_image_be16(&data[pos + 5]);
            if (height == 0 || width == 0 ||
                height > CDC_IMAGE_MAX_DIMENSION ||
                width > CDC_IMAGE_MAX_DIMENSION) {
                return false;
            }
            saw_sof = true;
        } else if (marker == 0xDA) {
            uint8_t comps;
            uint32_t min_len;

            if (!saw_sof || seg_len < 6) {
                return false;
            }
            comps = data[pos + 2];
            min_len = 2U + 1U + (uint32_t)comps * 2U + 3U;
            if (comps == 0 || seg_len < min_len) {
                return false;
            }
            return pos + seg_len < len - 2;
        }

        pos += seg_len;
    }

    return false;
}

/* The USB mode arbiter may override this to latch a complete CDC JPEG frame. */
__attribute__((weak)) void cdc_image_frame_received(void)
{
}

/* The mode arbiter may use this to release a Logo after the frame is on screen. */
__attribute__((weak)) void cdc_image_frame_rendered(void)
{
}

static size_t cdc_image_fb_total_size(const struct aicfb_screeninfo *info)
{
    size_t total;

    if (!info || !info->framebuffer || !info->smem_len) {
        return 0;
    }

    total = info->smem_len;
#ifdef AIC_PAN_DISPLAY
    total += info->smem_len;
#endif
#ifdef AIC_FB_ROTATE_EN
    total += info->smem_len * 2;
#endif

    return total;
}

static void cdc_image_set_ui_overlay_alpha(struct mpp_fb *fb, bool opaque)
{
    struct aicfb_alpha_config alpha = {0};

    if (!fb) {
        return;
    }

    alpha.layer_id = AICFB_LAYER_TYPE_UI;
    alpha.enable = 1;
    alpha.mode = opaque ? AICFB_GLOBAL_ALPHA_MODE : AICFB_PIXEL_ALPHA_MODE;
    alpha.value = opaque ? 0xFF : 0;
    mpp_fb_ioctl(fb, AICFB_UPDATE_ALPHA_CONFIG, &alpha);
}

void cdc_image_processor_blank_output(void)
{
    struct aicfb_screeninfo info = {0};
    struct aicfb_layer_data layer = {0};
    struct mpp_fb *fb;
    size_t total;

    if (!s_initialized) {
        return;
    }

    fb = mpp_fb_open();
    if (!fb) {
        return;
    }

    if (mpp_fb_ioctl(fb, AICFB_GET_SCREENINFO, &info) == 0) {
        total = cdc_image_fb_total_size(&info);
        if (total) {
            memset(info.framebuffer, 0, total);
            aicos_dcache_clean_range((unsigned long *)info.framebuffer,
                                     total);
        }
    }

    layer.layer_id = AICFB_LAYER_TYPE_UI;
    layer.rect_id = 0;
    if (mpp_fb_ioctl(fb, AICFB_GET_LAYER_CONFIG, &layer) == 0) {
        layer.enable = 0;
        if (mpp_fb_ioctl(fb, AICFB_UPDATE_LAYER_CONFIG, &layer) == 0) {
            mpp_fb_ioctl(fb, AICFB_WAIT_FOR_VSYNC, 0);
        }
    }

    /* LVGL/static Logo uses per-pixel alpha when it owns the UI layer. */
    cdc_image_set_ui_overlay_alpha(fb, false);

    mpp_fb_close(fb);
}

/*
 * Drop the pixels of the last displayed frame without disabling the UI layer.
 *
 * cdc_image_processor_blank_output() additionally clears layer.enable, and
 * nothing on the CDC decode path ever sets it back to 1: decode_pic() only
 * performs a GE bitblt plus a pan-display.  Using the full blank on a path
 * which is supposed to keep streaming therefore left the panel frozen on the
 * frame which happened to be on screen (typically the first frame after a
 * computer restart) until a physical re-plug re-initialised the UI layer.
 * This helper is the version for those paths: wipe the stale image, keep the
 * layer enabled so the next decoded JPEG is visible immediately.
 */
void cdc_image_processor_clear_frame(void)
{
    struct aicfb_screeninfo info = {0};
    struct mpp_fb *fb;
    size_t total;

    if (!s_initialized) {
        return;
    }

    fb = mpp_fb_open();
    if (!fb) {
        return;
    }

    if (mpp_fb_ioctl(fb, AICFB_GET_SCREENINFO, &info) == 0) {
        total = cdc_image_fb_total_size(&info);
        if (total) {
            memset(info.framebuffer, 0, total);
            aicos_dcache_clean_range((unsigned long *)info.framebuffer,
                                     total);
        }
    }

    mpp_fb_close(fb);
}

/*
 * Guarantee the UI layer is visible again before CDC output is re-enabled.
 * No-op while the layer is already enabled.
 */
void cdc_image_processor_ui_layer_enable(void)
{
    struct aicfb_layer_data layer = {0};
    struct mpp_fb *fb;

    if (!s_initialized) {
        return;
    }

    fb = mpp_fb_open();
    if (!fb) {
        return;
    }

    layer.layer_id = AICFB_LAYER_TYPE_UI;
    layer.rect_id = 0;
    if (mpp_fb_ioctl(fb, AICFB_GET_LAYER_CONFIG, &layer) == 0) {
        /* CDC JPEG output is RGB without a meaningful per-pixel alpha byte. */
        cdc_image_set_ui_overlay_alpha(fb, true);
        if (!layer.enable) {
            layer.enable = 1;
            if (mpp_fb_ioctl(fb, AICFB_UPDATE_LAYER_CONFIG, &layer) == 0) {
                mpp_fb_ioctl(fb, AICFB_WAIT_FOR_VSYNC, 0);
            }
        }
    }

    mpp_fb_close(fb);
}

static int choose_rx_buffer(void)
{
    int idx = -1;
    int i;
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    for (i = 0; i < CDC_IMAGE_BUF_NUM; i++) {
        int candidate = (s_rx_idx + i) % CDC_IMAGE_BUF_NUM;

        if (candidate != s_decoding_idx && candidate != s_ready_idx) {
            idx = candidate;
            s_rx_idx = idx;
            break;
        }
    }
    rt_hw_interrupt_enable(level);

    return idx;
}

static void publish_frame(int idx, uint32_t len)
{
    rt_base_t level;

    if (!s_enabled) {
        return;
    }

    level = rt_hw_interrupt_disable();
    s_img_size[idx] = len;
    s_ready_generation = s_stream_generation;
    s_ready_idx = idx;
    s_rx_idx = (idx + 1) % CDC_IMAGE_BUF_NUM;
    if (s_rx_idx == s_decoding_idx) {
        s_rx_idx = (s_rx_idx + 1) % CDC_IMAGE_BUF_NUM;
    }
    rt_hw_interrupt_enable(level);

    rt_sem_release(s_frame_sem);
    cdc_image_frame_received();
}

int cdc_image_processor_init(void)
{
    int i;

    if (s_initialized) {
        return RT_EOK;
    }

    for (i = 0; i < CDC_IMAGE_BUF_NUM; i++) {
        s_img_buf[i] = aicos_malloc_align(MEM_CMA, CDC_IMAGE_MAX_SIZE, CACHE_LINE_SIZE);
        if (!s_img_buf[i]) {
            while (i-- > 0) {
                aicos_free_align(MEM_CMA, s_img_buf[i]);
                s_img_buf[i] = RT_NULL;
            }
            return -RT_ENOMEM;
        }
        memset(s_img_buf[i], 0, CDC_IMAGE_MAX_SIZE);
    }

    s_frame_sem = rt_sem_create("cdc_img", 0, RT_IPC_FLAG_PRIO);
    if (!s_frame_sem) {
        for (i = 0; i < CDC_IMAGE_BUF_NUM; i++) {
            aicos_free_align(MEM_CMA, s_img_buf[i]);
            s_img_buf[i] = RT_NULL;
        }
        return -RT_ENOMEM;
    }

    s_initialized = true;
    rt_kprintf("cdc image max size %u\n", (unsigned int)CDC_IMAGE_MAX_SIZE);
    return RT_EOK;
}

void cdc_image_processor_set_enabled(bool enabled)
{
    int i;
    rt_base_t level;

    if (!s_initialized) {
        return;
    }

    level = rt_hw_interrupt_disable();
    if (s_enabled == enabled) {
        rt_hw_interrupt_enable(level);
        return;
    }
    s_enabled = enabled;
    s_receiving = false;
    s_drop_frame = false;
    s_soi_state = 0;
    s_prev_byte = 0;
    s_frame_len = 0;
    if (!enabled) {
        s_output_enabled = false;
        s_ready_idx = -1;
        s_rx_idx = 0;
        for (i = 0; i < CDC_IMAGE_BUF_NUM; i++) {
            s_img_size[i] = 0;
        }
    }
    rt_hw_interrupt_enable(level);

    if (!enabled) {
        while (s_frame_sem && rt_sem_take(s_frame_sem, RT_WAITING_NO) == RT_EOK) {
        }
    }
}

void cdc_image_processor_set_output_enabled(bool enabled)
{
    bool wake_decoder = false;
    rt_base_t level;

    if (!s_initialized) {
        return;
    }

    level = rt_hw_interrupt_disable();
    enabled = enabled && s_enabled;
    if (!s_output_enabled && enabled && s_ready_idx >= 0) {
        wake_decoder = true;
    }
    s_output_enabled = enabled;
    rt_hw_interrupt_enable(level);

    /* Decode the latest complete frame immediately after the logo releases VE. */
    if (wake_decoder && s_frame_sem) {
        rt_sem_release(s_frame_sem);
    }
}

void cdc_image_processor_reset_rx(void)
{
    rt_base_t level;

    if (!s_initialized) {
        return;
    }

    level = rt_hw_interrupt_disable();
    s_receiving = false;
    s_drop_frame = false;
    s_soi_state = 0;
    s_prev_byte = 0;
    s_frame_len = 0;
    rt_hw_interrupt_enable(level);
}

/*
 * USB suspend can split one JPEG between the pre-suspend and post-resume USB
 * sessions.  Drop that incomplete/stale frame before CDC output is restored;
 * otherwise the decoder repeatedly starts from a damaged stream until a manual
 * Display -> CDC switch happens to flush these buffers.
 */
void cdc_image_processor_reset_stream(void)
{
    int i;
    rt_base_t level;

    if (!s_initialized) {
        return;
    }

    level = rt_hw_interrupt_disable();
    s_stream_generation++;
    if (s_stream_generation == 0) {
        s_stream_generation = 1;
    }
    s_receiving = false;
    s_drop_frame = false;
    s_soi_state = 0;
    s_prev_byte = 0;
    s_frame_len = 0;
    s_ready_idx = -1;
    s_ready_generation = 0;
    s_rx_idx = 0;
    for (i = 0; i < CDC_IMAGE_BUF_NUM; i++) {
        s_img_size[i] = 0;
    }
    rt_hw_interrupt_enable(level);

    while (s_frame_sem &&
           rt_sem_take(s_frame_sem, RT_WAITING_NO) == RT_EOK) {
    }
}

bool cdc_image_processor_wait_idle(uint32_t timeout_ms)
{
    rt_tick_t deadline;

    if (!s_initialized) {
        return true;
    }

    deadline = rt_tick_get() + rt_tick_from_millisecond(timeout_ms);
    while (s_decoding_idx >= 0) {
        if ((rt_int32_t)(rt_tick_get() - deadline) >= 0) {
            return false;
        }
        rt_thread_mdelay(1);
    }

    return true;
}

void cdc_image_rx(const uint8_t *data, uint32_t len)
{
    uint32_t i;
    int idx;

    if (!s_initialized || !s_enabled || !data || len == 0) {
        return;
    }

    for (i = 0; i < len; i++) {
        uint8_t byte = data[i];

        if (!s_enabled) {
            return;
        }

        if (!s_receiving) {
            if (s_soi_state == 0) {
                s_soi_state = (byte == 0xFF) ? 1 : 0;
                continue;
            }

            if (byte == 0xD8) {
                idx = choose_rx_buffer();
                s_frame_len = 0;
                s_drop_frame = (idx < 0);
                if (!s_drop_frame) {
                    s_img_buf[idx][s_frame_len++] = 0xFF;
                    s_img_buf[idx][s_frame_len++] = 0xD8;
                }
                s_prev_byte = 0xD8;
                s_receiving = true;
                s_soi_state = 0;
                continue;
            }

            s_soi_state = (byte == 0xFF) ? 1 : 0;
            continue;
        }

        if (s_drop_frame) {
            if (s_prev_byte == 0xFF && byte == 0xD9) {
                s_receiving = false;
                s_drop_frame = false;
                s_soi_state = 0;
                s_prev_byte = 0;
                continue;
            }
            s_prev_byte = byte;
            continue;
        }

        idx = s_rx_idx;
        if (s_frame_len >= CDC_IMAGE_MAX_SIZE) {
            rt_kprintf("cdc image too large, drop until EOI, max %u\n",
                       (unsigned int)CDC_IMAGE_MAX_SIZE);
            s_frame_len = 0;
            s_soi_state = 0;
            s_drop_frame = true;
            if (s_prev_byte == 0xFF && byte == 0xD9) {
                s_receiving = false;
                s_drop_frame = false;
                s_prev_byte = 0;
            } else {
                s_prev_byte = byte;
            }
            continue;
        }

        s_img_buf[idx][s_frame_len++] = byte;

        if (s_prev_byte == 0xFF && byte == 0xD9) {
            publish_frame(idx, s_frame_len);
            s_receiving = false;
            s_soi_state = 0;
            s_frame_len = 0;
            s_prev_byte = 0;
            continue;
        }

        s_prev_byte = byte;
    }
}

static void cdc_image_decoder_thread(void *parameter)
{
    (void)parameter;

    while (1) {
        int idx = -1;
        uint32_t size = 0;
        uint32_t generation = 0;
        rt_base_t level;

        if (rt_sem_take(s_frame_sem, RT_WAITING_FOREVER) != RT_EOK) {
            continue;
        }

        level = rt_hw_interrupt_disable();
        if (s_enabled && s_output_enabled) {
            idx = s_ready_idx;
        }
        if (idx >= 0) {
            s_ready_idx = -1;
            s_decoding_idx = idx;
            size = s_img_size[idx];
            generation = s_ready_generation;
        }
        rt_hw_interrupt_enable(level);

        if (idx >= 0 && size > 0) {
            if (s_enabled && s_output_enabled &&
                generation == s_stream_generation) {
                int ret;

                if (!cdc_image_jpeg_sane(s_img_buf[idx], size)) {
                    rt_kprintf("cdc image drop invalid jpeg, len %u\n", size);
                    goto done;
                }

                ret = decode_pic_cdc_persistent(s_img_buf[idx], (int)size,
                                                AICFB_LAYER_TYPE_UI);
                if (ret != RT_EOK) {
                    rt_kprintf("cdc image decode failed: %d\n", ret);
                } else if (s_enabled && s_output_enabled &&
                           generation == s_stream_generation) {
                    /* decode_pic() returns only after pan-display/vsync. */
                    cdc_image_frame_rendered();
                }
            }
        }

done:
        level = rt_hw_interrupt_disable();
        s_decoding_idx = -1;
        rt_hw_interrupt_enable(level);
    }
}

int cdc_image_decoder_start(void)
{
    if (!s_initialized) {
        return -RT_ERROR;
    }

    if (s_decoder_tid) {
        return RT_EOK;
    }

    s_decoder_tid = rt_thread_create("cdc_dec",
                                     cdc_image_decoder_thread,
                                     RT_NULL,
                                     CDC_IMAGE_THREAD_STACK,
                                     CDC_IMAGE_THREAD_PRIO,
                                     CDC_IMAGE_THREAD_TICK);
    if (!s_decoder_tid) {
        return -RT_ERROR;
    }

    rt_thread_startup(s_decoder_tid);
    return RT_EOK;
}
