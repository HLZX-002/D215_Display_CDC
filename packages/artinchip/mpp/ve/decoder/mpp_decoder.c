/*
 * Copyright (C) 2020-2024 ArtInChip Technology Co. Ltd
 *
 *  SPDX-License-Identifier: Apache-2.0
 *  author: <qi.xu@artinchip.com>
 *  Desc: mpp_decoder interface
 */

#include "frame_allocator.h"
#include "mpp_codec.h"
#include "mpp_log.h"
#include <rtthread.h>
#include <string.h>

extern struct mpp_decoder *create_jpeg_decoder();
extern struct mpp_decoder *create_png_decoder();
extern struct mpp_decoder *create_h264_decoder();
extern struct mpp_decoder *create_aicp_decoder();

/*
 * USB Display owns a persistent MJPEG decoder even while usb_display_en is
 * zero (it still decodes an occasional keep-alive frame).  CDC uses a second,
 * short-lived decoder.  Serializing only individual VE calls is insufficient:
 * Display can otherwise enter between CDC init/decode/destroy and corrupt the
 * shared allocator state.  RT-Thread mutexes are recursive, so the CDC thread
 * can hold this for its complete transaction while the API wrappers below
 * safely take it again.
 */
static struct rt_mutex s_decoder_session_lock;

static int mpp_decoder_session_init(void)
{
    return rt_mutex_init(&s_decoder_session_lock, "mpp_dec", RT_IPC_FLAG_PRIO);
}
INIT_PREV_EXPORT(mpp_decoder_session_init);

void mpp_decoder_session_lock(void)
{
    rt_mutex_take(&s_decoder_session_lock, RT_WAITING_FOREVER);
}

void mpp_decoder_session_unlock(void)
{
    rt_mutex_release(&s_decoder_session_lock);
}

/*
 * The D21x VE cannot safely run a JPEG reset in the middle of an H.264
 * session.  This project-level gate lets the local H.264 logo own VE while
 * USB transport/CDC reception continue without touching the shared hardware.
 */
/* The USB mode arbiter overrides this to expose the first decoded JPEG. */
__attribute__((weak)) void mpp_decoder_mjpeg_frame_ready(void)
{
}

void mpp_decoder_set_mjpeg_decode_enabled(int enabled)
{
    /* Keep the compatibility API for the mode arbiter, but preserve the
     * official Display decoder's packet and return-value semantics. */
    (void)enabled;
}

int mpp_decoder_mjpeg_decode_is_busy(void)
{
    return 0;
}

struct mpp_decoder *mpp_decoder_create(enum mpp_codec_type type)
{
    struct mpp_decoder *decoder = NULL;

    mpp_decoder_session_lock();
    if (type == MPP_CODEC_VIDEO_DECODER_MJPEG)
        decoder = create_jpeg_decoder();
#ifdef AIC_MPP_H264_DEC_ENABLE
    else if (type == MPP_CODEC_VIDEO_DECODER_H264)
        decoder = create_h264_decoder();
#endif
    else if (type == MPP_CODEC_VIDEO_DECODER_PNG)
        decoder = create_png_decoder();
#ifdef AIC_MPP_AICP_DEC_ENABLE
    else if (type == MPP_CODEC_VIDEO_DECODER_AICP)
        decoder = create_aicp_decoder();
#endif
    mpp_decoder_session_unlock();
    return decoder;
}

void mpp_decoder_destory(struct mpp_decoder *decoder)
{
    if (decoder == NULL)
        return;

    mpp_decoder_session_lock();
    decoder->ops->destory(decoder);
    mpp_decoder_session_unlock();
}

int mpp_decoder_init(struct mpp_decoder *decoder, struct decode_config *config)
{
    int ret;

    if (decoder == NULL || config == NULL)
        return DEC_ERR_NULL_PTR;

    mpp_decoder_session_lock();
    ret = decoder->ops->init(decoder, config);
    mpp_decoder_session_unlock();
    return ret;
}

int mpp_decoder_decode(struct mpp_decoder *decoder)
{
    int ret;

    if (decoder == NULL)
        return DEC_ERR_NULL_PTR;

    mpp_decoder_session_lock();
    if (decoder->pm && decoder->fm) {
        logd("render frame number: %d, empty frame number: %d",
             fm_get_render_frame_num(decoder->fm),
             fm_get_empty_frame_num(decoder->fm));
    }

    if (decoder->pm && pm_get_ready_packet_num(decoder->pm) == 0) {
        ret = DEC_NO_READY_PACKET;
        goto out;
    }

    ret = decoder->ops->decode(decoder);
out:
    mpp_decoder_session_unlock();
    return ret;
}

