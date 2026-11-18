// Кучи гостя. Блок: 8 байт заголовка (запрошенный размер, метка с классом) + полезная часть,
// выровнено на 8 (как HeapAlloc в Win32). Мелкие и средние блоки — из кусков по 1 МБ со списками свободных
// по классам размеров (без слияния), крупнее BIG — отдельным vm_alloc. Указатели списков — гостевые.
#include "runtime/heap.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "runtime/vmem.h"

#define CHUNK 0x100000u
#define BIG 0x40000u  // блоки больше — отдельным выделением
#define HDR 8u
#define TAG 0x48500000u
#define TAG_MASK 0xFFF00000u
#define F_USED 0x80000u
#define F_BIG 0x40000u
#define NCLASS 160
#define MAX_HEAPS 64

typedef struct {
    u32 handle;
    u32 free_head[NCLASS];
    u32 bump, bump_end;
    u32 *chunks;  // куски и крупные блоки (для HeapDestroy)
    u32 nchunks, cap_chunks;
} heap_t;

static heap_t g_heaps[MAX_HEAPS];
static u32 g_class_cap[NCLASS];
static u32 g_nclass;
static u32 g_process_heap;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void build_classes(void)
{
    u32 n = 0;
    for (u32 s = 8; s <= 1024; s += 8)
        g_class_cap[n++] = s;
    for (u32 p = 1024; p < BIG; p *= 2)
        for (u32 k = 1; k <= 4; k++)
            g_class_cap[n++] = p + p / 4 * k;
    g_nclass = n;
}

static u32 class_of(u32 need)
{
    if (need <= 1024)
        return (need + 7) / 8 - 1;
    u32 lo = 128, hi = g_nclass - 1;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_class_cap[mid] >= need)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

static heap_t *find(u32 handle)
{
    for (int i = 0; i < MAX_HEAPS; i++)
        if (g_heaps[i].handle && g_heaps[i].handle == handle)
            return &g_heaps[i];
    return NULL;
}

static void track(heap_t *h, u32 a)
{
    if (h->nchunks == h->cap_chunks) {
        h->cap_chunks = h->cap_chunks ? h->cap_chunks * 2 : 16;
        h->chunks = realloc(h->chunks, h->cap_chunks * sizeof *h->chunks);
    }
    h->chunks[h->nchunks++] = a;
}

static void untrack(heap_t *h, u32 a)
{
    for (u32 i = 0; i < h->nchunks; i++)
        if (h->chunks[i] == a) {
            h->chunks[i] = h->chunks[--h->nchunks];
            return;
        }
}

// Остаток куска — в списки свободных (крупнейшими подходящими блоками).
static void spill_rest(heap_t *h)
{
    while (h->bump_end - h->bump >= HDR + 8) {
        u32 room = h->bump_end - h->bump - HDR, c = class_of(room);
        if (g_class_cap[c] > room)
            c--;
        u32 b = h->bump;
        wr32(g_mem, b, 0);
        wr32(g_mem, b + 4, TAG | c);
        wr32(g_mem, b + HDR, h->free_head[c]);
        h->free_head[c] = b + HDR;
        h->bump += HDR + g_class_cap[c];
    }
}

static u32 alloc_locked(heap_t *h, u32 flags, u32 size)
{
    u32 need = size ? (size + 7) & ~7u : 8;
    if (need > BIG) {
        u32 a = vm_alloc(0, need + HDR, VM_MEM_RESERVE | VM_MEM_COMMIT, 0x04);
        if (!a)
            return 0;
        track(h, a);
        wr32(g_mem, a, size);
        wr32(g_mem, a + 4, TAG | F_USED | F_BIG);
        return a + HDR;  // свежая память уже нулевая
    }
    u32 c = class_of(need), cap = g_class_cap[c], p = h->free_head[c];
    if (p) {
        h->free_head[c] = rd32(g_mem, p);
    } else {
        if (h->bump_end - h->bump < HDR + cap) {
            spill_rest(h);
            u32 a = vm_alloc(0, CHUNK, VM_MEM_RESERVE | VM_MEM_COMMIT, 0x04);
            if (!a)
                return 0;
            track(h, a);
            h->bump = a;
            h->bump_end = a + CHUNK;
        }
        p = h->bump + HDR;
        h->bump += HDR + cap;
    }
    wr32(g_mem, p - HDR, size);
    wr32(g_mem, p - 4, TAG | F_USED | c);
    if (flags & HEAP_ZERO_MEMORY)
        memset(g_mem + p, 0, cap);
    return p;
}

