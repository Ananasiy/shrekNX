// HLE Direct3D 8: IDirect3DDevice8 — состояние (render/texture-stage states, матрицы, свет, материал,
// viewport), создание ресурсов, вершинные объявления и FVF, вызовы отрисовки (d3d8_ff.c).
// Методы — stdcall с this в ARG(0): RET(n) считает и this.
#include <math.h>
#include <stdlib.h>

#include "platform/window.h"
#include "runtime/hle/d3d8.h"
#include "runtime/hle/d3d8_int.h"
#include "runtime/hle/user32.h"

static d3ddev *g_dev;
static u32 g_vt_dev;

d3ddev *d3d8_dev(void) { return g_dev; }

static float rdf(u32 a)
{
    float f;
    memcpy(&f, g_mem + a, 4);
    return f;
}

static float u2f(u32 v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static u32 f2u(float f)
{
    u32 v;
    memcpy(&v, &f, 4);
    return v;
}

static void identity(float *m)
{
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// ================================================================ состояние по умолчанию (как после CreateDevice/Reset)
enum {
    RS_ZENABLE = 7, RS_FILLMODE = 8, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_LASTPIXEL = 16, RS_SRCBLEND = 19,
    RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAFUNC = 25, RS_FOGEND = 37, RS_FOGDENSITY = 38,
    RS_STENCILFAIL = 53, RS_STENCILZFAIL = 54, RS_STENCILPASS = 55, RS_STENCILFUNC = 56, RS_STENCILMASK = 58,
    RS_STENCILWRITEMASK = 59, RS_TEXTUREFACTOR = 60, RS_CLIPPING = 136, RS_LIGHTING = 137, RS_COLORVERTEX = 141,
    RS_LOCALVIEWER = 142, RS_DIFFUSEMATERIALSOURCE = 145, RS_SPECULARMATERIALSOURCE = 146, RS_POINTSIZE = 154,
    RS_POINTSIZE_MIN = 155, RS_MULTISAMPLEANTIALIAS = 161, RS_MULTISAMPLEMASK = 162, RS_PATCHSEGMENTS = 164,
    RS_POINTSIZE_MAX = 166, RS_COLORWRITEENABLE = 168, RS_BLENDOP = 171, RS_POSITIONORDER = 172,
    RS_NORMALORDER = 173,
};

static void set_defaults(d3ddev *d)
{
    memset(d->rs, 0, sizeof d->rs);
    d->rs[RS_ZENABLE] = d->autodepth ? 1 : 0;
    d->rs[RS_FILLMODE] = 3;
    d->rs[RS_SHADEMODE] = 2;
    d->rs[RS_ZWRITEENABLE] = 1;
    d->rs[RS_LASTPIXEL] = 1;
    d->rs[RS_SRCBLEND] = 2;
    d->rs[RS_DESTBLEND] = 1;
    d->rs[RS_CULLMODE] = 3;
    d->rs[RS_ZFUNC] = 4;
    d->rs[RS_ALPHAFUNC] = 8;
    d->rs[RS_FOGEND] = f2u(1.0f);
    d->rs[RS_FOGDENSITY] = f2u(1.0f);
    d->rs[RS_STENCILFAIL] = d->rs[RS_STENCILZFAIL] = d->rs[RS_STENCILPASS] = 1;
    d->rs[RS_STENCILFUNC] = 8;
    d->rs[RS_STENCILMASK] = d->rs[RS_STENCILWRITEMASK] = 0xFFFFFFFFu;
    d->rs[RS_TEXTUREFACTOR] = 0xFFFFFFFFu;
    d->rs[RS_CLIPPING] = 1;
    d->rs[RS_LIGHTING] = 1;
    d->rs[RS_COLORVERTEX] = 1;
    d->rs[RS_LOCALVIEWER] = 1;
    d->rs[RS_DIFFUSEMATERIALSOURCE] = 1;
    d->rs[RS_SPECULARMATERIALSOURCE] = 2;
    d->rs[RS_POINTSIZE] = f2u(1.0f);
    d->rs[RS_POINTSIZE_MIN] = f2u(1.0f);
    d->rs[RS_POINTSIZE_MAX] = f2u(64.0f);
    d->rs[RS_MULTISAMPLEANTIALIAS] = 1;
    d->rs[RS_MULTISAMPLEMASK] = 0xFFFFFFFFu;
    d->rs[RS_PATCHSEGMENTS] = f2u(1.0f);
    d->rs[RS_COLORWRITEENABLE] = 0xF;
    d->rs[RS_BLENDOP] = 1;
    d->rs[RS_POSITIONORDER] = 3;  // D3DORDER_CUBIC
    d->rs[RS_NORMALORDER] = 1;    // D3DORDER_LINEAR
    for (int s = 0; s < MAX_STAGES; s++) {
        u32 *t = d->tss[s];
        memset(t, 0, 32 * 4);
        t[1] = s == 0 ? 4 : 1;   // COLOROP: MODULATE / DISABLE
        t[2] = 2;                // COLORARG1: TEXTURE
        t[3] = 1;                // COLORARG2: CURRENT
        t[4] = s == 0 ? 2 : 1;   // ALPHAOP: SELECTARG1 / DISABLE
        t[5] = 2;
        t[6] = 1;
        t[11] = (u32)s;          // TEXCOORDINDEX
        t[13] = t[14] = t[25] = 1;  // ADDRESSU/V/W: WRAP
        t[16] = t[17] = 1;       // MAG/MINFILTER: POINT
        t[21] = 1;               // MAXANISOTROPY
        t[26] = t[27] = t[28] = 1;  // COLORARG0, ALPHAARG0, RESULTARG: CURRENT
    }
    identity(d->view);
    identity(d->proj);
    identity(d->world);
    for (int s = 0; s < MAX_STAGES; s++)
        identity(d->texm[s]);
    memset(d->material, 0, sizeof d->material);
    memset(d->light_on, 0, sizeof d->light_on);
    d->vp[0] = d->vp[1] = 0;
    d->vp[2] = d->bb_w;
    d->vp[3] = d->bb_h;
    d->vpz[0] = 0.0f;
    d->vpz[1] = 1.0f;
    d->sampler_dirty = 0xFF;
    d->gen_xf++;
    d->gen_light++;
    d->gen_mat++;
    d->gen_misc++;
}

// ================================================================ вершинные объявления
enum { DT_FLOAT1, DT_FLOAT2, DT_FLOAT3, DT_FLOAT4, DT_D3DCOLOR, DT_UBYTE4, DT_SHORT2, DT_SHORT4 };
static const u8 g_dt_size[8] = {4, 8, 12, 16, 4, 4, 4, 8};

#define MAX_VS 512
typedef struct {
    int used;
    vdecl decl;
    u32 *tokens;
    u32 ntok;
} vshader;
static vshader g_vs[MAX_VS];
#define VS_HANDLE_BASE 0xF0000001u

static void add_elem(vdecl *v, u32 stream, u32 reg, u32 type, u32 offset)
{
    if (v->n < 16)
        v->e[v->n++] = (velem){(u8)stream, (u8)reg, (u8)type, (u8)offset};
}

static void decl_from_fvf(u32 fvf, vdecl *v)
{
    memset(v, 0, sizeof *v);
    u32 off = 0, pos = fvf & 0xE;
    if (pos == 0x4) {  // XYZRHW
        add_elem(v, 0, 0, DT_FLOAT4, off);
        off += 16;
        v->rhw = 1;
    } else if (pos) {
        add_elem(v, 0, 0, DT_FLOAT3, off);
        off += 12;
        u32 betas = pos >= 6 ? (pos - 4) / 2 : 0;
        if (betas) {
            int ub4 = (fvf & 0x1000) != 0;  // D3DFVF_LASTBETA_UBYTE4
            u32 w = ub4 ? betas - 1 : betas;
            if (w)
                add_elem(v, 0, 1, w > 4 ? DT_FLOAT4 : DT_FLOAT1 + (w > 4 ? 3 : w - 1), off);
            off += 4 * w;
            if (ub4) {
                add_elem(v, 0, 2, DT_UBYTE4, off);
                off += 4;
            }
        }
    }
    if (fvf & 0x10) {
        add_elem(v, 0, 3, DT_FLOAT3, off);
        off += 12;
    }
    if (fvf & 0x20) {
        add_elem(v, 0, 4, DT_FLOAT1, off);
        off += 4;
    }
    if (fvf & 0x40) {
        add_elem(v, 0, 5, DT_D3DCOLOR, off);
        off += 4;
    }
    if (fvf & 0x80) {
        add_elem(v, 0, 6, DT_D3DCOLOR, off);
        off += 4;
    }
    u32 ntex = (fvf >> 8) & 0xF;
    for (u32 i = 0; i < ntex && i < 8; i++) {
        static const u8 code_type[4] = {DT_FLOAT2, DT_FLOAT3, DT_FLOAT4, DT_FLOAT1};
        u32 t = code_type[(fvf >> (16 + 2 * i)) & 3];
        add_elem(v, 0, 7 + i, t, off);
        off += g_dt_size[t];
    }
}

// Объявление D3D8 (токены D3DVSD_*): STREAM, STREAMDATA (регистр/пропуск), CONSTMEM, EXT, END
static u32 parse_decl(d3ddev *d, u32 p, vdecl *v)
{
    memset(v, 0, sizeof *v);
    u32 stream = 0, off = 0, n = 0;
    for (;; n++) {
        u32 t = rd32(g_mem, p + 4 * n);
        if (t == 0xFFFFFFFFu || n > 256)
            break;
        switch (t >> 29) {
        case 1:  // STREAM
            if (!(t & 0x10000000u)) {
                stream = t & 0xF;
                off = 0;
            }
            break;
        case 2:  // STREAMDATA
            if (t & 0x10000000u)
                off += ((t >> 16) & 0xF) * 4;
            else {
                u32 type = (t >> 16) & 0xF;
                add_elem(v, stream, t & 0x1F, type & 7, off);
                off += g_dt_size[type & 7];
            }
            break;
        case 4: {  // CONSTMEM: (count) * 4 dword констант с адреса
            u32 cnt = (t >> 25) & 0xF, addr = t & 0x7F;
            for (u32 i = 0; i < cnt * 4; i++)
                if (addr + i / 4 < 96)
                    d->vsc[addr + i / 4][i % 4] = rdf(p + 4 * (n + 1 + i));
            n += cnt * 4;
            break;
        }
        case 5:  // EXT
            n += (t >> 24) & 0x1F;
            break;
        default:
            break;
        }
    }
    return n + 1;
}

int d3d8_vdecl_for(d3ddev *d, vdecl *out)
{
    if (d->vs < VS_HANDLE_BASE) {
        decl_from_fvf(d->vs, out);
        return out->n > 0;
    }
    u32 i = d->vs - VS_HANDLE_BASE;
    if (i >= MAX_VS || !g_vs[i].used)
        return 0;
    *out = g_vs[i].decl;
    return 1;
}

// ================================================================ IDirect3DDevice8
#define DEV(c) ((d3ddev *)com_host(ARG(0)))

static void dev_QueryInterface(CPU *c)
{
    if (com_iid_eq(ARG(1), 0x7385e5df, 0x8fe8, 0x41d5, 0x86b6d7b48547b6cfull)  // IID_IDirect3DDevice8
        || com_iid_eq(ARG(1), 0, 0, 0, 0xc000000000000046ull)) {
        com_addref(ARG(0));
        wr32(g_mem, ARG(2), ARG(0));
        RET(3, S_OK);
    }
    wr32(g_mem, ARG(2), 0);
    RET(3, E_NOINTERFACE);
}

static void dev_TestCooperativeLevel(CPU *c) { RET(1, D3D_OK); }
static void dev_GetAvailableTextureMem(CPU *c) { RET(1, d3d8_vidmem_total() - 16u * 1024 * 1024); }
static void dev_ResourceManagerDiscardBytes(CPU *c) { RET(2, D3D_OK); }

static u32 g_d3d8_obj;
static void dev_GetDirect3D(CPU *c)
{
    com_addref(g_d3d8_obj);
    wr32(g_mem, ARG(1), g_d3d8_obj);
    RET(2, D3D_OK);
}

static void dev_GetDeviceCaps(CPU *c)
{
    d3d8_fill_caps(ARG(1));
    RET(2, D3D_OK);
}

static void dev_GetDisplayMode(CPU *c)
{
    d3ddev *d = DEV(c);
    int w, h;
    user32_screen(&w, &h);
    u32 p = ARG(1);
    wr32(g_mem, p, d->windowed ? (u32)w : d->bb_w);
    wr32(g_mem, p + 4, d->windowed ? (u32)h : d->bb_h);
    wr32(g_mem, p + 8, 60);
    wr32(g_mem, p + 12, FMT_X8R8G8B8);
    RET(2, D3D_OK);
}

static void dev_GetCreationParameters(CPU *c)
{
    u32 p = ARG(1);
    wr32(g_mem, p, 0);
    wr32(g_mem, p + 4, 1);  // D3DDEVTYPE_HAL
    wr32(g_mem, p + 8, DEV(c)->hwnd);
    wr32(g_mem, p + 12, 0x40);  // D3DCREATE_HARDWARE_VERTEXPROCESSING
    RET(2, D3D_OK);
}

static void dev_SetCursorProperties(CPU *c) { RET(4, D3D_OK); }
static void dev_SetCursorPosition(CPU *c) { RET(4, 0); }
static void dev_ShowCursor(CPU *c) { RET(2, 0); }

// задний буфер и автоматическая глубина — renderbuffer-ы текущего размера
// Ссылки устройства: одна — на свой задний буфер/глубину (владение), по одной — на текущие цели.
static void surf_ref(d3dsurf *s, int add)
{
    if (!s)
        return;
    u32 obj = s->tex ? s->tex->obj : s->obj;
    if (add)
        com_addref(obj);
    else
        com_release(obj);
}

static void make_backbuffer(d3ddev *d)
{
    surf_ref(d->rt, 0);
    surf_ref(d->ds, 0);
    surf_ref(d->backbuffer, 0);
    surf_ref(d->autodepth, 0);
    d->rt = d->ds = d->backbuffer = d->autodepth = NULL;
    d->fbo_rt = d->fbo_ds = NULL;
    d->backbuffer = d3d8_surf_create(d->bb_w, d->bb_h, FMT_A8R8G8B8, USAGE_RENDERTARGET, POOL_DEFAULT);
    d->backbuffer->fmt = d->bb_fmt;
    d->backbuffer->backbuffer = 1;
    if (d->ds_fmt)
        d->autodepth = d3d8_surf_create(d->bb_w, d->bb_h, FMT_D24S8, USAGE_DEPTHSTENCIL, POOL_DEFAULT);
    d->rt = d->backbuffer;
    d->ds = d->autodepth;
    surf_ref(d->rt, 1);
    surf_ref(d->ds, 1);
}

// D3DPRESENT_PARAMETERS: BackBufferWidth, Height, Format, Count, MultiSampleType, SwapEffect, hDeviceWindow,
// Windowed, EnableAutoDepthStencil, AutoDepthStencilFormat, Flags, RefreshRate, PresentationInterval
static void read_pp(d3ddev *d, u32 pp)
{
    d->bb_w = rd32(g_mem, pp);
    d->bb_h = rd32(g_mem, pp + 4);
    d->bb_fmt = rd32(g_mem, pp + 8);
    if (rd32(g_mem, pp + 24))
        d->hwnd = rd32(g_mem, pp + 24);
    d->windowed = rd32(g_mem, pp + 28);
    d->ds_fmt = rd32(g_mem, pp + 32) ? rd32(g_mem, pp + 36) : 0;
    if (!d->bb_w || !d->bb_h) {
        int w, h;
        user32_client_size(d->hwnd, &w, &h);
        d->bb_w = (u32)w;
        d->bb_h = (u32)h;
        wr32(g_mem, pp, d->bb_w);
        wr32(g_mem, pp + 4, d->bb_h);
    }
    if (d->bb_fmt == FMT_UNKNOWN) {
        d->bb_fmt = FMT_X8R8G8B8;
        wr32(g_mem, pp + 8, d->bb_fmt);
    }
    fprintf(stderr, "[d3d8] back buffer %ux%u fmt %u %s, depth fmt %u\n", d->bb_w, d->bb_h, d->bb_fmt,
            d->windowed ? "windowed" : "fullscreen", d->ds_fmt);
}

static void dev_CreateAdditionalSwapChain(CPU *c)
{
    wr32(g_mem, ARG(2), 0);
    RET(3, D3DERR_NOTAVAILABLE);
}

// новые параметры показа (Reset и повторный CreateDevice): окно, задний буфер, состояние по умолчанию
static void apply_pp(d3ddev *d, u32 pp)
{
    read_pp(d, pp);
    cs_finish();  // окно меняется, пока поток рендера ничего не рисует
    plat_window_open((int)d->bb_w, (int)d->bb_h, !d->windowed, "Shrek 2");
    make_backbuffer(d);
    set_defaults(d);
    d3d8_bind_target(d);
}

static void dev_Reset(CPU *c)
{
    apply_pp(DEV(c), ARG(1));
    RET(2, D3D_OK);
}

static void dev_Present(CPU *c)
{
    d3d8_present(DEV(c));
    RET(5, D3D_OK);
}

static void dev_GetBackBuffer(CPU *c)  // (BackBuffer, Type, pp)
{
    d3ddev *d = DEV(c);
    com_addref(d->backbuffer->obj);
    wr32(g_mem, ARG(3), d->backbuffer->obj);
    RET(4, D3D_OK);
}

static void dev_GetRasterStatus(CPU *c)
{
    wr32(g_mem, ARG(1), 0);
    wr32(g_mem, ARG(1) + 4, 0);
    RET(2, D3D_OK);
}

static void dev_SetGammaRamp(CPU *c) { RET(3, 0); }  // TODO: гамма — коррекцией при Present

static void dev_GetGammaRamp(CPU *c)
{
    for (u32 i = 0; i < 256; i++)
        for (u32 ch = 0; ch < 3; ch++)
            wr16(g_mem, ARG(1) + ch * 512 + i * 2, (u16)(i * 257));
    RET(2, 0);
}

static u32 tex_result(d3dtex *t, u32 pp)
{
    if (!t) {
        wr32(g_mem, pp, 0);
        return D3DERR_INVALIDCALL;
    }
    wr32(g_mem, pp, t->obj);
    return D3D_OK;
}

// CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture)
static void dev_CreateTexture(CPU *c)
{
    d3dtex *t = d3d8_tex_create(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), 0);
    if (!t)
        fprintf(stderr, "[d3d8] CreateTexture %ux%u fmt %08x failed\n", ARG(1), ARG(2), ARG(5));
    RET(8, tex_result(t, ARG(7)));
}

// CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture)
static void dev_CreateCubeTexture(CPU *c)
{
    d3dtex *t = d3d8_tex_create(ARG(1), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), 1);
    RET(7, tex_result(t, ARG(6)));
}

