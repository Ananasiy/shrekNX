// Видео Bink: кадры декодирует libavcodec (декодер bink), здесь — подача пакетов и перевод YUV 4:2:0
// (ограниченный диапазон, BT.601; у файлов с альфой — плоскость A) в формат поверхности BinkCopyToBuffer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

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
    if (!w || !h || w > 16384u || h > 16384u)
        return NULL;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_BINKVIDEO);
    if (!codec)
        return NULL;
    bink_video *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    v->ctx = avcodec_alloc_context3(codec);
    if (!v->ctx) {
        free(v);
        return NULL;
    }
    v->ctx->width = (int)w;
    v->ctx->height = (int)h;
    v->ctx->codec_tag = signature;  // 4-й байт — ревизия формата
    v->ctx->extradata = av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!v->ctx->extradata) {
        avcodec_free_context(&v->ctx);
        free(v);
        return NULL;
    }
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
    if (!v->pkt || !v->frame) {
        bink_video_free(v);
        return NULL;
    }
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
    if (!v || !data || size > 0x7FFFFFFFu || av_new_packet(v->pkt, (int)size) < 0)
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

static inline u8 clamp8(int x) { return (u8)(x < 0 ? 0 : x > 255 ? 255 : x); }

#if defined(__aarch64__)
// Точная BT.601 (побитово совпадает со скалярной формулой): 16 пикселей за итерацию.
// c = (y - 16) * 298, d = u - 128, e = v - 128;
// r = clamp8((c + 409*e + 128) >> 8), g = clamp8((c - 100*d - 208*e + 128) >> 8), b = clamp8((c + 516*d + 128) >> 8).
static inline void yuv420_neon_16(const u8 *yp, const u8 *up, const u8 *vp,
                                  uint8x16_t *out_r, uint8x16_t *out_g, uint8x16_t *out_b)
{
    const int32x4_t v128 = vdupq_n_s32(128);
    const int16x8_t v16 = vdupq_n_s16(16);
    const int16x8_t v128_16 = vdupq_n_s16(128);

    // 8 пар хромы U/V (по одной на каждые 2 пикселя по горизонтали)
    uint8x8_t u8v = vld1_u8(up);
    uint8x8_t v8v = vld1_u8(vp);
    int16x8_t d16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u8v)), v128_16);
    int16x8_t e16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v8v)), v128_16);

    int16x4_t d_lo = vget_low_s16(d16);
    int16x4_t e_lo = vget_low_s16(e16);

    int32x4_t r_uv_lo = vmlal_n_s16(v128, e_lo, 409);
    int32x4_t r_uv_hi = vmlal_high_n_s16(v128, e16, 409);

    int32x4_t g_uv_lo = vmlsl_n_s16(vmlsl_n_s16(v128, d_lo, 100), e_lo, 208);
    int32x4_t g_uv_hi = vmlsl_high_n_s16(vmlsl_high_n_s16(v128, d16, 100), e16, 208);

    int32x4_t b_uv_lo = vmlal_n_s16(v128, d_lo, 516);
    int32x4_t b_uv_hi = vmlal_high_n_s16(v128, d16, 516);

    // 16 пикселей яркости Y
    uint8x16_t y16 = vld1q_u8(yp);
    int16x8_t y_lo = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(y16))), v16);
    int16x8_t y_hi = vsubq_s16(vreinterpretq_s16_u16(vmovl_high_u8(y16)), v16);

    int32x4_t c0 = vmull_n_s16(vget_low_s16(y_lo), 298);
    int32x4_t c1 = vmull_high_n_s16(y_lo, 298);
    int32x4_t c2 = vmull_n_s16(vget_low_s16(y_hi), 298);
    int32x4_t c3 = vmull_high_n_s16(y_hi, 298);

    // дублируем хрома-вклады по парам пикселей, складываем с Y, арифметически сдвигаем >> 8 и сужаем с насыщением
    int16x8_t r_lo = vcombine_s16(vshrn_n_s32(vaddq_s32(c0, vzip1q_s32(r_uv_lo, r_uv_lo)), 8),
                                  vshrn_n_s32(vaddq_s32(c1, vzip2q_s32(r_uv_lo, r_uv_lo)), 8));
    int16x8_t r_hi = vcombine_s16(vshrn_n_s32(vaddq_s32(c2, vzip1q_s32(r_uv_hi, r_uv_hi)), 8),
                                  vshrn_n_s32(vaddq_s32(c3, vzip2q_s32(r_uv_hi, r_uv_hi)), 8));
    *out_r = vqmovun_high_s16(vqmovun_s16(r_lo), r_hi);

    int16x8_t g_lo = vcombine_s16(vshrn_n_s32(vaddq_s32(c0, vzip1q_s32(g_uv_lo, g_uv_lo)), 8),
                                  vshrn_n_s32(vaddq_s32(c1, vzip2q_s32(g_uv_lo, g_uv_lo)), 8));
    int16x8_t g_hi = vcombine_s16(vshrn_n_s32(vaddq_s32(c2, vzip1q_s32(g_uv_hi, g_uv_hi)), 8),
                                  vshrn_n_s32(vaddq_s32(c3, vzip2q_s32(g_uv_hi, g_uv_hi)), 8));
    *out_g = vqmovun_high_s16(vqmovun_s16(g_lo), g_hi);

    int16x8_t b_lo = vcombine_s16(vshrn_n_s32(vaddq_s32(c0, vzip1q_s32(b_uv_lo, b_uv_lo)), 8),
                                  vshrn_n_s32(vaddq_s32(c1, vzip2q_s32(b_uv_lo, b_uv_lo)), 8));
    int16x8_t b_hi = vcombine_s16(vshrn_n_s32(vaddq_s32(c2, vzip1q_s32(b_uv_hi, b_uv_hi)), 8),
                                  vshrn_n_s32(vaddq_s32(c3, vzip2q_s32(b_uv_hi, b_uv_hi)), 8));
    *out_b = vqmovun_high_s16(vqmovun_s16(b_lo), b_hi);
}
#endif

