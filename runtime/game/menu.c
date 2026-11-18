// Меню игры для ввода хоста (см. menu.h). GUI игры — UE2 GUI (UT2003): GUIController (у Shrek 2 —
// ShGUIController в пакете Transient) держит ActivePage; страница — GUIMultiComponent со списком Controls;
// у элемента GUIComponent есть bVisible, bAcceptsInput, MenuState (2 — MSAT_Focused, 4 — MSAT_Disabled) и
// Bounds[4] — левый, верхний, правый, нижний край в пикселях заднего буфера (считаются при отрисовке).
//
// Геймпад в меню — родными средствами GUI. Фокус: событие SetFocus элемента (как у клавиатуры; фокус идёт
// цепочкой GUIMultiComponent.FocusedControl от страницы к элементу, элемент в фокусе подсвечивается стилем
// игры). Соседа в направлении крестовины ищем сами по Bounds: ссылки Links[4], по которым ходит код Xbox
// (UGUIMultiComponent::XControllerEvent), в версии для ПК пусты. Курсор GUI — GUIController.MouseX/MouseY
// (пиксели заднего буфера; мышь двигает их смещениями IK_MouseX/IK_MouseY): при переходе крестовиной курсор
// встаёт на элемент в фокусе и прячется (bHideMouseCursor) — наведение мышью не подсвечивает другой элемент,
// а A — обычный щелчок по нему.
#include "runtime/game/menu.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "runtime/game/uobj.h"

#define MAX_ITEMS 96

static u32 g_ctrl;  // GUIController игры
static u32 g_frames;
static struct {
    u32 active_page, controls, menu_state, bounds, focused, owner, mouse_x, mouse_y;
    uo_prop visible, accepts, hide_cursor, never_focus, force_check;
    int ok;
} g_off;
enum { C_MULTI = 1, C_SLIDER = 2, C_COMBO = 4, C_TABS = 8 };
static struct {
    u32 cls, kind, caption;  // C_*; смещение FString Caption (0 — нет)
} g_cls[96];                 // кеш по классам

typedef struct {
    u32 comp;
    game_rect r;
    int kind;  // GM_*
} item;

// состояние для хоста и запросы от него
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_open, g_focus_kind, g_has_back;
static game_rect g_back;
static int g_req_nav[8], g_req_nav_n, g_req_tab, g_req_cursor = -1;
static unsigned g_point_req, g_point_done;  // номер последнего запроса точки и исполненного
static float g_point_x, g_point_y;
static u32 g_point_frame;  // кадр, в котором точка поставлена
static int g_moved;        // гость сам поставил курсор (g_moved_x, g_moved_y)
static float g_moved_x, g_moved_y;

static int cls_slot(u32 obj)
{
    u32 cls = uo_class(obj);
    for (int i = 0; i < 96; i++) {
        if (g_cls[i].cls == cls)
            return i;
        if (!g_cls[i].cls) {
            uo_prop p;
            g_cls[i].cls = cls;
            g_cls[i].kind = (uo_is_a(obj, "GUIMultiComponent") ? C_MULTI : 0) | (uo_is_a(obj, "GUISlider") ? C_SLIDER : 0) |
                            (uo_is_a(obj, "GUIComboBox") ? C_COMBO : 0) | (uo_is_a(obj, "GUITabControl") ? C_TABS : 0);
            g_cls[i].caption = uo_find_prop(cls, "Caption", &p) ? p.offset : 0;
            return i;
        }
    }
    return -1;
}

static u32 kind_of(u32 obj)
{
    int i = cls_slot(obj);
    return i >= 0 ? g_cls[i].kind : 0;
}

static int is_multi(u32 obj) { return (kind_of(obj) & C_MULTI) != 0; }

// буква в верхний регистр: латиница и кириллица (А-Я, Ё)
static u32 upper16(u32 ch)
{
    if ((ch >= 'a' && ch <= 'z') || (ch >= 0x430 && ch <= 0x44F))
        return ch - 0x20;
    return ch == 0x451 ? 0x401 : ch;
}