// CreateVertexBuffer(Length, Usage, FVF, Pool, pp)
static void dev_CreateVertexBuffer(CPU *c)
{
    d3dbuf *b = d3d8_buf_create(0, ARG(1), ARG(2), ARG(3), ARG(4));
    wr32(g_mem, ARG(5), b ? b->obj : 0);
    RET(6, b ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY);
}

// CreateIndexBuffer(Length, Usage, Format, Pool, pp)
static void dev_CreateIndexBuffer(CPU *c)
{
    d3dbuf *b = d3d8_buf_create(1, ARG(1), ARG(2), ARG(3), ARG(4));
    wr32(g_mem, ARG(5), b ? b->obj : 0);
    RET(6, b ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY);
}

// CreateRenderTarget(Width, Height, Format, MultiSample, Lockable, pp)
static void dev_CreateRenderTarget(CPU *c)
{
    d3dsurf *s = d3d8_surf_create(ARG(1), ARG(2), ARG(3), USAGE_RENDERTARGET, POOL_DEFAULT);
    wr32(g_mem, ARG(6), s ? s->obj : 0);
    RET(7, s ? D3D_OK : D3DERR_INVALIDCALL);
}

// CreateDepthStencilSurface(Width, Height, Format, MultiSample, pp)
static void dev_CreateDepthStencilSurface(CPU *c)
{
    d3dsurf *s = d3d8_surf_create(ARG(1), ARG(2), ARG(3), USAGE_DEPTHSTENCIL, POOL_DEFAULT);
    wr32(g_mem, ARG(5), s ? s->obj : 0);
    RET(6, s ? D3D_OK : D3DERR_INVALIDCALL);
}

