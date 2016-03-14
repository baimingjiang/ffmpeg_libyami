/*
 * Intel Yet Another Media Infrastructure video decoder/encoder
 *
 * Copyright (c) 2016 Intel Corporation
 *     Zhou Yun(yunx.z.zhou@intel.com)
 *     Jun Zhao(jun.zhao@intel.com)
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libyami.h"
#include "libyami_utils.h"

#define ENCODE_TRACE(format, ...)  av_log(avctx, AV_LOG_VERBOSE, "< encode > line:%4d " format, __LINE__, ##__VA_ARGS__)

using namespace YamiMediaCodec;

static void *encodeThread(void *arg)
{
    AVCodecContext *avctx = (AVCodecContext *)arg;
    YamiEncContext *s = (YamiEncContext *)avctx->priv_data;

    while (1) {
        AVFrame *frame;
        VideoFrameRawData *in_buffer = NULL;
        // deque one input buffer
        ENCODE_TRACE("encode thread runs one cycle start ... \n");
        pthread_mutex_lock(&s->in_mutex);
        if (s->in_queue->empty()) {
            if (s->encode_status == ENCODE_THREAD_GOT_EOS) {
                pthread_mutex_unlock(&s->in_mutex);
                break;
            } else {
                ENCODE_TRACE("encode thread wait because s->in_queue is empty\n");
                pthread_cond_wait(&s->in_cond, &s->in_mutex); // wait if no todo frame is available
            }
        }

        if (s->in_queue->empty()) { // may wake up from EOS/Close
            pthread_mutex_unlock(&s->in_mutex);
            continue;
        }

        ENCODE_TRACE("s->in_queue->size()=%ld\n", s->in_queue->size());
        frame = s->in_queue->front();
        pthread_mutex_unlock(&s->in_mutex);

        // encode one input in_buffer
        Decode_Status status;
        if (frame->format != AV_PIX_FMT_YAMI) { /* non zero-copy mode */
            uint32_t src_linesize[4];
            const uint8_t *src_data[4];

            uint8_t *dst_data[4];

            in_buffer = (VideoFrameRawData *)av_malloc(sizeof(VideoFrameRawData));
            if (frame->pict_type == AV_PICTURE_TYPE_I)
                in_buffer->flags |= VIDEO_FRAME_FLAGS_KEY;

            in_buffer->width = avctx->width;
            in_buffer->height = avctx->height;

            uint8_t *yamidata = reinterpret_cast<uint8_t *>(s->m_buffer);
            
            if (avctx->pix_fmt == AV_PIX_FMT_YUV420P){
                in_buffer->pitch[0] = avctx->width;
                in_buffer->pitch[1] = (in_buffer->pitch[0] + 1) >> 1;
                in_buffer->pitch[2] = (in_buffer->pitch[0] + 1) >> 1;
                
                src_linesize[0] = frame->linesize[0];
                src_linesize[1] = frame->linesize[1];
                src_linesize[2] = frame->linesize[2];
                
                dst_data[0] = yamidata;
                dst_data[1] = yamidata + in_buffer->pitch[0] * avctx->height;
                dst_data[2] = dst_data[1] + in_buffer->pitch[1] * ((avctx->height + 1) >> 1);
                
                src_data[0] = frame->data[0];
                src_data[1] = frame->data[1];
                src_data[2] = frame->data[2];
            } else if (avctx->pix_fmt == AV_PIX_FMT_NV12){
                in_buffer->pitch[0] = avctx->width;
                in_buffer->pitch[1] = avctx->width;
                
                src_linesize[0] = frame->linesize[0];
                src_linesize[1] = frame->linesize[1];
                
                dst_data[0] = yamidata;
                dst_data[1] = yamidata + in_buffer->pitch[0] * avctx->height;
                
                src_data[0] = frame->data[0];
                src_data[1] = frame->data[1];
                src_data[2] = frame->data[2];

            }

            av_image_copy(dst_data, (int *)in_buffer->pitch, src_data,
                          (int *)src_linesize, avctx->pix_fmt, avctx->width,
                          avctx->height);

            in_buffer->handle = reinterpret_cast<intptr_t>(yamidata);
            in_buffer->width = avctx->width;
            in_buffer->height = avctx->height;
            in_buffer->fourcc = avctx->pix_fmt == AV_PIX_FMT_YUV420P ? VA_FOURCC_I420 : VA_FOURCC_NV12;
            in_buffer->offset[0] = 0;
            in_buffer->offset[1] = in_buffer->pitch[0] * avctx->height;
            in_buffer->offset[2] = in_buffer->offset[1] + in_buffer->pitch[1] * ((avctx->height + 1) >> 1);

            /* handle decoder busy case */
            do {
                 status = s->encoder->encode(in_buffer);
            } while (status == ENCODE_IS_BUSY);

            ENCODE_TRACE("encode() status=%d, encode_count_yami=%d\n", status, s->encode_count_yami);
            av_free(in_buffer);
        } else { /* zero-copy mode */
            SharedPtr < VideoFrame > yami_frame;
            in_buffer = (VideoFrameRawData *)frame->data[3];

            if (frame) {
                yami_frame.reset(new VideoFrame);
                yami_frame->surface = (intptr_t)in_buffer->internalID; /* XXX: get decoded surface */
                yami_frame->timeStamp = in_buffer->timeStamp;
                yami_frame->crop.x = 0;
                yami_frame->crop.y = 0;
                yami_frame->crop.width = avctx->width;
                yami_frame->crop.height = avctx->height;
                yami_frame->flags = 0;
                if (frame->pict_type == AV_PICTURE_TYPE_I)
                    in_buffer->flags |= VIDEO_FRAME_FLAGS_KEY;

            }

            /* handle decoder busy case */
            do {
                 status = s->encoder->encode(yami_frame);
            } while (status == ENCODE_IS_BUSY);

            ENCODE_TRACE("encode() status=%d, encode_count_yami=%d\n", status, s->encode_count_yami);
        }

        if (status < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "encode error %d frame %d\n", status , s->encode_count_yami);
        }
        s->encode_count_yami++;
        pthread_mutex_lock(&s->out_mutex);
        s->out_queue->push_back(frame);
        pthread_mutex_unlock(&s->out_mutex);
        s->in_queue->pop_front();
    }

    ENCODE_TRACE("encode thread exit\n");
    pthread_mutex_lock(&s->mutex_);
    s->encode_status = ENCODE_THREAD_EXIT;
    pthread_mutex_unlock(&s->mutex_);

    return NULL;
}

