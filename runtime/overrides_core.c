// Нативные замены функций Core.dll и копий memcpy в модулях (таблица — recomp/overrides.py).
#include <stdio.h>
#include <stdlib.h>

#include "runtime/abi.h"
#include "runtime/process.h"

// ---------------------------------------------------------------- обход свойств класса
// TFieldIterator<UProperty> (Core.dll, не экспорт): от текущего поля по цепочке Children/Next, затем в
// GetInheritanceSuper, пропускает всё, что не свойство. В цепочке класса после его свойств — его функции, состояния
// и перечисления (у Actor — сотни), и каждое из них — промах кеша: обход свойств объекта (сохранение, сборка мусора,
// копирование значений по умолчанию) у SerializeTaggedProperties при сохранении — четверть кадра. Цепочки полей
// меняются только при загрузке (пакет .u дописывает свойства в уже зарегистрированные нативные классы, UStruct::Link)
// и сборке мусора, и пропуск от первого не-свойства до следующего свойства запоминается: ключ — (структура, поле),
// значение — новые (структура, поле). Link и CollectGarbage сменяют эпоху — кеш забывается; до главного цикла
// (GIsRunning) и пока идёт загрузка (GObjBeginLoadCount) он не используется. Таблица своя у потока.
// SNX_FIELD_CHECK=1 — каждое попадание сверяется с обходом.
#define OBJ_CLASS 0x24u
#define FIELD_NEXT 0x30u
#define STRUCT_CHILDREN 0x40u
#define CLASS_CAST_FLAGS 0x49Cu
#define CAST_PROPERTY 0x8000u
#define VT_INHERITANCE_SUPER 0x80u

#define FN_BITS 14u
#define FN_SIZE (1u << FN_BITS)

typedef struct {
    u32 st, f, st2, f2;
} fn_entry;

typedef struct {
    u32 epoch, used;
    fn_entry e[FN_SIZE];
} fn_cache;

static u32 g_fn_epoch = 1;
static int g_fn_check = -1;
static u32 g_load_count_va, g_running_va;  // Core: UObject::GObjBeginLoadCount, GIsRunning
static __thread fn_cache *t_fn;

static inline int is_property(const u8 *m, u32 f)
{
    return (rd32(m, rd32(m, f + OBJ_CLASS) + CLASS_CAST_FLAGS) & CAST_PROPERTY) != 0;
}

// пропуск оригинала: от (st, f) до свойства или до конца иерархии (0, 0)
static void field_walk(CPU *c, u32 *pst, u32 *pf)
{
    const u8 *m = g_mem;
    u32 st = *pst, f = *pf;
    for (;;) {
        for (; f; f = rd32(m, f + FIELD_NEXT))
            if (is_property(m, f))
                goto out;
        c->ecx = st;
        st = rt_guest_call(c, rd32(m, rd32(m, st) + VT_INHERITANCE_SUPER), 0, NULL);
        if (!st)
            break;
        f = rd32(m, st + STRUCT_CHILDREN);
    }
out:
    *pst = st;
    *pf = f;
}

void ovr_core_field_next(CPU *c)
{
    u8 *m = g_mem;
    u32 it = c->ecx, st = rd32(m, it), f = rd32(m, it + 4);
    c->esp += 4;
    if (!st || (f && is_property(m, f)))
        return;
    if (!g_running_va) {
        rt_module *core = rt_module_by_name("Core");
        g_load_count_va = rt_module_export(core, "?GObjBeginLoadCount@UObject@@0HA", 0);
        g_running_va = rt_module_export(core, "?GIsRunning@@3HA", 0);
        if (!g_load_count_va || !g_running_va)
            g_running_va = 1;  // нет экспортов — кеш выключен (адрес 1 — не ноль)
    }
    fn_cache *t = t_fn;
    if (!g_load_count_va || !rd32(m, g_running_va) || rd32(m, g_load_count_va) ||
        (!t && !(t = t_fn = calloc(1, sizeof *t)))) {
        field_walk(c, &st, &f);
        goto store;
    }
    u32 ep = __atomic_load_n(&g_fn_epoch, __ATOMIC_RELAXED);
    if (t->epoch != ep || t->used > FN_SIZE / 2) {
        memset(t->e, 0, sizeof t->e);
        t->epoch = ep;
        t->used = 0;
    }
    u32 h = ((st * 0x9E3779B1u) ^ (f * 0x85EBCA6Bu)) >> (32 - FN_BITS);
    fn_entry *e;
    for (;; h = (h + 1) & (FN_SIZE - 1)) {
        e = &t->e[h];
        if (e->st == st && e->f == f) {
            if (g_fn_check < 0)
                g_fn_check = getenv("SNX_FIELD_CHECK") != NULL;
            if (g_fn_check) {
                u32 st2 = st, f2 = f;
                field_walk(c, &st2, &f2);
                if (st2 != e->st2 || f2 != e->f2) {
                    fprintf(stderr, "[core] field cache: (%08x, %08x) -> (%08x, %08x), walk (%08x, %08x)\n", st, f,
                            e->st2, e->f2, st2, f2);
                    abort();
                }
            }
            st = e->st2;
            f = e->f2;
            goto store;
        }
        if (!e->st)
            break;
    }
    u32 st0 = st, f0 = f;
    field_walk(c, &st, &f);
    *e = (fn_entry){st0, f0, st, f};
    t->used++;
store:
    wr32(m, it, st);
    wr32(m, it + 4, f);
}