// CreateImageSurface(Width, Height, Format, pp)
static void dev_CreateImageSurface(CPU *c)
{
    d3dsurf *s = d3d8_surf_create(ARG(1), ARG(2), ARG(3), 0, POOL_SYSTEMMEM);
    wr32(g_mem, ARG(4), s ? s->obj : 0);
    RET(5, s ? D3D_OK : D3DERR_INVALIDCALL);
}

// SetRenderTarget(pRenderTarget, pNewZStencil): устройство держит ссылки на цели
static void dev_SetRenderTarget(CPU *c)
{
    d3ddev *d = DEV(c);
    d3dsurf *rt = d3d8_surf_from(ARG(1)), *ds = d3d8_surf_from(ARG(2));
    if (d3d8_dumping(d) && d->rt && d->rt->tex)
        d3d8_dump_target(d);  // текстура-цель, которую покидаем
    if (rt && rt != d->rt) {
        surf_ref(rt, 1);
        surf_ref(d->rt, 0);
        d->rt = rt;
    }
    if (ds != d->ds) {
        surf_ref(ds, 1);
        surf_ref(d->ds, 0);
        d->ds = ds;
    }
    if (d3d8_dumping(d))
        fprintf(stderr, "[dump] SetRenderTarget %ux%u fmt %x%s ds %ux%u\n", d->rt->w, d->rt->h, d->rt->fmt,
                d->rt->tex ? " (texture)" : "", d->ds ? d->ds->w : 0, d->ds ? d->ds->h : 0);
    // как D3D: viewport — вся новая цель
    d->vp[0] = d->vp[1] = 0;
    d->vp[2] = d->rt->w;
    d->vp[3] = d->rt->h;
    d->vpz[0] = 0.0f;
    d->vpz[1] = 1.0f;
    d3d8_bind_target(d);
    RET(3, D3D_OK);
}