static bool
createOutputBuffer(VideoEncOutputBuffer *outputBuffer, int maxOutSize)
{
    outputBuffer->data = static_cast<uint8_t *>(malloc(maxOutSize));
    if (!outputBuffer->data)
        return false;
    outputBuffer->bufferSize = maxOutSize;
    outputBuffer->format = OUTPUT_EVERYTHING;

    return true;
}

static bool destroyOutputBuffer(VideoEncOutputBuffer *outputBuffer)
{
    if (outputBuffer->data)
        free(outputBuffer->data);

    return true;
}

int yami_enc_init(AVCodecContext *avctx, const char *mime_type)
{
    YamiEncContext *s = (YamiEncContext *) avctx->priv_data;
    Encode_Status status;

    enum AVPixelFormat pix_fmts[4] =
        {
            AV_PIX_FMT_YAMI,
            AV_PIX_FMT_NV12,
            AV_PIX_FMT_YUV420P,
            AV_PIX_FMT_NONE
        };

    if (avctx->pix_fmt == AV_PIX_FMT_NONE) {
        int ret = ff_get_format(avctx, pix_fmts);
        if (ret < 0)
            return ret;

        avctx->pix_fmt      = (AVPixelFormat)ret;
    }

    av_log(avctx, AV_LOG_VERBOSE, "yami_enc_init\n");
    s->encoder = createVideoEncoder(mime_type);
    if (!s->encoder) {
        av_log(avctx, AV_LOG_ERROR, "fail to create libyami %s encoder\n", mime_type);
        return -1;
    }

    NativeDisplay native_display;
    native_display.type = NATIVE_DISPLAY_VA;
    VADisplay m_display = createVADisplay();
    native_display.handle = (intptr_t)m_display;
    s->encoder->setNativeDisplay(&native_display);

    // configure encoding parameters
    VideoParamsCommon encVideoParams;
    encVideoParams.size = sizeof(VideoParamsCommon);
    s->encoder->getParameters(VideoParamsTypeCommon, &encVideoParams);

    encVideoParams.resolution.width  = avctx->width;
    encVideoParams.resolution.height = avctx->height;

    // frame rate parameters.
    if (avctx->framerate.den > 0 && avctx->framerate.num > 0) {
        encVideoParams.frameRate.frameRateDenom = avctx->framerate.den;
        encVideoParams.frameRate.frameRateNum = avctx->framerate.num;
    } else {
        encVideoParams.frameRate.frameRateNum = avctx->time_base.den;
        encVideoParams.frameRate.frameRateDenom = avctx->time_base.num;
    }

    // picture type and bitrate
    encVideoParams.intraPeriod = av_clip(avctx->gop_size, 1, 250);
    encVideoParams.ipPeriod = !avctx->has_b_frames ? 1 : 3;
    if (s->rcmod){
        if (!strcmp(s->rcmod, "CQP"))
            encVideoParams.rcMode = RATE_CONTROL_CQP;
        else if (!strcmp(s->rcmod, "VBR")){
            encVideoParams.rcMode = RATE_CONTROL_VBR;
            encVideoParams.rcParams.bitRate = avctx->bit_rate;
        } else {
            encVideoParams.rcMode = RATE_CONTROL_CBR;
            encVideoParams.rcParams.bitRate = avctx->bit_rate;
        }
    } else {
        encVideoParams.rcMode = RATE_CONTROL_CQP;
    }

    encVideoParams.rcParams.initQP = av_clip(s->cqp,1,52);

    if (s->level){
        encVideoParams.level = atoi(s->level);
    } else {
        encVideoParams.level = 40;
    }
    /*libyami only support h264 main now*/
//    if (s->profile){
//        if (!strcmp(s->profile , "high"))
//            encVideoParams.profile = VAProfileH264High;
//        else if(!strcmp(s->profile , "main")){
//            encVideoParams.profile = VAProfileH264Main;
//        } else {
//            encVideoParams.profile = VAProfileH264Baseline;
//        }
//    } else {
//        encVideoParams.profile = VAProfileH264High;
//    }
    // s->encoder->setEncoderParameters(&encVideoParams);
    encVideoParams.size = sizeof(VideoParamsCommon);
    s->encoder->setParameters(VideoParamsTypeCommon, &encVideoParams);

    if (!strcmp(mime_type, YAMI_MIME_H264)) {
        VideoConfigAVCStreamFormat streamFormat;
        streamFormat.size = sizeof(VideoConfigAVCStreamFormat);
        streamFormat.streamFormat = AVC_STREAM_FORMAT_ANNEXB;
        s->encoder->setParameters(VideoConfigTypeAVCStreamFormat, &streamFormat);
    }

    status = s->encoder->start();
    if (status != ENCODE_SUCCESS) {
        av_log(avctx, AV_LOG_ERROR, "yami encoder fail to start\n");
        return -1;
    }

    // init output buffer
    s->encoder->getMaxOutSize(&(s->maxOutSize));

    if (!createOutputBuffer(&s->outputBuffer, s->maxOutSize)) {
        av_log(avctx, AV_LOG_ERROR,  "fail to create output\n");
        return -1;
    }
    s->m_frameSize = FFALIGN(avctx->width, 32) * FFALIGN(avctx->height, 32) * 3;
    s->m_buffer = static_cast<uint8_t *>(av_mallocz(s->m_frameSize));

    s->in_queue = new std::deque<AVFrame *>;
    s->out_queue = new std::deque<AVFrame *>;
    pthread_mutex_init(&s->mutex_, NULL);
    pthread_mutex_init(&s->in_mutex, NULL);
    pthread_mutex_init(&s->out_mutex, NULL);
    pthread_cond_init(&s->in_cond, NULL);
    s->encode_status = ENCODE_THREAD_NOT_INIT;

    s->encode_count = 0;
    s->encode_count_yami = 0;
    s->render_count = 0;
    av_log(avctx, AV_LOG_DEBUG, "yami_enc_init\n");
    return 0;
}