// подпись элемента — кнопка «назад» (BACK, CANCEL, NO; в русской версии НАЗАД, ОТМЕНИТЬ, НЕТ)?
static int is_back(u32 obj)
{
    static const u16 back[] = {'B', 'A', 'C', 'K', 0}, cancel[] = {'C', 'A', 'N', 'C', 'E', 'L', 0}, no[] = {'N', 'O', 0},
                     nazad[] = {0x41D, 0x410, 0x417, 0x410, 0x414, 0}, otmenit[] = {0x41E, 0x422, 0x41C, 0x415, 0x41D, 0x418, 0x422, 0x42C, 0},
                     otmena[] = {0x41E, 0x422, 0x41C, 0x415, 0x41D, 0x410, 0}, net[] = {0x41D, 0x415, 0x422, 0};
    static const u16 *const names[] = {back, cancel, no, nazad, otmenit, otmena, net};
    int i = cls_slot(obj);
    if (i < 0 || !g_cls[i].caption)
        return 0;
    u32 str = obj + g_cls[i].caption, data = rd32(g_mem, str), num = rd32(g_mem, str + 4);
    for (size_t k = 0; data && num && k < sizeof names / sizeof names[0]; k++) {
        u32 j = 0;
        while (names[k][j] && j + 1 < num && upper16(rd16(g_mem, data + 2 * j)) == names[k][j])
            j++;
        if (!names[k][j] && j + 1 == num)
            return 1;
    }
    return 0;
}

static u32 find_controller(void)
{
    u32 cls = uo_find_class("GUIController");
    for (u32 i = 0, n = cls ? uo_count() : 0; i < n; i++) {
        u32 o = uo_at(i);
        if (o && uo_is_a_class(o, cls))
            return o;
    }
    return 0;
}

static int init_offsets(u32 page)
{
    static const struct {
        int ctrl;  // свойство GUIController (иначе — элемента страницы)
        const char *name;
        u32 *off;
    } props[] = {
        {1, "MouseX", &g_off.mouse_x}, {1, "MouseY", &g_off.mouse_y}, {0, "Controls", &g_off.controls},
        {0, "MenuState", &g_off.menu_state}, {0, "Bounds", &g_off.bounds}, {0, "FocusedControl", &g_off.focused},
        {0, "MenuOwner", &g_off.owner},
    };
    uo_prop p;
    for (size_t i = 0; i < sizeof props / sizeof props[0]; i++) {
        if (!uo_find_prop(uo_class(props[i].ctrl ? g_ctrl : page), props[i].name, &p))
            return 0;
        *props[i].off = p.offset;
    }
    if (!uo_find_prop(uo_class(page), "bVisible", &g_off.visible) ||
        !uo_find_prop(uo_class(page), "bAcceptsInput", &g_off.accepts) ||
        !uo_find_prop(uo_class(page), "bNeverFocus", &g_off.never_focus) ||
        !uo_find_prop(uo_class(g_ctrl), "bHideMouseCursor", &g_off.hide_cursor) ||
        !uo_find_prop(uo_class(g_ctrl), "bForceMouseCheck", &g_off.force_check))
        return 0;
    return g_off.ok = 1;
}

static int flag(u32 obj, const uo_prop *p) { return (rd32(g_mem, obj + p->offset) & p->bitmask) != 0; }
static void set_flag(u32 obj, const uo_prop *p, int v)
{
    u32 w = rd32(g_mem, obj + p->offset);
    wr32(g_mem, obj + p->offset, v ? w | p->bitmask : w & ~p->bitmask);
}

static void rect_of(u32 comp, game_rect *r)
{
    r->x0 = rdf32(g_mem, comp + g_off.bounds);
    r->y0 = rdf32(g_mem, comp + g_off.bounds + 4);
    r->x1 = rdf32(g_mem, comp + g_off.bounds + 8);
    r->y1 = rdf32(g_mem, comp + g_off.bounds + 12);
}

// ---------------------------------------------------------------- правки страниц настроек
// Панели вкладок настроек (и в игре, и в главном меню — подклассы): Tab_OptionsGame — клавиши, скорость мыши,
// Reset; Tab_OptionsVideo — яркость, разрешение, глубина цвета, рендерер, детализация. Ссылки на элементы —
// свойства панели (btnKeyForward, cbResolution...). Правки повторяются каждый кадр, пока панель видна: игра
// сама переписывает подписи и видимость при показе вкладки.
static u32 g_cls_game, g_cls_video;
static int g_cls_tries;

