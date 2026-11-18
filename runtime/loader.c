// Загрузка PE-модулей игры в гостевую память по фиксированной карте адресов.
#include "runtime/loader.h"

#include <stdio.h>
#include <stdlib.h>

#include "runtime/rt.h"

static u32 crc32_buf(const u8 *p, size_t n)
{
    static u32 table[256];
    if (!table[1]) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static u32 g_next_thunk = RT_THUNK_BASE;

u32 rt_hle_thunk(const char *dll, guest_fn fn)
{
    (void)dll;
    u32 a = g_next_thunk;
    g_next_thunk += RT_THUNK_SIZE;
    memset(g_mem + a, 0xCC, RT_THUNK_SIZE);
    rt_register_function(a, fn);
    return a;
}

static u8 *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static u32 le32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u16 le16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }

int rt_load_module(const rt_module_desc *d, const char *dir)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, d->name);
    size_t size = 0;
    u8 *file = read_file(path, &size);
    if (!file) {
        fprintf(stderr, "[loader] cannot read %s\n", path);
        return -1;
    }
    if (size != d->file_size || crc32_buf(file, size) != d->crc32) {
        fprintf(stderr, "[loader] %s: file differs from the one used for recompilation (size %zu/%u, crc %08x/%08x)\n",
                d->name, size, d->file_size, crc32_buf(file, size), d->crc32);
        free(file);
        return -1;
    }
    u32 pe = le32(file + 0x3C);
    u16 nsec = le16(file + pe + 6);
    u16 optsz = le16(file + pe + 20);
    const u8 *opt = file + pe + 24;
    u32 pref = le32(opt + 28);
    u32 hdrsize = le32(opt + 60);
    u32 reloc_rva = le32(opt + 96 + 5 * 8);
    u32 reloc_size = le32(opt + 96 + 5 * 8 + 4);
    u8 *img = g_mem + d->base;
    memset(img, 0, d->size);
    memcpy(img, file, hdrsize);
    const u8 *sec = opt + optsz;
    for (u16 i = 0; i < nsec; i++, sec += 40) {
        u32 vsize = le32(sec + 8), va = le32(sec + 12), rawsize = le32(sec + 16), rawptr = le32(sec + 20);
        u32 n = rawsize < vsize || !vsize ? rawsize : vsize;
        if (va + n > d->size)
            n = d->size - va;
        if (rawptr + n > size)
            n = (u32)size - rawptr;
        memcpy(img + va, file + rawptr, n);
    }
    free(file);
    u32 delta = d->base - pref;
    if (delta && reloc_size) {
        u32 off = 0;
        while (off + 8 <= reloc_size) {
            u32 page = le32(img + reloc_rva + off), bsize = le32(img + reloc_rva + off + 4);
            if (bsize < 8)
                break;
            for (u32 k = 8; k < bsize; k += 2) {
                u16 e = le16(img + reloc_rva + off + k);
                if ((e >> 12) == 3) {
                    u8 *p = img + page + (e & 0xFFF);
                    memcpy(p, &(u32){le32(p) + delta}, 4);
                } else if ((e >> 12) != 0) {
                    fprintf(stderr, "[loader] %s: unsupported reloc type %u\n", d->name, e >> 12);
                    return -1;
                }
            }
            off += bsize;
        }
    } else if (delta) {
        fprintf(stderr, "[loader] %s: needs rebase but has no relocations\n", d->name);
        return -1;
    }
    for (u32 i = 0; i < d->niat; i++) {
        const rt_iat_entry *e = &d->iat[i];
        if (!e->iat_va)
            continue;
        u32 v = e->guest_target ? e->guest_target : rt_hle_thunk(e->dll, e->hle);
        wr32(g_mem, e->iat_va, v);
    }
    rt_register_functions(d->funcs, d->nfuncs);
    return 0;
}