int yami_enc_frame(AVCodecContext *avctx, AVPacket *pkt,
                   const AVFrame *frame, int *got_packet)
{
    YamiEncContext *s = (YamiEncContext *)avctx->priv_data;
    Encode_Status status;
    int ret;

    if(!s->encoder)
        return -1;
    if (frame) {
        AVFrame *qframe = av_frame_alloc();
        if (!qframe) {
            return AVERROR(ENOMEM);
        }

        /* av_frame_ref the src frame and av_frame_unref in encode thread */
        ret = av_frame_ref(qframe, frame);
        if (ret < 0)
            return ret;

        while (s->encode_status < ENCODE_THREAD_GOT_EOS) { // we need enque eos buffer more than once
            pthread_mutex_lock(&s->in_mutex);
            if (s->in_queue->size() < ENCODE_QUEUE_SIZE) {
                s->in_queue->push_back(qframe);
                av_log(avctx, AV_LOG_VERBOSE, "wakeup encode thread ...\n");
                pthread_cond_signal(&s->in_cond);
                pthread_mutex_unlock(&s->in_mutex);
                break;
            }
            pthread_mutex_unlock(&s->in_mutex);

            av_log(avctx, AV_LOG_DEBUG,
                   "s->in_queue->size()=%ld, s->encode_count=%d, s->encode_count_yami=%d, too many buffer are under encoding, wait ...\n",
                   s->in_queue->size(), s->encode_count, s->encode_count_yami);
            usleep(1000);
        };
        s->encode_count++;
    }

    // decode thread status update
    pthread_mutex_lock(&s->mutex_);
    switch (s->encode_status) {
    case ENCODE_THREAD_NOT_INIT:
    case ENCODE_THREAD_EXIT:
        if (frame) {
            s->encode_status = ENCODE_THREAD_RUNING;
            pthread_create(&s->encode_thread_id, NULL, &encodeThread, avctx);
        }
        break;
    case ENCODE_THREAD_RUNING:
        if (!frame) {
            s->encode_status = ENCODE_THREAD_GOT_EOS; // call releaseLock for seek
        }
        break;
    case ENCODE_THREAD_GOT_EOS:
        if (s->in_queue->empty())
            s->encode_status = ENCODE_THREAD_NOT_INIT;
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&s->mutex_);

    do {
        status = s->encoder->getOutput(&s->outputBuffer, true);
    } while (!frame && status != ENCODE_SUCCESS && s->in_queue->size() > 0);
    if (status != ENCODE_SUCCESS)
        return 0;
    
    if ((ret = ff_alloc_packet2(avctx, pkt, s->outputBuffer.dataSize, 0)) < 0)
        return ret;
    pthread_mutex_lock(&s->out_mutex);
    if (!s->out_queue->empty()) {
        AVFrame *qframe = s->out_queue->front();
        if (qframe) {
            pkt->pts = qframe->pts;
            av_frame_free(&qframe);
        }
        s->out_queue->pop_front();
    }
    pthread_mutex_unlock(&s->out_mutex);
    s->render_count++;

    void *p = pkt->data;
    memcpy(p, s->outputBuffer.data, s->outputBuffer.dataSize);
    if (s->outputBuffer.flag & ENCODE_BUFFERFLAG_SYNCFRAME)
        pkt->flags |= AV_PKT_FLAG_KEY;

    *got_packet = 1;

    return 0;
}