// Специализированные строковые конвертеры без switch внутри цикла по x
static void row_bgra(const u8 *yp, const u8 *up, const u8 *vp, const u8 *al, u8 *o, int w)
{
    int x = 0;
#if defined(__aarch64__)
    const uint8x16_t a255 = vdupq_n_u8(255);
    if (al) {
        for (; x + 16 <= w; x += 16, o += 64) {
            uint8x16_t vr, vg, vb;
            yuv420_neon_16(yp + x, up + (x >> 1), vp + (x >> 1), &vr, &vg, &vb);
            uint8x16x4_t px = {{vb, vg, vr, vld1q_u8(al + x)}};
            vst4q_u8(o, px);
        }
    } else {
        for (; x + 16 <= w; x += 16, o += 64) {
            uint8x16_t vr, vg, vb;
            yuv420_neon_16(yp + x, up + (x >> 1), vp + (x >> 1), &vr, &vg, &vb);
            uint8x16x4_t px = {{vb, vg, vr, a255}};
            vst4q_u8(o, px);
        }
    }
#endif
    for (; x < w; x++, o += 4) {
        int c = (yp[x] - 16) * 298, d = up[x >> 1] - 128, e = vp[x >> 1] - 128;
        o[0] = clamp8((c + 516 * d + 128) >> 8);
        o[1] = clamp8((c - 100 * d - 208 * e + 128) >> 8);
        o[2] = clamp8((c + 409 * e + 128) >> 8);
        o[3] = al ? al[x] : 255;
    }
}

