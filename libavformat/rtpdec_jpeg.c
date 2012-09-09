/*
 * RTP JPEG-compressed Video Depacketizer, RFC 2435
 * Copyright (c) 2012 Samuel Pitoiset
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

#include "avformat.h"
#include "rtpdec_formats.h"
#include "libavutil/intreadwrite.h"
#include "libavcodec/mjpeg.h"

/**
 * RTP/JPEG specific private data.
 */
struct PayloadContext {
    AVIOContext *frame;         ///< current frame buffer
    uint32_t    timestamp;      ///< current frame timestamp
    int         hdr_size;       ///< size of the current frame header
};

static PayloadContext *jpeg_new_context(void)
{
    return av_mallocz(sizeof(PayloadContext));
}

static inline void free_frame_if_needed(PayloadContext *jpeg)
{
    if (jpeg->frame) {
        uint8_t *p;
        avio_close_dyn_buf(jpeg->frame, &p);
        av_free(p);
        jpeg->frame = NULL;
    }
}

static void jpeg_free_context(PayloadContext *jpeg)
{
    free_frame_if_needed(jpeg);
    av_free(jpeg);
}

static void jpeg_create_huffman_table(PutBitContext *p, int table_class,
                                      int table_id, const uint8_t *bits_table,
                                      const uint8_t *value_table)
{
    int i, n = 0;

    put_bits(p, 8, 0);
    put_bits(p, 4, table_class);
    put_bits(p, 4, table_id);

    for (i = 1; i <= 16; i++) {
        n += bits_table[i];
        put_bits(p, 8, bits_table[i]);
    }

    for (i = 0; i < n; i++) {
        put_bits(p, 8, value_table[i]);
    }
}

static int jpeg_create_header(uint8_t *buf, int size, uint32_t type, uint32_t w,
                              uint32_t h, const uint8_t *qtable, int nb_qtable)
{
    PutBitContext pbc;

    init_put_bits(&pbc, buf, size);

    /* Convert from blocks to pixels. */
    w <<= 3;
    h <<= 3;

    /* SOI */
    put_marker(&pbc, SOI);

    /* JFIF header */
    put_marker(&pbc, APP0);
    put_bits(&pbc, 16, 16);
    avpriv_put_string(&pbc, "JFIF", 1);
    put_bits(&pbc, 16, 0x0201);
    put_bits(&pbc, 8, 0);
    put_bits(&pbc, 16, 1);
    put_bits(&pbc, 16, 1);
    put_bits(&pbc, 8, 0);
    put_bits(&pbc, 8, 0);

    /* DQT */
    put_marker(&pbc, DQT);
    if (nb_qtable == 2) {
        put_bits(&pbc, 16, 2 + 2 * (1 + 64));
    } else {
        put_bits(&pbc, 16, 2 + 1 * (1 + 64));
    }
    put_bits(&pbc, 8, 0);

    /* Each table is an array of 64 values given in zig-zag
     * order, identical to the format used in a JFIF DQT
     * marker segment. */
    avpriv_copy_bits(&pbc, qtable, 64 * 8);

    if (nb_qtable == 2) {
        put_bits(&pbc, 8, 1);
        avpriv_copy_bits(&pbc, qtable + 64, 64 * 8);
    }

    /* DHT */
    put_marker(&pbc, DHT);

    jpeg_create_huffman_table(&pbc, 0, 0, avpriv_mjpeg_bits_dc_luminance,
                              avpriv_mjpeg_val_dc);
    jpeg_create_huffman_table(&pbc, 0, 1, avpriv_mjpeg_bits_dc_chrominance,
                              avpriv_mjpeg_val_dc);
    jpeg_create_huffman_table(&pbc, 1, 0, avpriv_mjpeg_bits_ac_luminance,
                              avpriv_mjpeg_val_ac_luminance);
    jpeg_create_huffman_table(&pbc, 1, 1, avpriv_mjpeg_bits_ac_chrominance,
                              avpriv_mjpeg_val_ac_chrominance);

    /* SOF0 */
    put_marker(&pbc, SOF0);
    put_bits(&pbc, 16, 17);
    put_bits(&pbc, 8, 8);
    put_bits(&pbc, 8, h >> 8);
    put_bits(&pbc, 8, h);
    put_bits(&pbc, 8, w >> 8);
    put_bits(&pbc, 8, w);
    put_bits(&pbc, 8, 3);
    put_bits(&pbc, 8, 1);
    put_bits(&pbc, 8, type ? 34 : 33);
    put_bits(&pbc, 8, 0);
    put_bits(&pbc, 8, 2);
    put_bits(&pbc, 8, 17);
    put_bits(&pbc, 8, nb_qtable == 2 ? 1 : 0);
    put_bits(&pbc, 8, 3);
    put_bits(&pbc, 8, 17);
    put_bits(&pbc, 8, nb_qtable == 2 ? 1 : 0);

    /* SOS */
    put_marker(&pbc, SOS);
    put_bits(&pbc, 16, 12);
    put_bits(&pbc, 8, 3);
    put_bits(&pbc, 8, 1);
    put_bits(&pbc, 8, 0);
    put_bits(&pbc, 8, 2);
    put_bits(&pbc, 8, 17);
    put_bits(&pbc, 8, 3);
    put_bits(&pbc, 8, 17);
    put_bits(&pbc, 8, 0);
    put_bits(&pbc, 8, 63);
    put_bits(&pbc, 8, 0);

    /* Fill the buffer. */
    flush_put_bits(&pbc);

    /* Return the length in bytes of the JPEG header. */
    return put_bits_count(&pbc) / 8;
}