int yami_enc_close(AVCodecContext *avctx)
{
    YamiEncContext *s = (YamiEncContext *)avctx->priv_data;

    destroyOutputBuffer(&s->outputBuffer);
    pthread_mutex_lock(&s->mutex_);
    while (s->encode_status == ENCODE_THREAD_RUNING) {
        // potential race condition on s->encode_status
        s->encode_status = ENCODE_THREAD_GOT_EOS;
        pthread_mutex_unlock(&s->mutex_);
        pthread_cond_signal(&s->in_cond);
        usleep(10000);
        pthread_mutex_lock(&s->mutex_);
    }
    pthread_mutex_unlock(&s->mutex_);

    if (s->encoder) {
        s->encoder->stop();
        releaseVideoEncoder(s->encoder);
        s->encoder = NULL;
    }
    pthread_mutex_destroy(&s->in_mutex);
    pthread_mutex_destroy(&s->out_mutex);
    pthread_cond_destroy(&s->in_cond);
    while (!s->in_queue->empty()) {
        AVFrame *in_buffer = s->in_queue->front();
        s->in_queue->pop_front();
        av_frame_free(&in_buffer);
    }
    while (!s->out_queue->empty()) {
            AVFrame *out_buffer = s->out_queue->front();
            s->out_queue->pop_front();
            av_frame_free(&out_buffer);
    }
    delete s->in_queue;
    delete s->out_queue;

    av_free(s->m_buffer);
    s->m_frameSize = 0;

    av_log(avctx, AV_LOG_DEBUG, "yami_enc_close\n");

    return 0;
}

