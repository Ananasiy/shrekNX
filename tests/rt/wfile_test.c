// Файлы гостя (runtime/hle/kernel32_file.c) против модели в памяти. Запись: буфер, правка накопленного на месте,
// сброс перед чтением/размером/закрытием, фоновая запись большого файла — случайные позиции (возвраты назад — как SavePackage к заголовку), записи
// мелкие и больше буфера, чтения посреди записи, размер, закрытие и повторное открытие. Чтение пакета игры: образ
// в памяти, который фоновый поток дочитывает, пока идут чтения (из образа и с диска вперемешку), и повторное
// открытие с готовым образом. Сами функции HLE — через стек гостя, как их зовёт сгенерированный код.
// Запуск: build/mac/tests/rt/wfile_test <пустой каталог>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runtime/abi.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/vfs.h"
#include "runtime/rt.h"

void hle_kernel32_CreateFileA(CPU *c);
void hle_kernel32_WriteFile(CPU *c);
void hle_kernel32_ReadFile(CPU *c);
void hle_kernel32_SetFilePointer(CPU *c);
void hle_kernel32_GetFileSize(CPU *c);
void hle_kernel32_CloseHandle(CPU *c);

#define STACK 0x00200000u  // стек вызовов (адреса гостя)
#define NAME 0x00300000u
#define IO 0x00400000u      // буфер данных гостя
#define OUT 0x00380000u     // *lpNumberOfBytes
#define MAXF (12u << 20)    // больше буфера записи (4 МБ)

static CPU cpu;
static u8 *model;
static u32 model_len;
static int fails;
static u64 rng = 0x9E3779B97F4A7C15ull;

static u32 rnd(u32 n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (u32)(rng % n);
}

static u32 call(void (*fn)(CPU *), int n, const u32 *args)
{
    cpu.esp = STACK - 4u * (u32)(n + 1);
    wr32(g_mem, cpu.esp, 0xDEADBEEFu);  // адрес возврата
    for (int i = 0; i < n; i++)
        wr32(g_mem, cpu.esp + 4u + 4u * (u32)i, args[i]);
    fn(&cpu);
    if (cpu.esp != STACK)
        printf("stack imbalance after call: %08x\n", cpu.esp), fails++;
    return cpu.eax;
}

static u32 open_file(u32 disp)
{
    u32 a[7] = {NAME, 0xC0000000u /* GENERIC_READ|WRITE */, 0, 0, disp, 0x80, 0};
    return call(hle_kernel32_CreateFileA, 7, a);
}

static void seek(u32 h, u32 pos)
{
    u32 a[4] = {h, pos, 0, 0};
    if (call(hle_kernel32_SetFilePointer, 4, a) != pos)
        printf("seek %u failed\n", pos), fails++;
}

static void write_at(u32 h, u32 pos, u32 n)
{
    seek(h, pos);
    for (u32 i = 0; i < n; i++)
        g_mem[IO + i] = (u8)rnd(256);
    u32 a[5] = {h, IO, n, OUT, 0};
    if (!call(hle_kernel32_WriteFile, 5, a) || rd32(g_mem, OUT) != n)
        printf("write %u at %u failed\n", n, pos), fails++;
    memcpy(model + pos, g_mem + IO, n);
    if (pos + n > model_len)
        model_len = pos + n;
}

static void check_read(u32 h, u32 pos, u32 n, const char *what)
{
    seek(h, pos);
    memset(g_mem + IO, 0xCD, n);
    u32 a[5] = {h, IO, n, OUT, 0};
    u32 want = pos >= model_len ? 0 : (pos + n > model_len ? model_len - pos : n);
    if (!call(hle_kernel32_ReadFile, 5, a) || rd32(g_mem, OUT) != want || memcmp(g_mem + IO, model + pos, want)) {
        if (fails++ < 10)
            printf("%s: read %u at %u: got %u bytes, want %u\n", what, n, pos, rd32(g_mem, OUT), want);
    }
}

static void check_size(u32 h)
{
    u32 a[2] = {h, 0};
    u32 sz = call(hle_kernel32_GetFileSize, 2, a);
    if (sz != model_len && fails++ < 10)
        printf("size %u, want %u\n", sz, model_len);
}

