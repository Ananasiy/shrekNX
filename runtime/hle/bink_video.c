// Видео Bink: кадры декодирует libavcodec (декодер bink), здесь — подача пакетов и перевод YUV 4:2:0
// (ограниченный диапазон, BT.601; у файлов с альфой — плоскость A) в формат поверхности BinkCopyToBuffer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>

#include "runtime/hle/bink.h"

struct bink_video {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    int have;  // в frame есть декодированный кадр
};

bink_video *bink_video_new(u32 signature, u32 video_flags, u32 w, u32 h)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_BINKVIDEO);
    if (!codec)
        return NULL;
    bink_video *v = calloc(1, sizeof *v);
    v->ctx = avcodec_alloc_context3(codec);
    v->ctx->width = (int)w;
    v->ctx->height = (int)h;
    v->ctx->codec_tag = signature;  // 4-й байт — ревизия формата
    v->ctx->extradata = av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(v->ctx->extradata, &video_flags, 4);
    v->ctx->extradata_size = 4;
    v->ctx->thread_count = 1;
    if (avcodec_open2(v->ctx, codec, NULL) < 0) {
        avcodec_free_context(&v->ctx);
        free(v);
        return NULL;
    }
    v->pkt = av_packet_alloc();
    v->frame = av_frame_alloc();
    return v;
}

void bink_video_free(bink_video *v)
{
    if (!v)
        return;
    av_frame_free(&v->frame);
    av_packet_free(&v->pkt);
    avcodec_free_context(&v->ctx);
    free(v);
}

void bink_video_flush(bink_video *v)
{
    if (v)
        avcodec_flush_buffers(v->ctx);
}

int bink_video_decode(bink_video *v, const u8 *data, u32 size, int key)
{
    if (!v || av_new_packet(v->pkt, (int)size) < 0)
        return -1;
    memcpy(v->pkt->data, data, size);
    if (key)
        v->pkt->flags |= AV_PKT_FLAG_KEY;
    int r = avcodec_send_packet(v->ctx, v->pkt);
    av_packet_unref(v->pkt);
    if (r >= 0)
        r = avcodec_receive_frame(v->ctx, v->frame);
    if (r < 0) {
        static int reported;
        if (reported++ < 4)
            fprintf(stderr, "[bink] video decode error %d (%u bytes)\n", r, size);
        return r;
    }
    v->have = 1;
    return 0;
}

static u8 clamp8(int x) { return (u8)(x < 0 ? 0 : x > 255 ? 255 : x); }

// Типы поверхностей bink.h: 1 BINKSURFACE24 (B,G,R), 2 24R (R,G,B), 3 32 (B,G,R,x), 4 32R (R,G,B,x),
// 5 32A (B,G,R,A), 6 32RA (R,G,B,A), 7 4444, 8 5551, 9 555, 10 565 (11 655 и 12 664 — как 565)
void bink_video_copy(bink_video *v, u8 *dst, u32 pitch, u32 rows, u32 x0, u32 y0, u32 surface)
{
    if (!v || !v->have)
        return;
    const AVFrame *f = v->frame;
    int w = f->width, h = f->height;
    if ((u32)h > rows)
        h = (int)rows;
    const u8 *ap = f->format == AV_PIX_FMT_YUVA420P ? f->data[3] : NULL;
    u32 type = surface & 15, bpp = type == 1 || type == 2 ? 3 : type >= 7 && type <= 12 ? 2 : 4;
    for (int y = 0; y < h; y++) {
        const u8 *yp = f->data[0] + y * f->linesize[0];
        const u8 *up = f->data[1] + (y >> 1) * f->linesize[1];
        const u8 *vp = f->data[2] + (y >> 1) * f->linesize[2];
        const u8 *al = ap ? ap + y * f->linesize[3] : NULL;
        u8 *o = dst + (y0 + (u32)y) * pitch + x0 * bpp;
        for (int x = 0; x < w; x++, o += bpp) {
            int c = (yp[x] - 16) * 298, d = up[x >> 1] - 128, e = vp[x >> 1] - 128;
            u8 r = clamp8((c + 409 * e + 128) >> 8);
            u8 g = clamp8((c - 100 * d - 208 * e + 128) >> 8);
            u8 b = clamp8((c + 516 * d + 128) >> 8);
            u8 a = al ? al[x] : 255;
            switch (type) {
            case 3: case 5: o[0] = b; o[1] = g; o[2] = r; o[3] = a; break;
            case 4: case 6: o[0] = r; o[1] = g; o[2] = b; o[3] = a; break;
            case 1: o[0] = b; o[1] = g; o[2] = r; break;
            case 2: o[0] = r; o[1] = g; o[2] = b; break;
            case 10: case 11: case 12: { u16 p = (u16)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); memcpy(o, &p, 2); break; }
            case 9: { u16 p = (u16)((r >> 3) << 10 | (g >> 3) << 5 | b >> 3); memcpy(o, &p, 2); break; }
            case 8: { u16 p = (u16)((a >> 7) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3); memcpy(o, &p, 2); break; }
            case 7: { u16 p = (u16)((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4); memcpy(o, &p, 2); break; }
            default: o[0] = b; o[1] = g; o[2] = r; o[3] = a; break;
            }
        }
    }
}