#if CONFIG_LIBYAMI_H264_ENCODER
static av_cold int yami_enc_h264_init(AVCodecContext *avctx)
{
    if (avctx->width%2 != 0 || avctx->height%2 != 0) {
        av_log(avctx, AV_LOG_ERROR,
                      "width or height not divisible by 2 (%dx%d) .\n",
                       avctx->width, avctx->height);
        return AVERROR(EINVAL);
    }
    return yami_enc_init(avctx, YAMI_MIME_H264);
}

static int yami_enc_h264_frame(AVCodecContext *avctx, AVPacket *pkt,
                               const AVFrame *frame, int *got_packet)
{
    return yami_enc_frame(avctx, pkt, frame, got_packet);
}

static av_cold int yami_enc_h264_close(AVCodecContext *avctx)
{
    return yami_enc_close(avctx);
}

static const AVCodecDefault yami_enc_264_defaults[] = {
    { (uint8_t *)("b"),                (uint8_t *)("2M") },
    { (uint8_t *)("g"),                (uint8_t *)("30") },
    { NULL },
};

#define OFFSET(x) offsetof(YamiEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption h264_options[] = {
    { "profile",       "Set profile restrictions ", OFFSET(profile),       AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE},
    { "level",         "Specify level (as defined by Annex A)", OFFSET(level), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, VE},
    { "rcmode",        "rate control mode", OFFSET(rcmod), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, VE},
    { "qp",            "Constant quantization parameter rate control method",OFFSET(cqp),        AV_OPT_TYPE_INT,    { .i64 = 26 }, 1, 52, VE },
//    { "cavlc",            NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 },  INT_MIN, INT_MAX, VE, "coder" },
//    { "cabac",            NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 },  INT_MIN, INT_MAX, VE, "coder" },
    { NULL },
};