static void dev_GetRenderTarget(CPU *c)
{
    d3ddev *d = DEV(c);
    surf_ref(d->rt, 1);
    wr32(g_mem, ARG(1), d->rt->obj);
    RET(2, D3D_OK);
}

static void dev_GetDepthStencilSurface(CPU *c)
{
    d3ddev *d = DEV(c);
    if (d->ds)
        com_addref(d->ds->obj);
    wr32(g_mem, ARG(1), d->ds ? d->ds->obj : 0);
    RET(2, d->ds ? D3D_OK : 0x88760866u);  // D3DERR_NOTFOUND
}

static void dev_BeginScene(CPU *c) { RET(1, D3D_OK); }
static void dev_EndScene(CPU *c) { RET(1, D3D_OK); }

// Clear(Count, pRects, Flags, Color, Z, Stencil): в пределах viewport (и прямоугольников)
static void dev_Clear(CPU *c)
{
    d3ddev *d = DEV(c);
    u32 count = ARG(1), rects = ARG(2), flags = ARG(3), color = ARG(4), stencil = ARG(6);
    float z = u2f(ARG(5));
    GLbitfield mask = 0;
    if (flags & 1) {
        mask |= GL_COLOR_BUFFER_BIT;
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor((float)((color >> 16) & 0xFF) / 255.0f, (float)((color >> 8) & 0xFF) / 255.0f,
                     (float)(color & 0xFF) / 255.0f, (float)(color >> 24) / 255.0f);
    }
    if ((flags & 2) && d->ds) {
        mask |= GL_DEPTH_BUFFER_BIT;
        glDepthMask(GL_TRUE);
        glClearDepth(z);
    }
    if ((flags & 4) && d->ds) {
        mask |= GL_STENCIL_BUFFER_BIT;
        glStencilMask(0xFF);
        glClearStencil((GLint)stencil);
    }
    if (d3d8_dumping(d))
        fprintf(stderr, "[dump] Clear flags %x color %08x z %g rects %u vp %u,%u %ux%u\n", flags, color, z, count,
                d->vp[0], d->vp[1], d->vp[2], d->vp[3]);
    if (mask) {
        glEnable(GL_SCISSOR_TEST);
        if (!count || !rects) {
            glScissor((GLint)d->vp[0], (GLint)d->vp[1], (GLsizei)d->vp[2], (GLsizei)d->vp[3]);
            glClear(mask);
        }
        for (u32 i = 0; rects && i < count; i++) {
            s32 x1 = (s32)rd32(g_mem, rects + 16 * i), y1 = (s32)rd32(g_mem, rects + 16 * i + 4);
            s32 x2 = (s32)rd32(g_mem, rects + 16 * i + 8), y2 = (s32)rd32(g_mem, rects + 16 * i + 12);
            if (x2 > x1 && y2 > y1) {
                glScissor(x1, y1, x2 - x1, y2 - y1);
                glClear(mask);
            }
        }
        d->gen_misc++;  // маски записи и scissor переустанавливаются перед следующей отрисовкой
    }
    RET(7, D3D_OK);
}