static void row_rgba(const u8 *yp, const u8 *up, const u8 *vp, const u8 *al, u8 *o, int w)
{
    int x = 0;
#if defined(__aarch64__)
    const uint8x16_t a255 = vdupq_n_u8(255);
    if (al) {
        for (; x + 16 <= w; x += 16, o += 64) {
            uint8x16_t vr, vg, vb;
            yuv420_neon_16(yp + x, up + (x >> 1), vp + (x >> 1), &vr, &vg, &vb);
            uint8x16x4_t px = {{vr, vg, vb, vld1q_u8(al + x)}};
            vst4q_u8(o, px);
        }
    } else {
        for (; x + 16 <= w; x += 16, o += 64) {
            uint8x16_t vr, vg, vb;
            yuv420_neon_16(yp + x, up + (x >> 1), vp + (x >> 1), &vr, &vg, &vb);
            uint8x16x4_t px = {{vr, vg, vb, a255}};
            vst4q_u8(o, px);
        }
    }
#endif
    for (; x < w; x++, o += 4) {
        int c = (yp[x] - 16) * 298, d = up[x >> 1] - 128, e = vp[x >> 1] - 128;
        o[0] = clamp8((c + 409 * e + 128) >> 8);
        o[1] = clamp8((c - 100 * d - 208 * e + 128) >> 8);
        o[2] = clamp8((c + 516 * d + 128) >> 8);
        o[3] = al ? al[x] : 255;
    }
}

static void row_bgr24(const u8 *yp, const u8 *up, const u8 *vp, const u8 *al, u8 *o, int w)
{
    (void)al;
    int x = 0;
#if defined(__aarch64__)
    for (; x + 16 <= w; x += 16, o += 48) {
        uint8x16_t vr, vg, vb;
        yuv420_neon_16(yp + x, up + (x >> 1), vp + (x >> 1), &vr, &vg, &vb);
        uint8x16x3_t px = {{vb, vg, vr}};
        vst3q_u8(o, px);
    }
#endif
    for (; x < w; x++, o += 3) {
        int c = (yp[x] - 16) * 298, d = up[x >> 1] - 128, e = vp[x >> 1] - 128;
        o[0] = clamp8((c + 516 * d + 128) >> 8);
        o[1] = clamp8((c - 100 * d - 208 * e + 128) >> 8);
        o[2] = clamp8((c + 409 * e + 128) >> 8);
    }
}

static void row_rgb24(const u8 *yp, const u8 *up, const u8 *vp, const u8 *al, u8 *o, int w)
{
    (void)al;
    int x = 0;
#if defined(__aarch64__)
    for (; x + 16 <= w; x += 16, o += 48) {
        uint8x16_t vr, vg, vb;
        yuv420_neon_16(yp + x, up + (x >> 1), vp + (x >> 1), &vr, &vg, &vb);
        uint8x16x3_t px = {{vr, vg, vb}};
        vst3q_u8(o, px);
    }
#endif
    for (; x < w; x++, o += 3) {
        int c = (yp[x] - 16) * 298, d = up[x >> 1] - 128, e = vp[x >> 1] - 128;
        o[0] = clamp8((c + 409 * e + 128) >> 8);
        o[1] = clamp8((c - 100 * d - 208 * e + 128) >> 8);
        o[2] = clamp8((c + 516 * d + 128) >> 8);
    }
}

static void row_565(const u8 *yp, const u8 *up, const u8 *vp, const u8 *al, u8 *o, int w)
{
    (void)al;
    for (int x = 0; x < w; x++, o += 2) {
        int c = (yp[x] - 16) * 298, d = up[x >> 1] - 128, e = vp[x >> 1] - 128;
        u8 r = clamp8((c + 409 * e + 128) >> 8);
        u8 g = clamp8((c - 100 * d - 208 * e + 128) >> 8);
        u8 b = clamp8((c + 516 * d + 128) >> 8);
        u16 p = (u16)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        memcpy(o, &p, 2);
    }
}