static const AVClass yami_enc_h264_class = {
    .class_name = "libyami_h264",
    .item_name  = av_default_item_name,
    .option     = h264_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_libyami_h264_encoder = {
    .name                  = "libyami_h264",
    .long_name             = NULL_IF_CONFIG_SMALL("libyami H.264 encoder"),
    .type                  = AVMEDIA_TYPE_VIDEO,
    .id                    = AV_CODEC_ID_H264,
    .capabilities          = CODEC_CAP_DELAY, // it is not necessary to support multi-threads
    .supported_framerates  = NULL,
    .pix_fmts              = (const enum AVPixelFormat[]) { AV_PIX_FMT_YAMI,
                                                            AV_PIX_FMT_NV12,
                                                            AV_PIX_FMT_YUV420P,
                                                            AV_PIX_FMT_NONE},
    .supported_samplerates = NULL,
    .sample_fmts           = NULL,
    .channel_layouts       = NULL,
#if FF_API_LOWRES
    .max_lowres            = 0,
#endif
    .priv_class            = &yami_enc_h264_class,
    .profiles              = NULL,
    .priv_data_size        = sizeof(YamiEncContext),
    .next                  = NULL,
    .init_thread_copy      = NULL,
    .update_thread_context = NULL,
    .defaults              = yami_enc_264_defaults,
    .init_static_data      = NULL,
    .init                  = yami_enc_h264_init,
    .encode_sub            = NULL,
    .encode2               = yami_enc_h264_frame,
    .decode                = NULL,
    .close                 = yami_enc_h264_close,
    .flush                 = NULL, // TODO, add it
};
#endif

#if CONFIG_LIBYAMI_VP8_ENCODER
static av_cold int yami_enc_vp8_init(AVCodecContext *avctx)
{
    return yami_enc_init(avctx, YAMI_MIME_VP8);
}

static int yami_enc_vp8_frame(AVCodecContext *avctx, AVPacket *pkt,
                               const AVFrame *frame, int *got_packet)
{
    return yami_enc_frame(avctx, pkt, frame, got_packet);
}

static av_cold int yami_enc_vp8_close(AVCodecContext *avctx)
{
    return yami_enc_close(avctx);
}

static const AVCodecDefault yami_enc_vp8_defaults[] = {
    { (uint8_t *)("b"),                (uint8_t *)("2M") },
    { (uint8_t *)("g"),                (uint8_t *)("30") },
    { NULL },
};

#define OFFSET(x) offsetof(YamiEncContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption vp8_options[] = {
    { "profile",       "Set profile restrictions ", OFFSET(profile),       AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE},
    { "level",         "Specify level (as defined by Annex A)", OFFSET(level), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, VE},
    { "rcmode",        "rate control mode", OFFSET(rcmod), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, VE},
    { "qp",            "Constant quantization parameter rate control method",OFFSET(cqp),        AV_OPT_TYPE_INT,    { .i64 = 26 }, 1, 52, VE },
//    { "cavlc",            NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 0 },  INT_MIN, INT_MAX, VE, "coder" },
//    { "cabac",            NULL, 0, AV_OPT_TYPE_CONST, { .i64 = 1 },  INT_MIN, INT_MAX, VE, "coder" },
    { NULL },
};

static const AVClass yami_enc_vp8_class = {
    .class_name = "libyami_vp8",
    .item_name  = av_default_item_name,
    .option     = vp8_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_libyami_vp8_encoder = {
    .name                  = "libyami_vp8",
    .long_name             = NULL_IF_CONFIG_SMALL("libyami VP8 encoder"),
    .type                  = AVMEDIA_TYPE_VIDEO,
    .id                    = AV_CODEC_ID_VP8,
    .capabilities          = CODEC_CAP_DELAY, // it is not necessary to support multi-threads
    .supported_framerates  = NULL,
    .pix_fmts              = (const enum AVPixelFormat[]) { AV_PIX_FMT_YAMI,
                                                            AV_PIX_FMT_NV12,
                                                            AV_PIX_FMT_YUV420P,
                                                            AV_PIX_FMT_NONE},
    .supported_samplerates = NULL,
    .sample_fmts           = NULL,
    .channel_layouts       = NULL,
#if FF_API_LOWRES
    .max_lowres            = 0,
#endif
    .priv_class            = &yami_enc_vp8_class,
    .profiles              = NULL,
    .priv_data_size        = sizeof(YamiEncContext),
    .next                  = NULL,
    .init_thread_copy      = NULL,
    .update_thread_context = NULL,
    .defaults              = yami_enc_vp8_defaults,
    .init_static_data      = NULL,
    .init                  = yami_enc_vp8_init,
    .encode_sub            = NULL,
    .encode2               = yami_enc_vp8_frame,
    .decode                = NULL,
    .close                 = yami_enc_vp8_close,
    .flush                 = NULL, // TODO, add it
};
#endif
