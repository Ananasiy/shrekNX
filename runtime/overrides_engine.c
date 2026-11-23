// Нативные обёртки функций Engine.dll (таблица — recomp/overrides.py, WRAPS).
#include <math.h>
#include <stdio.h>

#include "runtime/abi.h"
#include "runtime/rt.h"

// ---------------------------------------------------------------- Hor+ на широком экране
// FCameraSceneNode::UpdateMatrices строит проекцию FPerspectiveMatrix(FOV/2, FOV/2, 1, W/H): горизонтальный
// угол FOV (градусы, [node+0x1E0]) постоянен, и на 16:9 картинка обрезается по вертикали (vert-). Если кадр
// шире 4:3, матрицы строятся заново с горизонтальным углом, при котором вертикальный — как в 4:3 (Hor+).
// Отсечение по пирамиде видимости движок считает из этих же матриц. FOV узла после вызова — прежний.
// Узел: +0x94 ProjectionMatrix (M[0][0] +0x94, M[1][1] +0xA8, M[2][3] +0xC0, M[3][3] +0xD0).
extern void (*const ovr_orig_ovr_engine_camera_matrices)(CPU *c);

#define NODE_FOV 0x1E0u
#define PROJ 0x94u
#define SCREEN_TO_CAM 0xD4u
#define WORLD_TO_SCREEN 0x114u
#define SCREEN_TO_WORLD 0x154u
#define NODE_DET 0x1B8u

void ovr_engine_camera_matrices(CPU *c)
{
    u32 node = c->ecx, ret = rd32(g_mem, c->esp);
    RT_ENTER_GEN(c, ovr_orig_ovr_engine_camera_matrices);  // оригинал — код гостя
    float m00 = rdf32(g_mem, node + PROJ), m11 = rdf32(g_mem, node + PROJ + 0x14);
    float m23 = rdf32(g_mem, node + PROJ + 0x2C), m33 = rdf32(g_mem, node + PROJ + 0x3C);
    if (m23 != 1.0f || m33 != 0.0f || !(m00 > 0.0f))
        return;  // не перспектива (виды редактора, ортогональные)
    double aspect = (double)m11 / (double)m00, k = aspect / (4.0 / 3.0);
    float fov = rdf32(g_mem, node + NODE_FOV);
    if (k < 1.01 || !(fov > 1.0f && fov < 170.0f))
        return;
    // В режиме 4:3 для страниц меню (MenuAspect4x3) задний буфер 16:9 при Present сжимается по ширине до 4:3:
    // вместо Hor+ приводим вертикальный масштаб проекции к 4:3 (P' = P * diag(1, 1/k, 1, 1)), чтобы 3D-книга
    // сохранила исходные пропорции и границы без пустой области справа и без сжатия по горизонтали.
    if (__atomic_load_n(&rt_menu_aspect_4x3, __ATOMIC_RELAXED)) {
        float inv_k = (float)(1.0 / k), fk = (float)k;
        wrf32(g_mem, node + PROJ + 0x14u, m11 * inv_k);
        wrf32(g_mem, node + SCREEN_TO_CAM + 0x14u, rdf32(g_mem, node + SCREEN_TO_CAM + 0x14u) * fk);
        for (u32 r = 0; r < 4; r++) {
            u32 off = node + WORLD_TO_SCREEN + (r * 4u + 1u) * 4u;
            wrf32(g_mem, off, rdf32(g_mem, off) * inv_k);
        }
        for (u32 col = 0; col < 4; col++) {
            u32 off = node + SCREEN_TO_WORLD + (4u + col) * 4u;
            wrf32(g_mem, off, rdf32(g_mem, off) * fk);
        }
        wrf32(g_mem, node + NODE_DET, rdf32(g_mem, node + NODE_DET) * inv_k);
        return;
    }
    double wide = 2.0 * atan(tan((double)fov * M_PI / 360.0) * k) * 180.0 / M_PI;
    wrf32(g_mem, node + NODE_FOV, (float)wide);
    c->esp -= 4;  // повторный вызов тем же адресом возврата (thiscall без аргументов)
    wr32(g_mem, c->esp, ret);
    c->ecx = node;
    RT_ENTER_GEN(c, ovr_orig_ovr_engine_camera_matrices);  // оригинал — код гостя
    wrf32(g_mem, node + NODE_FOV, fov);
}