static float *xf_slot(d3ddev *d, u32 state)
{
    if (state == 2)
        return d->view;
    if (state == 3)
        return d->proj;
    if (state >= 16 && state < 16 + MAX_STAGES)
        return d->texm[state - 16];
    if (state == 256)
        return d->world;
    return NULL;
}

static void dev_SetTransform(CPU *c)
{
    d3ddev *d = DEV(c);
    float *m = xf_slot(d, ARG(1));
    if (m) {
        memcpy(m, g_mem + ARG(2), 64);
        d->gen_xf++;
    }
    RET(3, D3D_OK);
}

static void dev_GetTransform(CPU *c)
{
    d3ddev *d = DEV(c);
    float *m = xf_slot(d, ARG(1)), id[16];
    if (!m) {
        identity(id);
        m = id;
    }
    memcpy(g_mem + ARG(2), m, 64);
    RET(3, D3D_OK);
}

static void dev_MultiplyTransform(CPU *c)
{
    d3ddev *d = DEV(c);
    float *m = xf_slot(d, ARG(1)), b[16], r[16];
    if (m) {
        memcpy(b, g_mem + ARG(2), 64);
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r[i * 4 + j] = m[i * 4] * b[j] + m[i * 4 + 1] * b[4 + j] + m[i * 4 + 2] * b[8 + j] + m[i * 4 + 3] * b[12 + j];
        memcpy(m, r, 64);
        d->gen_xf++;
    }
    RET(3, D3D_OK);
}

// D3DVIEWPORT8: X, Y, Width, Height, MinZ, MaxZ
static void dev_SetViewport(CPU *c)
{
    d3ddev *d = DEV(c);
    for (int i = 0; i < 4; i++)
        d->vp[i] = rd32(g_mem, ARG(1) + 4u * (u32)i);
    d->vpz[0] = rdf(ARG(1) + 16);
    d->vpz[1] = rdf(ARG(1) + 20);
    if (d3d8_dumping(d))
        fprintf(stderr, "[dump] SetViewport %u,%u %ux%u\n", d->vp[0], d->vp[1], d->vp[2], d->vp[3]);
    d->gen_misc++;
    RET(2, D3D_OK);
}

static void dev_GetViewport(CPU *c)
{
    d3ddev *d = DEV(c);
    for (int i = 0; i < 4; i++)
        wr32(g_mem, ARG(1) + 4u * (u32)i, d->vp[i]);
    wr32(g_mem, ARG(1) + 16, f2u(d->vpz[0]));
    wr32(g_mem, ARG(1) + 20, f2u(d->vpz[1]));
    RET(2, D3D_OK);
}

static void dev_SetMaterial(CPU *c)
{
    d3ddev *d = DEV(c);
    memcpy(d->material, g_mem + ARG(1), sizeof d->material);
    d->gen_mat++;
    RET(2, D3D_OK);
}

static void dev_GetMaterial(CPU *c)
{
    memcpy(g_mem + ARG(1), DEV(c)->material, sizeof DEV(c)->material);
    RET(2, D3D_OK);
}

static void dev_SetLight(CPU *c)
{
    d3ddev *d = DEV(c);
    u32 i = ARG(1);
    if (i < MAX_LIGHTS) {
        memcpy(&d->lights[i], g_mem + ARG(2), sizeof(d3dlight));
        d->light_set[i] = 1;
        d->gen_light++;
    } else
        hle_todo("d3d8: light index >= 8");
    RET(3, D3D_OK);
}

static void dev_GetLight(CPU *c)
{
    d3ddev *d = DEV(c);
    if (ARG(1) >= MAX_LIGHTS || !d->light_set[ARG(1)])
        RET(3, D3DERR_INVALIDCALL);
    memcpy(g_mem + ARG(2), &d->lights[ARG(1)], sizeof(d3dlight));
    RET(3, D3D_OK);
}

static void dev_LightEnable(CPU *c)
{
    d3ddev *d = DEV(c);
    u32 i = ARG(1);
    if (i < MAX_LIGHTS) {
        if (!d->light_set[i]) {  // как D3D: включение незаданного источника — белый направленный (0,0,1)
            memset(&d->lights[i], 0, sizeof(d3dlight));
            d->lights[i].type = 3;
            d->lights[i].diffuse[0] = d->lights[i].diffuse[1] = d->lights[i].diffuse[2] = 1.0f;
            d->lights[i].dir[2] = 1.0f;
            d->light_set[i] = 1;
        }
        d->light_on[i] = ARG(2) != 0;
        d->gen_light++;
    }
    RET(3, D3D_OK);
}

static void dev_GetLightEnable(CPU *c)
{
    d3ddev *d = DEV(c);
    wr32(g_mem, ARG(2), ARG(1) < MAX_LIGHTS ? d->light_on[ARG(1)] : 0);
    RET(3, D3D_OK);
}

static void dev_SetClipPlane(CPU *c)
{
    d3ddev *d = DEV(c);
    if (ARG(1) < 6) {
        memcpy(d->clip[ARG(1)], g_mem + ARG(2), 16);
        d->gen_misc++;
    }
    RET(3, D3D_OK);
}

static void dev_GetClipPlane(CPU *c)
{
    d3ddev *d = DEV(c);
    if (ARG(1) < 6)
        memcpy(g_mem + ARG(2), d->clip[ARG(1)], 16);
    RET(3, D3D_OK);
}

static void dev_SetRenderState(CPU *c)
{
    d3ddev *d = DEV(c);
    if (ARG(1) < 256 && d->rs[ARG(1)] != ARG(2)) {
        d->rs[ARG(1)] = ARG(2);
        d->gen_misc++;
    }
    RET(3, D3D_OK);
}

static void dev_GetRenderState(CPU *c)
{
    wr32(g_mem, ARG(2), ARG(1) < 256 ? DEV(c)->rs[ARG(1)] : 0);
    RET(3, D3D_OK);
}

static void dev_SetClipStatus(CPU *c) { RET(2, D3D_OK); }

static void dev_GetClipStatus(CPU *c)
{
    memset(g_mem + ARG(1), 0, 8);
    RET(2, D3D_OK);
}