// UObject::CollectGarbage (освобождённые классы) и UStruct::Link (новые поля): кеш обхода свойств устарел
extern void (*const ovr_orig_ovr_core_collect_garbage)(CPU *c);
extern void (*const ovr_orig_ovr_core_struct_link)(CPU *c);

void ovr_core_collect_garbage(CPU *c)
{
    __atomic_add_fetch(&g_fn_epoch, 1, __ATOMIC_RELAXED);
    RT_ENTER_GEN(c, ovr_orig_ovr_core_collect_garbage);  // оригинал — код гостя
    __atomic_add_fetch(&g_fn_epoch, 1, __ATOMIC_RELAXED);
}

void ovr_core_struct_link(CPU *c)
{
    __atomic_add_fetch(&g_fn_epoch, 1, __ATOMIC_RELAXED);
    RT_ENTER_GEN(c, ovr_orig_ovr_core_struct_link);  // оригинал — код гостя
    __atomic_add_fetch(&g_fn_epoch, 1, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------- memcpy модулей
// Своя копия у каждой DLL (из статического CRT, cdecl dst, src, n): на SSE — выравнивание и хвост переходами по
// вычисленному адресу (jmp ecx), а это поиск функции по адресу на каждом переходе. Зовут её и на 1–4 байта
// (FArchiveFileWriter::Serialize на каждое поле при сохранении).
void ovr_memcpy(CPU *c)
{
    u32 dst = rd32(g_mem, c->esp + 4), src = rd32(g_mem, c->esp + 8), n = rd32(g_mem, c->esp + 12);
    memmove(g_mem + dst, g_mem + src, n);
    c->eax = dst;
    c->esp += 4;
}

// ---------------------------------------------------------------- запись файла (game.exe)
// FArchiveFileWriter::Serialize(V, Length): копия в буфер 4 КБ архива, полный буфер — виртуальный Flush (WriteFile).
// Сохранение пишет так каждое поле по 1–4 байта; помещается в буфер — копия здесь же, иначе — оригинал.
#define FW_POS 0x48u
#define FW_COUNT 0x4Cu
#define FW_BUF 0x50u
#define FW_BUF_SIZE 0x1000u

extern void (*const ovr_orig_ovr_launch_file_write)(CPU *c);

void ovr_launch_file_write(CPU *c)
{
    u8 *m = g_mem;
    u32 ar = c->ecx, v = rd32(m, c->esp + 4);
    s32 len = (s32)rd32(m, c->esp + 8), cnt = (s32)rd32(m, ar + FW_COUNT);
    if (len < 0 || cnt < 0 || len > (s32)FW_BUF_SIZE - cnt) {
        RT_ENTER_GEN(c, ovr_orig_ovr_launch_file_write);  // оригинал — код гостя
        return;
    }
    wr32(m, ar + FW_POS, rd32(m, ar + FW_POS) + (u32)len);
    memmove(m + ar + FW_BUF + (u32)cnt, m + v, (u32)len);
    wr32(m, ar + FW_COUNT, (u32)(cnt + len));
    c->esp += 12;  // thiscall, два аргумента: ret 8
}