static u32 member(u32 panel, const char *name)
{
    uo_prop p;
    return uo_find_prop(uo_class(panel), name, &p) ? rd32(g_mem, panel + p.offset) : 0;
}

static void hide(u32 comp)
{
    if (comp)
        set_flag(comp, &g_off.visible, 0);
}

// вкладка GAME — схема управления геймпадом: подписи строк и кнопок (переназначение клавиш выключено)
static int g_rus;  // русская версия (game_menu_set_language)

static const struct {
    const char *label, *button, *caption[2], *keys[2];  // [0] — английская, [1] — русская
} g_rows[] = {
    {"lblKeyForward", "btnKeyForward", {"MOVE", "ДВИЖЕНИЕ"}, {"LEFT STICK", "ЛЕВЫЙ СТИК"}},
    {"lblKeyBackward", "btnKeyBackward", {"CAMERA", "КАМЕРА"}, {"RIGHT STICK", "ПРАВЫЙ СТИК"}},
    {"lblKeyTurnLeft", "btnKeyTurnLeft", {"ACTION", "ДЕЙСТВИЕ"}, {"X", "X"}},
    {"lblKeyTurnRight", "btnKeyTurnRight", {"POTIONS", "ЗЕЛЬЯ"}, {"ZL + D-PAD, X A B Y, R", "ZL + КРЕСТОВИНА, X A B Y, R"}},
    {"lblKeyJump", "btnKeyJump", {"JUMP", "ПРЫЖОК"}, {"B", "B"}},
    {"lblKeyAttack", "btnKeyAttack", {"ATTACK", "УДАР"}, {"A", "A"}},
};

// делегат элемента — пустой (Object и FunctionName — 0): движок зовёт тогда пустую реализацию GUIComponent
static void unbind(u32 comp, const char *delegate)
{
    uo_prop p;
    if (comp && uo_find_prop(uo_class(comp), delegate, &p)) {
        wr32(g_mem, comp + p.offset, 0);
        wr32(g_mem, comp + p.offset + 4, 0);
    }
}

static void patch_game(u32 panel)
{
    for (size_t i = 0; i < sizeof g_rows / sizeof g_rows[0]; i++) {
        uo_set_str(member(panel, g_rows[i].label), "Caption", g_rows[i].caption[g_rus]);
        u32 b = member(panel, g_rows[i].button);
        if (b) {
            uo_set_str(b, "Caption", g_rows[i].keys[g_rus]);
            set_flag(b, &g_off.accepts, 0);
            // Щелчок (или нажатие «мыши», пока кнопка в фокусе) запускает у панели ожидание клавиши (bPolling), и
            // следующая нажатая кнопка — в том числе геймпада — забирает действие строки: Tab_OptionsGame.ProcessKey
            // стирает его у остальных клавиш (W, Ctrl, Joy2...) и сохраняет в User.ini. Обработчики кнопок клавиш
            // (OnClick панели, OnKeyEvent — TabOptionsInput_OnKeyEvent) снимаем.
            unbind(b, "__OnClick__Delegate");
            unbind(b, "__OnKeyEvent__Delegate");
        }
    }
    uo_set_bool(panel, "bPolling", 0);
    uo_set_str(member(panel, "lblMouseSpeedGame"), "Caption", g_rus ? "СКОРОСТЬ КАМЕРЫ" : "LOOK SPEED");
    hide(member(panel, "btnReset"));  // сброс клавиш стёр бы и привязки геймпада в User.ini
}

// за край экрана (WinLeft больше 1 — в пикселях): видимость этим элементам игра выставляет заново каждый кадр
static void move_away(u32 comp)
{
    uo_prop p;
    if (comp && uo_find_prop(uo_class(comp), "WinLeft", &p))
        wrf32(g_mem, comp + p.offset, 100000.0f);
}