static void dev_GetTexture(CPU *c)
{
    d3ddev *d = DEV(c);
    d3dtex *t = ARG(1) < MAX_STAGES ? d->tex[ARG(1)] : NULL;
    if (t)
        com_addref(t->obj);
    wr32(g_mem, ARG(2), t ? t->obj : 0);
    RET(3, D3D_OK);
}

static void dev_SetTexture(CPU *c)
{
    d3ddev *d = DEV(c);
    u32 s = ARG(1);
    if (s >= MAX_STAGES)
        RET(3, D3D_OK);
    d3dtex *t = d3d8_tex_from(ARG(2));
    if (t != d->tex[s]) {
        if (t)
            com_addref(t->obj);
        if (d->tex[s])
            com_release(d->tex[s]->obj);
        d->tex[s] = t;
        d->gen_misc++;
    }
    RET(3, D3D_OK);
}

static void dev_GetTextureStageState(CPU *c)
{
    d3ddev *d = DEV(c);
    wr32(g_mem, ARG(3), ARG(1) < MAX_STAGES && ARG(2) < 32 ? d->tss[ARG(1)][ARG(2)] : 0);
    RET(4, D3D_OK);
}

static void dev_SetTextureStageState(CPU *c)
{
    d3ddev *d = DEV(c);
    u32 s = ARG(1), t = ARG(2);
    if (s < MAX_STAGES && t < 32 && d->tss[s][t] != ARG(3)) {
        d->tss[s][t] = ARG(3);
        d->gen_misc++;
        if ((t >= 13 && t <= 21) || t == 25)
            d->sampler_dirty |= 1u << s;
    }
    RET(4, D3D_OK);
}

static void dev_ValidateDevice(CPU *c)
{
    wr32(g_mem, ARG(1), 1);
    RET(2, D3D_OK);
}

static void dev_GetInfo(CPU *c) { RET(4, 1u); }  // S_FALSE: сведений нет
static void dev_SetPaletteEntries(CPU *c) { RET(3, D3D_OK); }
static void dev_SetCurrentTexturePalette(CPU *c) { RET(2, D3D_OK); }

// ---------------------------------------------------------------- отрисовка
static void dev_DrawPrimitive(CPU *c)  // (Type, StartVertex, PrimitiveCount)
{
    d3d8_draw(DEV(c), ARG(1), ARG(3), 0, ARG(2), 0, 0, NULL, 0, NULL, 0);
    RET(4, D3D_OK);
}

static void dev_DrawIndexedPrimitive(CPU *c)  // (Type, MinIndex, NumVertices, StartIndex, PrimCount)
{
    d3d8_draw(DEV(c), ARG(1), ARG(5), 1, ARG(4), ARG(2), ARG(3), NULL, 0, NULL, 0);
    RET(6, D3D_OK);
}

static void dev_DrawPrimitiveUP(CPU *c)  // (Type, PrimitiveCount, pVertexStreamZeroData, Stride)
{
    d3ddev *d = DEV(c);
    d3d8_draw(d, ARG(1), ARG(2), 0, 0, 0, 0, g_mem + ARG(3), ARG(4), NULL, 0);
    // как D3D: после *UP поток 0 и индексы сброшены
    if (d->stream[0]) {
        com_release(d->stream[0]->obj);
        d->stream[0] = NULL;
    }
    d->stride[0] = 0;
    RET(5, D3D_OK);
}

// (Type, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexData, Stride)
static void dev_DrawIndexedPrimitiveUP(CPU *c)
{
    d3ddev *d = DEV(c);
    d3d8_draw(d, ARG(1), ARG(4), 1, 0, ARG(2), ARG(3), g_mem + ARG(7), ARG(8), g_mem + ARG(5), ARG(6));
    if (d->stream[0]) {
        com_release(d->stream[0]->obj);
        d->stream[0] = NULL;
    }
    if (d->indices) {
        com_release(d->indices->obj);
        d->indices = NULL;
    }
    d->stride[0] = 0;
    RET(9, D3D_OK);
}

// ---------------------------------------------------------------- шейдеры
// CreateVertexShader(pDeclaration, pFunction, pHandle, Usage)
static void dev_CreateVertexShader(CPU *c)
{
    d3ddev *d = DEV(c);
    if (ARG(2)) {
        hle_todo("d3d8: programmable vertex shaders (vs.1.1) are not translated yet");
        RET(5, D3DERR_INVALIDCALL);
    }
    for (u32 i = 0; i < MAX_VS; i++)
        if (!g_vs[i].used) {
            g_vs[i].used = 1;
            g_vs[i].ntok = parse_decl(d, ARG(1), &g_vs[i].decl);
            g_vs[i].tokens = malloc(4 * g_vs[i].ntok);
            memcpy(g_vs[i].tokens, g_mem + ARG(1), 4 * g_vs[i].ntok);
            wr32(g_mem, ARG(3), VS_HANDLE_BASE + i);
            RET(5, D3D_OK);
        }
    RET(5, D3DERR_OUTOFVIDEOMEMORY);
}

static void dev_SetVertexShader(CPU *c)
{
    d3ddev *d = DEV(c);
    if (d->vs != ARG(1)) {
        d->vs = ARG(1);
        d->gen_misc++;
    }
    RET(2, D3D_OK);
}

static void dev_GetVertexShader(CPU *c)
{
    wr32(g_mem, ARG(1), DEV(c)->vs);
    RET(2, D3D_OK);
}

static void dev_DeleteVertexShader(CPU *c)
{
    u32 i = ARG(1) - VS_HANDLE_BASE;
    if (ARG(1) >= VS_HANDLE_BASE && i < MAX_VS && g_vs[i].used) {
        free(g_vs[i].tokens);
        memset(&g_vs[i], 0, sizeof g_vs[i]);
    }
    RET(2, D3D_OK);
}

static void dev_SetVertexShaderConstant(CPU *c)  // (Register, pConstantData, ConstantCount)
{
    d3ddev *d = DEV(c);
    for (u32 i = 0; i < ARG(3) && ARG(1) + i < 96; i++)
        memcpy(d->vsc[ARG(1) + i], g_mem + ARG(2) + 16 * i, 16);
    RET(4, D3D_OK);
}

static void dev_GetVertexShaderConstant(CPU *c)
{
    d3ddev *d = DEV(c);
    for (u32 i = 0; i < ARG(3) && ARG(1) + i < 96; i++)
        memcpy(g_mem + ARG(2) + 16 * i, d->vsc[ARG(1) + i], 16);
    RET(4, D3D_OK);
}