static int jpeg_parse_packet(AVFormatContext *ctx, PayloadContext *jpeg,
                             AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                             const uint8_t *buf, int len, int flags)
{
    uint8_t type, q, width, height;
    const uint8_t *qtables = NULL;
    uint16_t qtable_len;
    uint32_t off;
    int ret;

    if (len < 8) {
        av_log(ctx, AV_LOG_ERROR, "Too short RTP/JPEG packet.\n");
        return AVERROR_INVALIDDATA;
    }

    /* Parse the main JPEG header. */
    off    = AV_RB24(buf + 1);  /* fragment byte offset */
    type   = AV_RB8(buf + 4);   /* id of jpeg decoder params */
    q      = AV_RB8(buf + 5);   /* quantization factor (or table id) */
    width  = AV_RB8(buf + 6);   /* frame width in 8 pixel blocks */
    height = AV_RB8(buf + 7);   /* frame height in 8 pixel blocks */
    buf += 8;
    len -= 8;

    /* Parse the restart marker header. */
    if (type > 63) {
        av_log(ctx, AV_LOG_ERROR,
               "Unimplemented RTP/JPEG restart marker header.\n");
        return AVERROR_PATCHWELCOME;
    }

    /* Parse the quantization table header. */
    if (q > 127 && off == 0) {
        uint8_t precision;

        if (len < 4) {
            av_log(ctx, AV_LOG_ERROR, "Too short RTP/JPEG packet.\n");
            return AVERROR_INVALIDDATA;
        }

        /* The first byte is reserved for future use. */
        precision  = AV_RB8(buf + 1);    /* size of coefficients */
        qtable_len = AV_RB16(buf + 2);   /* length in bytes */
        buf += 4;
        len -= 4;

        if (precision)
            av_log(ctx, AV_LOG_WARNING, "Only 8-bit precision is supported.\n");

        if (q == 255 && qtable_len == 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "Invalid RTP/JPEG packet. Quantization tables not found.\n");
            return AVERROR_INVALIDDATA;
        }

        if (qtable_len > 0) {
            if (len < qtable_len) {
                av_log(ctx, AV_LOG_ERROR, "Too short RTP/JPEG packet.\n");
                return AVERROR_INVALIDDATA;
            }
            qtables = buf;
            buf += qtable_len;
            len -= qtable_len;
        }
    }

    if (off == 0) {
        /* Start of JPEG data packet. */
        uint8_t hdr[1024];

        /* Skip the current frame in case of the end packet
         * has been lost somewhere. */
        free_frame_if_needed(jpeg);

        if ((ret = avio_open_dyn_buf(&jpeg->frame)) < 0)
            return ret;
        jpeg->timestamp = *timestamp;

        if (!qtables) {
            av_log(ctx, AV_LOG_ERROR,
                   "Unimplemented default quantization tables.\n");
            return AVERROR_PATCHWELCOME;
        }

        /* Generate a frame and scan headers that can be prepended to the
         * RTP/JPEG data payload to produce a JPEG compressed image in
         * interchange format. */
        jpeg->hdr_size = jpeg_create_header(hdr, sizeof(hdr), type, width,
                                            height, qtables,
                                            qtable_len > 64 ? 2 : 1);

        /* Copy JPEG header to frame buffer. */
        avio_write(jpeg->frame, hdr, jpeg->hdr_size);
    }

    if (!jpeg->frame) {
        av_log(ctx, AV_LOG_ERROR,
               "Received packet without a start chunk; dropping frame.\n");
        return AVERROR(EAGAIN);
    }

    if (jpeg->timestamp != *timestamp) {
        /* Skip the current frame if timestamp is incorrect.
         * A start packet has been lost somewhere. */
        free_frame_if_needed(jpeg);
        av_log(ctx, AV_LOG_ERROR, "RTP timestamps don't match.\n");
        return AVERROR_INVALIDDATA;
    }

    if (off != avio_tell(jpeg->frame) - jpeg->hdr_size) {
        av_log(ctx, AV_LOG_ERROR,
               "Missing packets; dropping frame.\n");
        return AVERROR(EAGAIN);
    }

    /* Copy data to frame buffer. */
    avio_write(jpeg->frame, buf, len);

    if (flags & RTP_FLAG_MARKER) {
        /* End of JPEG data packet. */
        PutBitContext pbc;
        uint8_t buf[2];

        /* Put EOI marker. */
        init_put_bits(&pbc, buf, sizeof(buf));
        put_marker(&pbc, EOI);
        flush_put_bits(&pbc);
        avio_write(jpeg->frame, buf, sizeof(buf));

        /* Prepare the JPEG packet. */
        av_init_packet(pkt);
        pkt->size = avio_close_dyn_buf(jpeg->frame, &pkt->data);
        if (pkt->size < 0) {
            av_log(ctx, AV_LOG_ERROR,
                   "Error occured when getting frame buffer.\n");
            jpeg->frame = NULL;
            return pkt->size;
        }
        pkt->stream_index = st->index;
        pkt->destruct     = av_destruct_packet;

        /* Re-init the frame buffer. */
        jpeg->frame = NULL;

        return 0;
    }

    return AVERROR(EAGAIN);
}

RTPDynamicProtocolHandler ff_jpeg_dynamic_handler = {
    .enc_name          = "JPEG",
    .codec_type        = AVMEDIA_TYPE_VIDEO,
    .codec_id          = AV_CODEC_ID_MJPEG,
    .alloc             = jpeg_new_context,
    .free              = jpeg_free_context,
    .parse_packet      = jpeg_parse_packet,
    .static_payload_id = 26,
};