static int valid_block(u32 p)
{
    if (p < 0x10000u + HDR || (p & 7))
        return 0;
    u32 t = rd32(g_mem, p - 4);
    return (t & TAG_MASK) == TAG && (t & F_USED);
}

static u32 capacity(u32 p)
{
    u32 t = rd32(g_mem, p - 4);
    if (t & F_BIG) {
        vm_info vi;
        vm_query(p - HDR, &vi);
        return vi.alloc_base + vi.size - p;
    }
    return g_class_cap[t & 0xFFFFu];
}

static int free_locked(heap_t *h, u32 p)
{
    if (!valid_block(p))
        return 0;
    u32 t = rd32(g_mem, p - 4);
    if (t & F_BIG) {
        untrack(h, p - HDR);
        return vm_free(p - HDR, 0, VM_MEM_RELEASE);
    }
    u32 c = t & 0xFFFFu;
    wr32(g_mem, p - 4, TAG | c);
    wr32(g_mem, p, h->free_head[c]);
    h->free_head[c] = p;
    return 1;
}

void heap_init(void)
{
    build_classes();
    g_process_heap = heap_create(0);
}

u32 heap_process(void) { return g_process_heap; }

u32 heap_create(u32 flags)
{
    (void)flags;
    pthread_mutex_lock(&g_lock);
    u32 ret = 0;
    for (int i = 0; i < MAX_HEAPS; i++)
        if (!g_heaps[i].handle) {
            // описатель — гостевой адрес служебной страницы (код CRT сравнивает описатели, но не читает их)
            u32 a = vm_alloc(0, CHUNK, VM_MEM_RESERVE | VM_MEM_COMMIT, 0x04);
            if (!a)
                break;
            heap_t *h = &g_heaps[i];
            memset(h, 0, sizeof *h);
            h->handle = a;
            track(h, a);
            h->bump = a + 0x1000u;
            h->bump_end = a + CHUNK;
            ret = a;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    return ret;
}

int heap_destroy(u32 handle)
{
    pthread_mutex_lock(&g_lock);
    heap_t *h = find(handle);
    if (h && handle != g_process_heap) {
        for (u32 i = 0; i < h->nchunks; i++)
            vm_free(h->chunks[i], 0, VM_MEM_RELEASE);
        free(h->chunks);
        memset(h, 0, sizeof *h);
    }
    pthread_mutex_unlock(&g_lock);
    return h != NULL;
}

u32 heap_alloc(u32 handle, u32 flags, u32 size)
{
    pthread_mutex_lock(&g_lock);
    heap_t *h = find(handle);
    u32 p = h ? alloc_locked(h, flags, size) : 0;
    pthread_mutex_unlock(&g_lock);
    return p;
}

u32 heap_realloc(u32 handle, u32 flags, u32 p, u32 size)
{
    pthread_mutex_lock(&g_lock);
    heap_t *h = find(handle);
    u32 ret = 0;
    if (h && valid_block(p)) {
        u32 old = rd32(g_mem, p - HDR), cap = capacity(p);
        if (size <= cap) {
            if ((flags & HEAP_ZERO_MEMORY) && size > old)
                memset(g_mem + p + old, 0, size - old);
            wr32(g_mem, p - HDR, size);
            ret = p;
        } else if (!(flags & HEAP_REALLOC_IN_PLACE_ONLY)) {
            ret = alloc_locked(h, flags & HEAP_ZERO_MEMORY, size);
            if (ret) {
                memcpy(g_mem + ret, g_mem + p, old);
                free_locked(h, p);
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ret;
}

int heap_free(u32 handle, u32 p)
{
    if (!p)
        return 1;
    pthread_mutex_lock(&g_lock);
    heap_t *h = find(handle);
    int ok = h ? free_locked(h, p) : 0;
    pthread_mutex_unlock(&g_lock);
    if (!ok)
        fprintf(stderr, "[heap] HeapFree(%08x, %08x): not a live block\n", handle, p);
    return ok;
}

u32 heap_size(u32 handle, u32 p)
{
    (void)handle;
    return valid_block(p) ? rd32(g_mem, p - HDR) : 0xFFFFFFFFu;
}