static void dev_SetStreamSource(CPU *c)  // (StreamNumber, pStreamData, Stride)
{
    d3ddev *d = DEV(c);
    u32 n = ARG(1);
    if (n >= MAX_STREAMS)
        RET(4, D3DERR_INVALIDCALL);
    d3dbuf *b = d3d8_buf_from(ARG(2));
    if (b != d->stream[n]) {
        if (b)
            com_addref(b->obj);
        if (d->stream[n])
            com_release(d->stream[n]->obj);
        d->stream[n] = b;
    }
    d->stride[n] = ARG(3);
    RET(4, D3D_OK);
}

static void dev_GetStreamSource(CPU *c)
{
    d3ddev *d = DEV(c);
    u32 n = ARG(1);
    d3dbuf *b = n < MAX_STREAMS ? d->stream[n] : NULL;
    if (b)
        com_addref(b->obj);
    wr32(g_mem, ARG(2), b ? b->obj : 0);
    wr32(g_mem, ARG(3), n < MAX_STREAMS ? d->stride[n] : 0);
    RET(4, D3D_OK);
}

static void dev_SetIndices(CPU *c)  // (pIndexData, BaseVertexIndex)
{
    d3ddev *d = DEV(c);
    d3dbuf *b = d3d8_buf_from(ARG(1));
    if (b != d->indices) {
        if (b)
            com_addref(b->obj);
        if (d->indices)
            com_release(d->indices->obj);
        d->indices = b;
    }
    d->base_vertex = ARG(2);
    RET(3, D3D_OK);
}

static void dev_GetIndices(CPU *c)
{
    d3ddev *d = DEV(c);
    if (d->indices)
        com_addref(d->indices->obj);
    wr32(g_mem, ARG(1), d->indices ? d->indices->obj : 0);
    wr32(g_mem, ARG(2), d->base_vertex);
    RET(3, D3D_OK);
}

static void dev_CreatePixelShader(CPU *c)  // (pFunction, pHandle)
{
    u32 h = d3d8_ps_create((const u32 *)(g_mem + ARG(1)), (RT_WINDOW - ARG(1)) / 4);
    wr32(g_mem, ARG(2), h);
    RET(3, h ? D3D_OK : D3DERR_INVALIDCALL);
}

static void dev_SetPixelShader(CPU *c)
{
    d3ddev *d = DEV(c);
    if (d->ps != ARG(1)) {
        d->ps = ARG(1);
        d->gen_misc++;
    }
    RET(2, D3D_OK);
}

static void dev_GetPixelShader(CPU *c)
{
    wr32(g_mem, ARG(1), DEV(c)->ps);
    RET(2, D3D_OK);
}

static void dev_DeletePixelShader(CPU *c)
{
    d3d8_ps_delete(ARG(1));
    RET(2, D3D_OK);
}

static void dev_SetPixelShaderConstant(CPU *c)  // (Register, pConstantData, ConstantCount)
{
    d3ddev *d = DEV(c);
    for (u32 i = 0; i < ARG(3) && ARG(1) + i < 8; i++)
        memcpy(d->psc[ARG(1) + i], g_mem + ARG(2) + 16 * i, 16);
    d->gen_misc++;
    RET(4, D3D_OK);
}

static void dev_GetPixelShaderConstant(CPU *c)
{
    d3ddev *d = DEV(c);
    for (u32 i = 0; i < ARG(3) && ARG(1) + i < 8; i++)
        memcpy(g_mem + ARG(2) + 16 * i, d->psc[ARG(1) + i], 16);
    RET(4, D3D_OK);
}

// ================================================================ создание
// Игра освобождает устройство при выходе и при смене разрешения (SetRes пересоздаёт устройство). Окно, контекст
// GL и его объекты остаются: следующий CreateDevice берёт их снова с новым COM-объектом. Привязанные текстуры и
// буферы устройство отпускает, как D3D.
static void dev_destroy(void *p)
{
    d3ddev *d = p;
    d->obj = 0;
    for (int s = 0; s < MAX_STAGES; s++)
        if (d->tex[s]) {
            com_release(d->tex[s]->obj);
            d->tex[s] = NULL;
        }
    for (int n = 0; n < MAX_STREAMS; n++)
        if (d->stream[n]) {
            com_release(d->stream[n]->obj);
            d->stream[n] = NULL;
        }
    if (d->indices) {
        com_release(d->indices->obj);
        d->indices = NULL;
    }
    d->gen_misc++;
}

