/*
 * Intel MediaSDK QSV utility functions
 *
 * copyright (c) 2013 Luca Barbato
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <string.h>
#include <sys/types.h>
#include <mfx/mfxvideo.h>

#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/time.h"
#include "internal.h"
#include "avcodec.h"
#include "qsv.h"

int ff_qsv_error(int mfx_err)
{
    switch (mfx_err) {
    case MFX_ERR_NONE:
        return 0;
    case MFX_ERR_MEMORY_ALLOC:
    case MFX_ERR_NOT_ENOUGH_BUFFER:
        return AVERROR(ENOMEM);
    case MFX_ERR_INVALID_HANDLE:
        return AVERROR(EINVAL);
    case MFX_ERR_DEVICE_FAILED:
    case MFX_ERR_DEVICE_LOST:
    case MFX_ERR_LOCK_MEMORY:
        return AVERROR(EIO);
    case MFX_ERR_NULL_PTR:
    case MFX_ERR_UNDEFINED_BEHAVIOR:
    case MFX_ERR_NOT_INITIALIZED:
        return AVERROR_BUG;
    case MFX_ERR_UNSUPPORTED:
    case MFX_ERR_NOT_FOUND:
        return AVERROR(ENOSYS);
    case MFX_ERR_MORE_DATA:
    case MFX_ERR_MORE_SURFACE:
    case MFX_ERR_MORE_BITSTREAM:
        return AVERROR(EAGAIN);
    case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:
    case MFX_ERR_INVALID_VIDEO_PARAM:
        return AVERROR(EINVAL);
    case MFX_ERR_ABORTED:
    case MFX_ERR_UNKNOWN:
    default:
        return AVERROR_UNKNOWN;
    }
}

static int codec_id_to_mfx(enum AVCodecID codec_id)
{
    switch (codec_id) {
    case AV_CODEC_ID_H264:
        return MFX_CODEC_AVC;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
        return MFX_CODEC_MPEG2;
    case AV_CODEC_ID_VC1:
        return MFX_CODEC_VC1;
    default:
        break;
    }

    return AVERROR(ENOSYS);
}

static void qsv_surface_free(QSVContext *q)
{
    if (q->surfaces) {
        for (int i = 0; i < q->nb_surfaces; i++)
            av_free(q->surfaces[i].Data.Y);
        av_freep(&q->surfaces);
    }
    av_free(q->dts);
    av_free(q->pts);
}

static int qsv_surface_alloc(QSVContext *q)
{
    int width, height, ret;
    mfxFrameAllocRequest req = { { 0 } };

    ret = MFXVideoDECODE_QueryIOSurf(q->session, &q->param, &req);
    if (ret < 0)
        return ff_qsv_error(ret);

    q->nb_surfaces = req.NumFrameSuggested + q->param.AsyncDepth;
    q->surfaces    = av_mallocz_array(q->nb_surfaces, sizeof(*q->surfaces));
    q->dts         = av_mallocz_array(q->nb_surfaces, sizeof(*q->dts));
    q->pts         = av_mallocz_array(q->nb_surfaces, sizeof(*q->pts));

    if (!q->surfaces || !q->dts || !q->pts)
        goto fail;

    width  = FFALIGN(req.Info.Width, 32);
    height = FFALIGN(req.Info.Height, 32);

    for (int i = 0; i < q->nb_surfaces; i++) {
        if (!(q->surfaces[i].Data.Y = av_mallocz(width * height * 12 / 8)))
            goto fail;
        q->surfaces[i].Data.U = q->surfaces[i].Data.Y + width * height;
        q->surfaces[i].Data.V = q->surfaces[i].Data.U + 1;
        q->surfaces[i].Data.Pitch = width;
        q->surfaces[i].Info = q->param.mfx.FrameInfo;
        q->pts[i] = q->dts[i] = AV_NOPTS_VALUE;
    }

fail:
    if (ret)
        qsv_surface_free(q);
    else
        q->last_ret = MFX_ERR_MORE_DATA;

    return ret;
}

static void qsv_setup(AVCodecContext *c, QSVContext *q)
{
//    if (c->width != q->param.mfx.FrameInfo.CropW)
        av_log(NULL, AV_LOG_ERROR, "width! %d %d\n",
               c->width, q->param.mfx.FrameInfo.CropW);
//    if (c->height != q->param.mfx.FrameInfo.CropH)
        av_log(NULL, AV_LOG_ERROR, "height! %d %d\n",
               c->height, q->param.mfx.FrameInfo.CropH);

    c->width         = q->param.mfx.FrameInfo.CropW;
    c->height        = q->param.mfx.FrameInfo.CropH;
    c->coded_width   = q->param.mfx.FrameInfo.Width;
    c->coded_height  = q->param.mfx.FrameInfo.Height;

    c->time_base.den = q->param.mfx.FrameInfo.FrameRateExtN;
    c->time_base.num = q->param.mfx.FrameInfo.FrameRateExtD;
}

int ff_qsv_init(AVCodecContext *c, QSVContext *q)
{
    int ret;
    mfxIMPL impl     = MFX_IMPL_AUTO_ANY;
    mfxVersion ver   = { { QSV_VERSION_MINOR, QSV_VERSION_MAJOR } };
    mfxBitstream *bs = &q->bs;

    if ((ret = codec_id_to_mfx(c->codec_id)) < 0)
        return ret;

    q->param.IOPattern   = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    q->param.AsyncDepth  = q->async_depth;
    q->param.mfx.CodecId = ret;

    if ((ret = MFXInit(impl, &ver, &q->session)) < 0) {
        av_log(c, AV_LOG_INFO, "Init %d\n", ret);
        return ff_qsv_error(ret);
    }

    MFXQueryIMPL(q->session, &impl);

    if (impl & MFX_IMPL_SOFTWARE)
        av_log(c, AV_LOG_INFO,
               "Using Intel QuickSync software implementation.\n");
    else if (impl & MFX_IMPL_HARDWARE)
        av_log(c, AV_LOG_INFO,
               "Using Intel QuickSync hardware accelerated implementation.\n");
    else
        av_log(c, AV_LOG_INFO,
               "Unknown Intel QuickSync implementation %d.\n", impl);

    if ((ret = MFXVideoDECODE_DecodeHeader(q->session, bs, &q->param)) < 0) {
        av_log(c, AV_LOG_INFO, "DecodeHeader %d\n", ret);
        return ff_qsv_error(ret);
    }
    bs->DataFlag = MFX_BITSTREAM_COMPLETE_FRAME;

    qsv_setup(c, q);

    ret = qsv_surface_alloc(q);
    if (ret < 0)
        return ret;

    ret = MFXVideoDECODE_Init(q->session, &q->param);
    if (ret < 0)
        ret = ff_qsv_error(ret);

    return ret;
}

static int qsv_reinit(AVCodecContext *c, QSVContext *q)
{
    int ret;

    if ((ret = MFXVideoDECODE_Reset(q->session, &q->param))) {
        av_log(NULL, AV_LOG_ERROR, "Val %d\n", ret);
        return ff_qsv_error(ret);
    }

    qsv_surface_free(q);

    av_log(NULL, AV_LOG_ERROR, "BS %d %p\n", q->bs.DataOffset, q->bs.Data);

    if ((ret = MFXVideoDECODE_Reset(q->session, &q->param))) {
        av_log(NULL, AV_LOG_ERROR, "Reset error %d\n", ret);
    }

    memset(&q->param, 0, sizeof(q->param));

    MFXClose(q->session);

    ret = ff_qsv_init(c, q);

    av_log(NULL, AV_LOG_ERROR, "BS %d %p\n", q->bs.DataOffset, q->bs.Data);

    q->reinit = 0;

    av_log(NULL, AV_LOG_ERROR, "Reinit complete %d %d\n", q->last_ret, ret);
    return 0;
}

static int bitstream_realloc(mfxBitstream *bs, int size)
{
    uint8_t *tmp;

    if (bs->MaxLength >= size)
        return 0;

    tmp = av_realloc(bs->Data, size);
    if (!tmp) {
        av_freep(&bs->Data);
        return AVERROR(ENOMEM);
    }

    bs->Data      = tmp;
    bs->MaxLength = size;

    return 0;
}

static int bitstream_enqueue(mfxBitstream *bs, uint8_t *data, int size)
{
    int bs_size = bs->DataLength + size;
    int ret;

    if ((ret = bitstream_realloc(bs, bs_size)) < 0)
        return ret;

    if (bs_size > bs->MaxLength - bs->DataOffset) {
        memmove(bs->Data, bs->Data + bs->DataOffset, bs->DataLength);
        bs->DataOffset = 0;
    }

    memcpy(bs->Data + bs->DataOffset + bs->DataLength, data, size);

    bs->DataLength += size;

    return 0;
}

static mfxFrameSurface1 *get_surface(QSVContext *q)
{
    int i;
    for (i = 0; i < q->nb_surfaces; i++) {
        if (!q->surfaces[i].Data.Locked)
            break;
    }
    if (i == q->nb_surfaces) {
        av_log(NULL, AV_LOG_INFO, "No surfaces!\n");
        return NULL;
    }
    return q->surfaces + i;
}

static int get_dts(QSVContext *q, int64_t pts,  int64_t *dts)
{
    int i;

    if (pts == AV_NOPTS_VALUE) {
        *dts = AV_NOPTS_VALUE;
        return 0;
    }

    for (i = 0; i < q->nb_surfaces; i++) {
        if (q->pts[i] == pts)
            break;
    }

    if (i == q->nb_surfaces) {
        *dts = AV_NOPTS_VALUE;
        return 0;
    }

    *dts = q->dts[i];

    q->pts[i] = AV_NOPTS_VALUE;

    return 0;
}

static int put_dts(QSVContext *q, int64_t pts, int64_t dts)
{
    int i;

    if (pts == AV_NOPTS_VALUE)
        return 0;

    for (i = 0; i < q->nb_surfaces; i++) {
        if (q->pts[i] == AV_NOPTS_VALUE)
            break;
    }

    if (i == q->nb_surfaces) {
        av_log(q, AV_LOG_INFO, "No slot available.\n");
        return AVERROR_BUG;
    }

    q->pts[i] = pts;
    q->dts[i] = dts;

    return 0;
}

int ff_qsv_decode(AVCodecContext *avctx, QSVContext *q,
                  AVFrame *frame, int *got_frame,
                  AVPacket *avpkt)
{
    mfxFrameSurface1 *insurf;
    mfxFrameSurface1 *outsurf;
    mfxSyncPoint sync;
    mfxBitstream *bs = &q->bs;
    int size         = avpkt->size;
    int ret, i;

    *got_frame = 0;

    if (size)
        ff_packet_list_put(&q->pending, &q->pending_end, avpkt);

    ret = q->last_ret;
    do {
        if (ret == MFX_ERR_MORE_DATA) {
            if (q->pending) {
                AVPacket pkt = { 0 };

                ff_packet_list_get(&q->pending, &q->pending_end, &pkt);

                if (!(ret = put_dts(q, pkt.pts, pkt.dts))) {
                    q->bs.TimeStamp = pkt.pts;

                    ret = bitstream_enqueue(&q->bs, pkt.data, pkt.size);
                }

                av_packet_unref(&pkt);

                if (ret < 0) {
                    q->last_ret = MFX_ERR_MORE_DATA;
                    return ret;
                }
            } else if (!size && bs) {
                // Flush cached frames when EOF
                av_log(NULL, AV_LOG_ERROR, "Flushing\n");
                bs = NULL;
            } else {
                av_log(NULL, AV_LOG_ERROR, "No pending packets\n");
                return size;
            }

            if (q->reinit) {
                // We are done flushing the decoded frame queue
                if (q->reinit) {
                    av_log(NULL, AV_LOG_ERROR, "Calling reinit\n");
                    if ((ret = qsv_reinit(avctx, q)) < 0)
                        return ret;
                }

                return size;
            }
        }

        if (q->reinit) // Flush the frame queue
            bs = NULL;

        if (!(insurf = get_surface(q)))
            break;

        ret = MFXVideoDECODE_DecodeFrameAsync(q->session, bs,
                                              insurf, &outsurf, &sync);

        av_log(NULL, AV_LOG_INFO, "DECODE %d Reinit %d\n", ret, q->reinit);

    } while (ret == MFX_ERR_MORE_SURFACE || ret == MFX_ERR_MORE_DATA);

    q->last_ret = ret;

    switch (ret) {
    case MFX_ERR_MORE_DATA:
        ret = 0;
        break;
    case MFX_WRN_INCOMPATIBLE_VIDEO_PARAM:
        av_log(NULL, AV_LOG_ERROR, "Incompatible %ld\n", avpkt->pts);
        ret = 0;
        break;
    case MFX_WRN_VIDEO_PARAM_CHANGED:
        av_log(NULL, AV_LOG_ERROR, "Changed! %ld\n", avpkt->pts);
        ret = 0;
        break;
    case MFX_WRN_DEVICE_BUSY:
        return AVERROR(EAGAIN);
    case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:
        av_log(NULL, AV_LOG_ERROR, "Resetting! %d\n", ret);
        q->reinit = 1;
        return size;
    default:
        av_log(NULL, AV_LOG_ERROR, "Got %d! %ld\n",ret, avpkt->pts);
        break;
    }

    if (sync) {
        int64_t dts;

        MFXVideoCORE_SyncOperation(q->session, sync, 60000);

        if ((ret = get_dts(q, outsurf->Data.TimeStamp, &dts)) < 0)
            return ret;

        if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
            return ret;
        }

        *got_frame = 1;

        frame->pkt_pts = frame->pts = outsurf->Data.TimeStamp;
        frame->pkt_dts = dts;

        frame->repeat_pict =
            outsurf->Info.PicStruct & MFX_PICSTRUCT_FRAME_TRIPLING ? 4 :
            outsurf->Info.PicStruct & MFX_PICSTRUCT_FRAME_DOUBLING ? 2 :
            outsurf->Info.PicStruct & MFX_PICSTRUCT_FIELD_REPEATED ? 1 : 0;
        frame->top_field_first =
            outsurf->Info.PicStruct & MFX_PICSTRUCT_FIELD_TFF;
        frame->interlaced_frame =
            !(outsurf->Info.PicStruct & MFX_PICSTRUCT_PROGRESSIVE);

        for (i = 0; i < frame->height; i++) {
            memcpy(frame->data[0] + frame->linesize[0] * i,
                   outsurf->Data.Y + outsurf->Data.Pitch * i,
                   FFMIN(outsurf->Data.Pitch, frame->linesize[0]));
        }
        for (i = 0; i < frame->height/2; i++) {
            memcpy(frame->data[1] + frame->linesize[1] * i,
                   outsurf->Data.UV + outsurf->Data.Pitch * i,
                   FFMIN(outsurf->Data.Pitch, frame->linesize[1]));
        }
    }

    if (ret < 0)
        return ff_qsv_error(ret);

    return size;
}

int ff_qsv_flush(QSVContext *q)
{
    int ret;

    if ((ret = MFXVideoDECODE_Reset(q->session, &q->param)))
        ret = ff_qsv_error(ret);

    q->bs.DataOffset = q->bs.DataLength = 0;

    ff_packet_list_free(&q->pending, &q->pending_end);

    return ret;
}

int ff_qsv_close(QSVContext *q)
{
    int ret = MFXClose(q->session);

    qsv_surface_free(q);

    ff_packet_list_free(&q->pending, &q->pending_end);

    return ff_qsv_error(ret);
}