int mpp_decoder_control(struct mpp_decoder *decoder, int cmd, void *param)
{
    int ret = 0;

    if (decoder == NULL || param == NULL)
        return DEC_ERR_NULL_PTR;

    mpp_decoder_session_lock();
    if (cmd == MPP_DEC_INIT_CMD_SET_EXT_FRAME_ALLOCATOR) {
        struct frame_allocator *allocator = (struct frame_allocator *)param;
        decoder->allocator = allocator;
    } else if (cmd == MPP_DEC_INIT_CMD_SET_ROT_FLIP_FLAG) {
        decoder->rotmir_flag = *((int *)param);
        logi("decoder->rotmir_flag: %d", decoder->rotmir_flag);
    } else if (cmd == MPP_DEC_INIT_CMD_SET_SCALE) {
        struct mpp_scale_ratio *ratio = (struct mpp_scale_ratio *)param;
        decoder->ver_scale = ratio->ver_scale;
        decoder->hor_scale = ratio->hor_scale;
    } else if (cmd == MPP_DEC_INIT_CMD_SET_CROP_INFO) {
        struct mpp_dec_crop_info *info = (struct mpp_dec_crop_info *)param;
        decoder->crop_en = 1;
        decoder->crop_x = info->crop_x;
        decoder->crop_y = info->crop_y;
        decoder->crop_width = info->crop_width;
        decoder->crop_height = info->crop_height;
        decoder->output_x = info->crop_out_x;
        decoder->output_y = info->crop_out_y;
    } else if (cmd == MPP_DEC_INIT_CMD_SET_EXT_PACKET_ALLOCATOR) {
        struct packet_allocator *allocator = (struct packet_allocator *)param;
        decoder->pkt_allocator = allocator;
    } else {
        ret = decoder->ops->control(decoder, cmd, param);
    }
    mpp_decoder_session_unlock();
    return ret;
}

int mpp_decoder_reset(struct mpp_decoder *decoder)
{
    int ret;

    if (decoder == NULL)
        return DEC_ERR_NULL_PTR;

    mpp_decoder_session_lock();
    ret = decoder->ops->reset(decoder);
    mpp_decoder_session_unlock();
    return ret;
}

int mpp_decoder_get_packet(struct mpp_decoder *decoder, struct mpp_packet *packet, int size)
{
    int ret;

    if (decoder == NULL || packet == NULL || decoder->pm == NULL)
        return DEC_ERR_NULL_PTR;

    mpp_decoder_session_lock();
    if (pm_get_empty_packet_num(decoder->pm) == 0)
        ret = DEC_NO_EMPTY_PACKET;
    else
        ret = pm_dequeue_empty_packet(decoder->pm, packet, size);
    mpp_decoder_session_unlock();
    return ret;
}

int mpp_decoder_put_packet(struct mpp_decoder *decoder, struct mpp_packet *packet)
{
    int ret;

    if (decoder == NULL || packet == NULL || decoder->pm == NULL)
        return DEC_ERR_NULL_PTR;

    mpp_decoder_session_lock();
    ret = pm_enqueue_ready_packet(decoder->pm, packet);
    mpp_decoder_session_unlock();
    return ret;
}

int mpp_decoder_get_frame(struct mpp_decoder *decoder, struct mpp_frame *frame)
{
    int ret;

    if (decoder == NULL || frame == NULL)
        return DEC_ERR_NULL_PTR;

    mpp_decoder_session_lock();
    if (decoder->fm == NULL) {
        // logw("frame manager not create now, wait");
        ret = DEC_ERR_FM_NOT_CREATE;
        goto out;
    }

    if (fm_get_render_frame_num(decoder->fm) == 0) {
        ret = DEC_NO_RENDER_FRAME;
        goto out;
    }

    ret = fm_render_get_frame(decoder->fm, frame);
    if (ret == DEC_OK && decoder->ops && decoder->ops->name &&
        strcmp(decoder->ops->name, "mjpeg") == 0) {
        mpp_decoder_mjpeg_frame_ready();
    }

out:
    mpp_decoder_session_unlock();
    return ret;
}

int mpp_decoder_put_frame(struct mpp_decoder *decoder, struct mpp_frame *frame)
{
    int ret;

    if (decoder == NULL || frame == NULL)
        return DEC_ERR_NULL_PTR;

    mpp_decoder_session_lock();
    if (decoder->fm == NULL) {
        // logw("frame manager not create now, wait");
        ret = DEC_ERR_FM_NOT_CREATE;
    } else {
        ret = fm_render_put_frame(decoder->fm, frame);
    }
    mpp_decoder_session_unlock();
    return ret;
}