static void init_vtable(void)
{
    if (g_vt_dev)
        return;
    guest_fn m[97] = {0};
    m[0] = dev_QueryInterface;
    m[1] = com_AddRef;
    m[2] = com_Release;
    m[3] = dev_TestCooperativeLevel;
    m[4] = dev_GetAvailableTextureMem;
    m[5] = dev_ResourceManagerDiscardBytes;
    m[6] = dev_GetDirect3D;
    m[7] = dev_GetDeviceCaps;
    m[8] = dev_GetDisplayMode;
    m[9] = dev_GetCreationParameters;
    m[10] = dev_SetCursorProperties;
    m[11] = dev_SetCursorPosition;
    m[12] = dev_ShowCursor;
    m[13] = dev_CreateAdditionalSwapChain;
    m[14] = dev_Reset;
    m[15] = dev_Present;
    m[16] = dev_GetBackBuffer;
    m[17] = dev_GetRasterStatus;
    m[18] = dev_SetGammaRamp;
    m[19] = dev_GetGammaRamp;
    m[20] = dev_CreateTexture;
    // 21 CreateVolumeTexture
    m[22] = dev_CreateCubeTexture;
    m[23] = dev_CreateVertexBuffer;
    m[24] = dev_CreateIndexBuffer;
    m[25] = dev_CreateRenderTarget;
    m[26] = dev_CreateDepthStencilSurface;
    m[27] = dev_CreateImageSurface;
    // 28 CopyRects, 29 UpdateTexture, 30 GetFrontBuffer
    m[31] = dev_SetRenderTarget;
    m[32] = dev_GetRenderTarget;
    m[33] = dev_GetDepthStencilSurface;
    m[34] = dev_BeginScene;
    m[35] = dev_EndScene;
    m[36] = dev_Clear;
    m[37] = dev_SetTransform;
    m[38] = dev_GetTransform;
    m[39] = dev_MultiplyTransform;
    m[40] = dev_SetViewport;
    m[41] = dev_GetViewport;
    m[42] = dev_SetMaterial;
    m[43] = dev_GetMaterial;
    m[44] = dev_SetLight;
    m[45] = dev_GetLight;
    m[46] = dev_LightEnable;
    m[47] = dev_GetLightEnable;
    m[48] = dev_SetClipPlane;
    m[49] = dev_GetClipPlane;
    m[50] = dev_SetRenderState;
    m[51] = dev_GetRenderState;
    // 52..57 state blocks
    m[58] = dev_SetClipStatus;
    m[59] = dev_GetClipStatus;
    m[60] = dev_GetTexture;
    m[61] = dev_SetTexture;
    m[62] = dev_GetTextureStageState;
    m[63] = dev_SetTextureStageState;
    m[64] = dev_ValidateDevice;
    m[65] = dev_GetInfo;
    m[66] = dev_SetPaletteEntries;
    // 67 GetPaletteEntries
    m[68] = dev_SetCurrentTexturePalette;
    // 69 GetCurrentTexturePalette
    m[70] = dev_DrawPrimitive;
    m[71] = dev_DrawIndexedPrimitive;
    m[72] = dev_DrawPrimitiveUP;
    m[73] = dev_DrawIndexedPrimitiveUP;
    // 74 ProcessVertices
    m[75] = dev_CreateVertexShader;
    m[76] = dev_SetVertexShader;
    m[77] = dev_GetVertexShader;
    m[78] = dev_DeleteVertexShader;
    m[79] = dev_SetVertexShaderConstant;
    m[80] = dev_GetVertexShaderConstant;
    // 81 GetVertexShaderDeclaration, 82 GetVertexShaderFunction
    m[83] = dev_SetStreamSource;
    m[84] = dev_GetStreamSource;
    m[85] = dev_SetIndices;
    m[86] = dev_GetIndices;
    m[87] = dev_CreatePixelShader;
    m[88] = dev_SetPixelShader;
    m[89] = dev_GetPixelShader;
    m[90] = dev_DeletePixelShader;
    m[91] = dev_SetPixelShaderConstant;
    m[92] = dev_GetPixelShaderConstant;
    // 93 GetPixelShaderFunction, 94..96 патчи
    g_vt_dev = com_vtable("IDirect3DDevice8", m, 97);
}

// Switch: гость — ядро 1, его фоновые потоки — 0 (kernel32), поток рендера — 2
#define RENDER_CORE 2

static void bind_gl(void *r) { *(int *)r = plat_gl_bind(); }

// окно с контекстом GL и поток рендера (создание устройства; заставка — раньше него)
static int open_window(int w, int h, int fullscreen)
{
    if (plat_window_open(w, h, fullscreen, "Shrek 2") != 0)
        return -1;
    // поток рендера (по умолчанию; SNX_RENDER_THREAD=0 — без него): контекст GL — у него, вызовы GL слоя D3D8
    // идут через очередь. С экспериментом SNX_GLTHREAD (glthread Mesa) не совмещается.
    const char *rtv = getenv("SNX_RENDER_THREAD");
    int rt_on = !getenv("SNX_GLTHREAD") && !(rtv && *rtv == '0');
    if (rt_on && cs_start(RENDER_CORE) != 0)
        fprintf(stderr, "[d3d8] cannot start the render thread\n");
    int bound = -1;
    cs_sync(bind_gl, &bound);
    return bound;
}

// ---------------------------------------------------------------- заставка загрузки
typedef struct {
    const u8 *px;  // BGRX, строки снизу вверх
    int w, h;
    u32 stride;
} splash_img;

static void draw_splash(void *arg)
{
    const splash_img *s = arg;
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(s->stride / 4));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, s->w, s->h, 0, GL_BGRA, GL_UNSIGNED_BYTE, s->px);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    int W, H;
    plat_drawable_size(&W, &H);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // вписать с сохранением пропорций
    int w = W, h = (int)((long long)W * s->h / s->w);
    if (h > H) {
        h = H;
        w = (int)((long long)H * s->w / s->h);
    }
    glBlitFramebuffer(0, 0, s->w, s->h, (W - w) / 2, (H - h) / 2, (W - w) / 2 + w, (H - h) / 2 + h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    plat_swap();
}

// game.exe показывает Help\Logo.bmp в диалоге, пока движок грузит пакеты и создаёт устройство (на Switch —
// секунды): окно и поток рендера открываются заранее, картинка — по центру экрана
void d3d8_splash(const u8 *bgrx, int w, int h, u32 stride)
{
    if (g_dev || w <= 0 || h <= 0)
        return;
    int sw = w, sh = h;  // мак: окно размером с картинку, как заставка на ПК
#ifdef __SWITCH__
    plat_screen_size(&sw, &sh);
#endif
    if (open_window(sw, sh, 1) != 0)
        return;
    splash_img s = {bgrx, w, h, stride};
    cs_sync(draw_splash, &s);
}

u32 d3d8_device_create(CPU *c, u32 hwnd, u32 pp)
{
    (void)c;
    if (g_dev) {
        d3ddev *d = g_dev;
        if (d->obj)
            com_addref(d->obj);  // старое ещё не освобождено — то же устройство
        else {
            d->obj = com_create(g_vt_dev, d, dev_destroy);
            d3d8_res_init(d->obj);
        }
        d->hwnd = hwnd;
        apply_pp(d, pp);
        return d->obj;
    }
    init_vtable();
    d3ddev *d = calloc(1, sizeof *d);
    d->hwnd = hwnd;
    read_pp(d, pp);
    if (open_window((int)d->bb_w, (int)d->bb_h, !d->windowed) != 0) {
        free(d);
        return 0;
    }
    d->obj = com_create(g_vt_dev, d, dev_destroy);
    g_dev = d;
    d3d8_res_init(d->obj);
    glGenVertexArrays(1, &d->vao);
    glBindVertexArray(d->vao);
    glGenBuffers(1, &d->up_vbo);
    glGenBuffers(1, &d->up_ibo);
    glGenSamplers(MAX_STAGES, d->samplers);
    d3d8_caps_probe(d);
    d3d8_prog_preload();
    make_backbuffer(d);
    set_defaults(d);
    d3d8_bind_target(d);
    return d->obj;
}

void d3d8_set_d3d_object(u32 obj) { g_d3d8_obj = obj; }