// Примечание по пропорциям 2D HUD на 16:9:
// UCanvas::Update (Engine.dll!0x120f95e0) переносит Viewport->SizeX/SizeY в Canvas->ClipX/ClipY (+0x40/+0x44)
// и SizeX/SizeY (+0x64/+0x68). Скрипты HUD (SHHud, HudItemHealthBar.RenderHud, HudItemCoins в SHGame.u)
// вычисляют X/ширину отдельно от Canvas.SizeX, а Y/высоту — от Canvas.SizeY, рисуя через общий UCanvas::DrawTile
// (Engine.dll!0x120f88a0), который также используется меню GUI, шрифтами, затемнениями экрана и экранами загрузки.
// Подмена координат в UCanvas::DrawTile без контекста виджета нарушит выравнивание текста, клик-области GUI и
// полноэкранные затемнения, поэтому страницы меню исправляются 4:3-pillarbox в d3d8_present (d3d8_set_menu_aspect),
// а адаптация внутриигрового HUD требует скоординированного перехвата в recomp/overrides.py + проверки в игре.

// ---------------------------------------------------------------- без сброса рендера после сохранения
// UGameEngine::Tick после отложенного сохранения (автосохранение, точки сохранения) зовёт SaveGame, затем
// Exec("RestartSounds") и Flush(1): все ресурсы рендера (текстуры, буферы мешей) выбрасываются, и следующий кадр
// прогревает уровень целиком — 36 МБ текстур и 14 тыс. вызовов отрисовки на болоте, на Switch ≈1,3 с из
// двухсекундного фриза сохранения. Уровень после сохранения тот же, и этот сброс пропускается: Flush с той же
// глубины стека гостя, что и SaveGame, и вскоре за местом его вызова (тот же Tick), ничего не делает. Консольный
// SAVEGAME сброса не зовёт. Каждый Flush — строка в лог (откуда и с каким аргументом), SaveGame — с его временем.
extern void (*const ovr_orig_ovr_engine_save_game)(CPU *c);
extern void (*const ovr_orig_ovr_engine_flush)(CPU *c);

static u32 g_save_esp, g_save_ret;  // стек и адрес возврата последнего SaveGame (0 — нет)

void ovr_engine_save_game(CPU *c)
{
    u32 esp = c->esp, ret = rd32(g_mem, esp), slot = rd32(g_mem, esp + 4);
    u64 t0 = rt_ticks_ns();
    RT_ENTER_GEN(c, ovr_orig_ovr_engine_save_game);  // оригинал — код гостя
    fprintf(stderr, "[engine] SaveGame(%d): %.0f ms\n", (int)slot, (double)(rt_ticks_ns() - t0) / 1e6);
    g_save_esp = esp;
    g_save_ret = ret;
}

void ovr_engine_flush(CPU *c)
{
    u32 ret = rd32(g_mem, c->esp), precache = rd32(g_mem, c->esp + 4);
    int skip = g_save_ret && c->esp == g_save_esp && ret - g_save_ret < 0x100u;
    g_save_ret = 0;
    fprintf(stderr, "[engine] Flush(%u) from %08x%s\n", precache, ret, skip ? ", right after SaveGame: skipped" : "");
    if (skip) {
        c->esp += 8;  // thiscall с одним аргументом: ret 4
        return;
    }
    RT_ENTER_GEN(c, ovr_orig_ovr_engine_flush);  // оригинал — код гостя
}