int main(int argc, char **argv)
{
    if (argc < 2 || rt_mem_init() != 0)
        return 2;
    char game[1024];
    snprintf(game, sizeof game, "%s/game", argv[1]);
    mkdir(argv[1], 0755);
    mkdir(game, 0755);
    vfs_init(game, argv[1], "C:\\Shrek2");
    strcpy((char *)g_mem + NAME, "C:\\Shrek2\\System\\Save.tmp");
    model = calloc(MAXF, 1);
    int ops = 0;
    for (int round = 0; round < 40; round++) {
        u32 h = open_file(round % 4 ? 4 /* OPEN_ALWAYS */ : 2 /* CREATE_ALWAYS */);
        if (h == 0xFFFFFFFFu) {
            printf("open failed\n");
            return 1;
        }
        if (round % 4 == 0)
            model_len = 0;
        for (int k = 0; k < 300; k++, ops++) {
            u32 kind = rnd(100), end = model_len;
            if (kind < 55) {  // дальше подряд, как FArchiveFileWriter
                u32 n = rnd(10) ? 1 + rnd(4096) : 1 + rnd(64 << 10);
                if (end + n < MAXF)
                    write_at(h, end, n);
            } else if (kind < 80) {  // назад внутрь записанного (заголовок, таблицы)
                u32 n = 1 + rnd(256);
                if (end > n)
                    write_at(h, rnd(end - n), n);
            } else if (kind < 83) {  // крупная запись, больше буфера
                u32 n = (4u << 20) + rnd(1u << 20);
                u32 pos = rnd(end + 1);
                if (pos + n < MAXF)
                    write_at(h, pos, n);
            } else if (kind < 86) {  // за конец с дырой
                u32 pos = end + 1 + rnd(5000), n = 1 + rnd(3000);
                if (pos + n < MAXF) {
                    memset(model + end, 0, pos - end);  // дыра читается нулями
                    write_at(h, pos, n);
                }
            } else if (kind < 95) {
                u32 pos = rnd(end + 100);
                check_read(h, pos, 1 + rnd(8192), "mid");
            } else {
                check_size(h);
            }
        }
        check_size(h);
        check_read(h, 0, model_len, "before close");
        u32 a[1] = {h};
        call(hle_kernel32_CloseHandle, 1, a);
        // с диска после закрытия
        char path[1100];
        snprintf(path, sizeof path, "%s/System/Save.tmp", argv[1]);
        FILE *f = fopen(path, "rb");
        u8 *disk = malloc(MAXF);
        size_t got = f ? fread(disk, 1, MAXF, f) : 0;
        if (f)
            fclose(f);
        if (got != model_len || memcmp(disk, model, model_len)) {
            if (fails++ < 10)
                printf("round %d: file on disk %zu bytes, want %u%s\n", round, got, model_len,
                       got == model_len ? " (content differs)" : "");
        }
        free(disk);
    }
    // как SavePackage: 2,2 МБ подряд мелкими кусками (уходят на диск фоном), затем заголовок
    // заново — внутрь уже записанного фоном
    {
        u32 h = open_file(2 /* CREATE_ALWAYS */);
        model_len = 0;
        while (model_len < (2200u << 10)) {
            write_at(h, model_len, 1 + rnd(16));
            ops++;
        }
        write_at(h, 0, 200);
        write_at(h, 300u << 10, 64);
        u32 a[1] = {h};
        call(hle_kernel32_CloseHandle, 1, a);
        char path[1100];
        snprintf(path, sizeof path, "%s/System/Save.tmp", argv[1]);
        FILE *f = fopen(path, "rb");
        u8 *disk = malloc(MAXF);
        size_t got = f ? fread(disk, 1, MAXF, f) : 0;
        if (f)
            fclose(f);
        if ((got != model_len || memcmp(disk, model, model_len)) && fails++ < 10)
            printf("save-like: file on disk %zu bytes, want %u\n", got, model_len);
        free(disk);
    }
    // чтение пакета из каталога игры: образ в памяти
    char pkg[1100];
    snprintf(pkg, sizeof pkg, "%s/Textures", game);
    mkdir(pkg, 0755);
    snprintf(pkg, sizeof pkg, "%s/Textures/Big.utx", game);
    model_len = (7u << 20) + 12345;
    for (u32 i = 0; i < model_len; i++)
        model[i] = (u8)rnd(256);
    FILE *pf = fopen(pkg, "wb");
    fwrite(model, 1, model_len, pf);
    fclose(pf);
    strcpy((char *)g_mem + NAME, "C:\\Shrek2\\Textures\\Big.utx");
    for (int round = 0; round < 3; round++) {
        if (round == 1)
            usleep(1500000);  // затишье: поток подкачки дочитывает образ
        unsigned long long reads0 = g_hitch.reads;
        u32 a7[7] = {NAME, 0x80000000u /* GENERIC_READ */, 1, 0, 3 /* OPEN_EXISTING */, 0x80, 0};
        u32 h = call(hle_kernel32_CreateFileA, 7, a7);
        if (h == 0xFFFFFFFFu) {
            printf("open package failed\n");
            return 1;
        }
        for (int k = 0; k < 3000; k++, ops++) {
            u32 n = rnd(4) ? 1 + rnd(2048) : 1 + rnd(300 << 10);
            check_read(h, rnd(model_len + 1000), n, "package");
        }
        check_read(h, 0, model_len, "package whole");
        u32 a[1] = {h};
        call(hle_kernel32_CloseHandle, 1, a);
        const char *cm = getenv("SNX_FILE_CACHE_MB");  // 0 — без образов: проверяются только данные
        if (round && !(cm && !atoi(cm)) && g_hitch.reads != reads0 && fails++ < 10)  // образ готов — с диска ничего
            printf("package round %d: %llu disk reads with a ready image\n", round, g_hitch.reads - reads0);
    }
    printf("wfile_test: %d operations, %d failed\n", ops, fails);
    return fails != 0;
}