// разрешение и глубину цвета задаёт порт (экран Switch), рендерер — только D3D
static void patch_video(u32 panel)
{
    static const char *const names[] = {"lblResolution", "cbResolution", "lblColorDepth", "cbColorDepth",
                                        "lblRenderer", "cbRenderer"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        move_away(member(panel, names[i]));
}

static void patch(u32 comp)
{
    if (g_cls_game && uo_is_a_class(comp, g_cls_game))
        patch_game(comp);
    else if (g_cls_video && uo_is_a_class(comp, g_cls_video))
        patch_video(comp);
}

// ---------------------------------------------------------------- элементы страницы
static unsigned g_bb_w, g_bb_h;
static item g_items[MAX_ITEMS];
static int g_n;
static u32 g_tabs;  // видимый GUITabControl страницы
static int g_back_i;

static void add(u32 c, int kind)
{
    if (g_n >= MAX_ITEMS || g_mem[c + g_off.menu_state] == 4)
        return;
    game_rect r;
    rect_of(c, &r);
    if (r.x1 - r.x0 < 2 || r.y1 - r.y0 < 2 || r.x0 < 0 || r.y0 < 0 || r.x1 > (float)g_bb_w || r.y1 > (float)g_bb_h)
        return;
    if (g_back_i < 0 && kind == GM_BUTTON && is_back(c))
        g_back_i = g_n;
    g_items[g_n++] = (item){c, r, kind};
}

// элементы, принимающие ввод, — в g_items (рекурсивно по Controls); выпадающий список — один элемент
static void collect(u32 comp, int depth)
{
    u32 arr = rd32(g_mem, comp + g_off.controls), num = rd32(g_mem, comp + g_off.controls + 4);
    for (u32 i = 0; i < num && i < 256; i++) {
        u32 c = rd32(g_mem, arr + 4 * i);
        if (!c || !flag(c, &g_off.visible))
            continue;
        u32 k = kind_of(c);
        if (k & C_MULTI)
            patch(c);
        if (!flag(c, &g_off.visible))
            continue;
        if (k & C_COMBO) {
            add(c, GM_COMBO);
            continue;
        }
        if ((k & C_TABS) && !g_tabs)
            g_tabs = c;
        if (flag(c, &g_off.accepts))
            add(c, k & C_SLIDER ? GM_SLIDER : GM_BUTTON);
        if (depth < 6 && (k & C_MULTI))
            collect(c, depth + 1);
    }
}

// элемент в фокусе: конец цепочки FocusedControl от страницы, если он среди g_items (или внутри одного из них)
static int focused_item(u32 page)
{
    u32 cur = page;
    for (int d = 0; d < 8 && is_multi(cur); d++) {
        u32 f = rd32(g_mem, cur + g_off.focused);
        if (!f || f == cur || !flag(f, &g_off.visible))
            break;
        cur = f;
    }
    if (cur == page || g_mem[cur + g_off.menu_state] != 2)
        return -1;
    for (u32 c = cur, d = 0; c && d < 4; c = rd32(g_mem, c + g_off.owner), d++)
        for (int i = 0; i < g_n; i++)
            if (g_items[i].comp == c)
                return i;
    return -1;
}

// сосед элемента from в направлении dir (0 вверх, 1 вниз, 2 влево, 3 вправо); from < 0 — первый по порядку
// чтения (сверху вниз, слева направо)
static int neighbor(int from, int dir)
{
    int best = -1;
    float best_cost = 1e30f;
    for (int i = 0; i < g_n; i++) {
        const game_rect *r = &g_items[i].r;
        float cost;
        if (from < 0)
            cost = r->y0 * 4.0f + r->x0;
        else {
            const game_rect *f = &g_items[from].r;
            if (i == from)
                continue;
            float dx = (r->x0 + r->x1 - f->x0 - f->x1) / 2, dy = (r->y0 + r->y1 - f->y0 - f->y1) / 2;
            float along = dir == 0 ? -dy : dir == 1 ? dy : dir == 2 ? -dx : dx;
            float perp = dir < 2 ? dx : dy;
            // перекрытие по поперечной оси — сосед «прямо» в этом направлении
            float lo = dir < 2 ? (r->x0 > f->x0 ? r->x0 : f->x0) : (r->y0 > f->y0 ? r->y0 : f->y0);
            float hi = dir < 2 ? (r->x1 < f->x1 ? r->x1 : f->x1) : (r->y1 < f->y1 ? r->y1 : f->y1);
            perp = perp < 0 ? -perp : perp;
            // целиком дальше края в этом направлении и не слишком вбок (без перекрытия — в конусе ±63°): справа от
            // широкой «Настройки» и от «Выход» нет ничего — фокус остаётся, а не прыгает на ячейку сбоку
            int beyond = dir == 0 ? r->y1 <= f->y0 + 4.0f : dir == 1 ? r->y0 >= f->y1 - 4.0f
                       : dir == 2 ? r->x1 <= f->x0 + 4.0f : r->x0 >= f->x1 - 4.0f;
            if (along < 4.0f || !beyond || (hi <= lo && perp > 2.0f * along))
                continue;
            cost = along + (hi > lo ? 0.0f : 2.0f * perp);
        }
        if (cost < best_cost) {
            best_cost = cost;
            best = i;
        }
    }
    return best;
}

// Курсор GUI. Элемент под курсором (ActiveControl, ему достаётся нажатие) GUI ищет заново при движении мыши; после
// клавиши (стрелка у ползунка) там остаётся прежний, и щелчок по «Назад» уходил странице — bForceMouseCheck.
static void set_mouse(float x, float y)
{
    wrf32(g_mem, g_ctrl + g_off.mouse_x, x);
    wrf32(g_mem, g_ctrl + g_off.mouse_y, y);
    set_flag(g_ctrl, &g_off.force_check, 1);
}

static void focus(int i)
{
    u32 none = 0;
    const game_rect *r = &g_items[i].r;
    if (getenv("SNX_LOG_MENU"))
        fprintf(stderr, "[menu] focus %s (%.0f,%.0f)-(%.0f,%.0f) of %d items\n", uo_name(g_items[i].comp), r->x0, r->y0,
                r->x1, r->y1, g_n);
    // кнопки меню ПК (главная страница книги) — bNeverFocus: SetFocus их не трогает; с геймпада фокус им нужен
    set_flag(g_items[i].comp, &g_off.never_focus, 0);
    uo_call(g_items[i].comp, "SetFocus", &none, 1, NULL);
    float x = (r->x0 + r->x1) / 2, y = (r->y0 + r->y1) / 2;
    set_mouse(x, y);
    set_flag(g_ctrl, &g_off.hide_cursor, 1);
    pthread_mutex_lock(&g_lock);
    g_moved = 1;
    g_moved_x = x;
    g_moved_y = y;
    pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------- кадр
void game_menu_frame(unsigned bb_w, unsigned bb_h)
{
    static u32 last_page;
    static int settle;  // кадров до автофокуса новой страницы
    if (!uo_init())
        return;
    g_frames++;
    // контроллер ищется один раз (живёт всю игру); раз в 10 с — проверка, что он всё ещё на месте
    if (g_ctrl && g_frames % 600 == 0 && uo_at(rd32(g_mem, g_ctrl + 4)) != g_ctrl)
        g_ctrl = 0;
    if (!g_ctrl && g_frames % 120 == 1 && (g_ctrl = find_controller()))
        fprintf(stderr, "[menu] GUI controller %s at %08x\n", uo_name(g_ctrl), g_ctrl);
    u32 page = 0;
    if (g_ctrl) {
        uo_prop p;
        if (!g_off.active_page && uo_find_prop(uo_class(g_ctrl), "ActivePage", &p))
            g_off.active_page = p.offset;
        page = g_off.active_page ? rd32(g_mem, g_ctrl + g_off.active_page) : 0;
    }
    if (page && !g_cls_game && g_cls_tries++ % 60 == 0) {
        g_cls_game = uo_find_class("Tab_OptionsGame");
        g_cls_video = uo_find_class("Tab_OptionsVideo");
    }
    g_n = 0;
    g_tabs = 0;
    g_back_i = -1;
    g_bb_w = bb_w;
    g_bb_h = bb_h;
    if (page && !g_off.ok && !init_offsets(page))
        page = 0;
    if (page)
        collect(page, 0);
    int fi = page ? focused_item(page) : -1;

    int nav[9], nav_n, tab, cursor, point = 0;
    float px = 0, py = 0;
    pthread_mutex_lock(&g_lock);
    nav_n = g_req_nav_n;
    memcpy(nav, g_req_nav, sizeof g_req_nav);
    g_req_nav_n = 0;
    tab = g_req_tab;
    g_req_tab = 0;
    cursor = g_req_cursor;
    g_req_cursor = -1;
    if (g_point_req != g_point_done) {
        point = 1;
        px = g_point_x;
        py = g_point_y;
    }
    pthread_mutex_unlock(&g_lock);

    if (page) {
        if (page != last_page)
            settle = 3;  // Bounds новой страницы считаются при её первой отрисовке
        else if (settle && !--settle && fi < 0 && cursor != 1 && g_n)
            nav[nav_n++] = -1;  // сразу фокус на первый элемент, как на консоли
        if (tab && g_tabs)
            uo_call(g_tabs, tab > 0 ? "NextPage" : "PrevPage", NULL, 0, NULL);
        if (tab && getenv("SNX_LOG_MENU"))
            fprintf(stderr, "[menu] tab %+d (%s)\n", tab, g_tabs ? uo_name(g_tabs) : "no tab control");
        for (int k = 0; k < nav_n && !tab; k++) {
            int to = neighbor(nav[k] < 0 ? -1 : fi, nav[k]);
            if (to >= 0 && to != fi)
                focus(fi = to);
        }
        if (cursor >= 0)
            set_flag(g_ctrl, &g_off.hide_cursor, !cursor);
        if (point) {
            set_mouse(px, py);
            set_flag(g_ctrl, &g_off.hide_cursor, 1);
        }
    }
    last_page = page;
    pthread_mutex_lock(&g_lock);
    g_open = page != 0;
    g_focus_kind = fi >= 0 ? g_items[fi].kind : GM_NONE;
    g_has_back = g_back_i >= 0;
    if (g_has_back)
        g_back = g_items[g_back_i].r;
    if (point) {
        g_point_done = g_point_req;
        g_point_frame = g_frames;
    }
    pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------- для хоста
void game_menu_set_language(const char *lang) { g_rus = lang && !strncasecmp(lang, "rus", 3); }

int game_menu_open(int *focus_kind)
{
    pthread_mutex_lock(&g_lock);
    int open = g_open;
    if (focus_kind)
        *focus_kind = g_focus_kind;
    pthread_mutex_unlock(&g_lock);
    return open;
}

int game_menu_back(game_rect *r)
{
    pthread_mutex_lock(&g_lock);
    int has = g_open && g_has_back;
    if (has)
        *r = g_back;
    pthread_mutex_unlock(&g_lock);
    return has;
}

void game_menu_nav(int dir)
{
    pthread_mutex_lock(&g_lock);
    if (g_req_nav_n < 8)
        g_req_nav[g_req_nav_n++] = dir;
    pthread_mutex_unlock(&g_lock);
}

void game_menu_tab(int d)
{
    pthread_mutex_lock(&g_lock);
    g_req_tab = d;
    pthread_mutex_unlock(&g_lock);
}

unsigned game_menu_point(float x, float y)
{
    pthread_mutex_lock(&g_lock);
    unsigned t = ++g_point_req;
    g_point_x = x;
    g_point_y = y;
    pthread_mutex_unlock(&g_lock);
    return t;
}

int game_menu_point_done(unsigned ticket)
{
    pthread_mutex_lock(&g_lock);
    // поставлен, и после этого прошёл ещё кадр: GUI нашёл элемент под курсором
    int done = (int)(g_point_done - ticket) >= 0 && g_frames != g_point_frame;
    pthread_mutex_unlock(&g_lock);
    return done;
}

void game_menu_cursor(int visible)
{
    pthread_mutex_lock(&g_lock);
    g_req_cursor = visible;
    pthread_mutex_unlock(&g_lock);
}

int game_menu_cursor_moved(float *x, float *y)
{
    pthread_mutex_lock(&g_lock);
    int m = g_moved;
    if (m) {
        *x = g_moved_x;
        *y = g_moved_y;
        g_moved = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return m;
}