static void row_555(const u8 *yp, const u8 *up, const u8 *vp, const u8 *al, u8 *o, int w)
{
    (void)al;
    for (int x = 0; x < w; x++, o += 2) {
        int c = (yp[x] - 16) * 298, d = up[x >> 1] - 128, e = vp[x >> 1] - 128;
        u8 r = clamp8((c + 409 * e + 128) >> 8);
        u8 g = clamp8((c - 100 * d - 208 * e + 128) >> 8);
        u8 b = clamp8((c + 516 * d + 128) >> 8);
        u16 p = (u16)((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
        memcpy(o, &p, 2);
    }
}

static void row_5551(const u8 *yp, const u8 *up, const u8 *vp, const u8 *al, u8 *o, int w)
{
    for (int x = 0; x < w; x++, o += 2) {
        int c = (yp[x] - 16) * 298, d = up[x >> 1] - 128, e = vp[x >> 1] - 128;
        u8 r = clamp8((c + 409 * e + 128) >> 8);
        u8 g = clamp8((c - 100 * d - 208 * e + 128) >> 8);
        u8 b = clamp8((c + 516 * d + 128) >> 8);
        u8 a = al ? al[x] : 255;
        u16 p = (u16)((a >> 7) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
        memcpy(o, &p, 2);
    }
}

static void row_4444(const u8 *yp, const u8 *up, const u8 *vp, const u8 *al, u8 *o, int w)
{
    for (int x = 0; x < w; x++, o += 2) {
        int c = (yp[x] - 16) * 298, d = up[x >> 1] - 128, e = vp[x >> 1] - 128;
        u8 r = clamp8((c + 409 * e + 128) >> 8);
        u8 g = clamp8((c - 100 * d - 208 * e + 128) >> 8);
        u8 b = clamp8((c + 516 * d + 128) >> 8);
        u8 a = al ? al[x] : 255;
        u16 p = (u16)((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
        memcpy(o, &p, 2);
    }
}

typedef void (*row_fn)(const u8 *yp, const u8 *up, const u8 *vp, const u8 *al, u8 *o, int w);

// Типы поверхностей bink.h: 1 BINKSURFACE24 (B,G,R), 2 24R (R,G,B), 3 32 (B,G,R,x), 4 32R (R,G,B,x),
// 5 32A (B,G,R,A), 6 32RA (R,G,B,A), 7 4444, 8 5551, 9 555, 10 565 (11 655 и 12 664 — как 565)
void bink_video_copy(bink_video *v, u8 *dst, u32 pitch, u32 rows, u32 x0, u32 y0, u32 surface)
{
    if (!v || !v->have || !dst)
        return;
    const AVFrame *f = v->frame;
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVA420P)
        return;
    int w = f->width, h = f->height;
    if (w <= 0 || h <= 0 || rows == 0)
        return;
    if ((u32)h > rows)
        h = (int)rows;
    const u8 *ap = f->format == AV_PIX_FMT_YUVA420P ? f->data[3] : NULL;
    u32 type = surface & 15u;
    u32 bpp = 4u;
    row_fn fn = row_bgra;
    switch (type) {
    case 3: case 5: fn = row_bgra; bpp = 4u; break;
    case 4: case 6: fn = row_rgba; bpp = 4u; break;
    case 1: fn = row_bgr24; bpp = 3u; break;
    case 2: fn = row_rgb24; bpp = 3u; break;
    case 10: case 11: case 12: fn = row_565; bpp = 2u; break;
    case 9: fn = row_555; bpp = 2u; break;
    case 8: fn = row_5551; bpp = 2u; break;
    case 7: fn = row_4444; bpp = 2u; break;
    default: fn = row_bgra; bpp = 4u; break;
    }
    for (int y = 0; y < h; y++) {
        const u8 *yp = f->data[0] + y * f->linesize[0];
        const u8 *up = f->data[1] + (y >> 1) * f->linesize[1];
        const u8 *vp = f->data[2] + (y >> 1) * f->linesize[2];
        const u8 *al = ap ? ap + y * f->linesize[3] : NULL;
        u8 *o = dst + ((size_t)y0 + (size_t)y) * pitch + (size_t)x0 * bpp;
        fn(yp, up, vp, al, o, w);
    }
}
