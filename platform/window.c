// Окно игры и GL-контекст поверх SDL2: главный поток — события, жизненный цикл ОС и создание окна,
// поток гостя/рендера — рисование.
#include "platform/window.h"

#include <SDL.h>
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <pthread.h>
#include <stdatomic.h>

#include "platform/gl.h"
#include "platform/platform.h"

plat_event_fn plat_on_event;
void (*plat_on_tick)(void);
void (*plat_on_focus)(int focused);
void (*plat_on_resume)(void);
void (*plat_on_screen_mode)(int w, int h);
void (*plat_on_exit_request)(void);

static SDL_Window *g_win;
static SDL_GLContext g_ctx;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int g_quit;
static _Atomic int g_focused = 1;
static _Atomic int g_screen_w;
static _Atomic int g_screen_h;
static _Atomic int g_screen_mode_dirty;
static _Atomic int g_gl_loaded;
#ifdef __SWITCH__
static AppletHookCookie g_applet_cookie;
static int g_applet_hooked;
#endif

// запрос окна из потока гостя, исполняется главным потоком
static struct {
    int pending, done, result;
    int w, h, fullscreen;
    char title[128];
} g_req;

static void handle_focus(int focused)
{
    focused = focused ? 1 : 0;
    if (atomic_exchange(&g_focused, focused) == focused)
        return;
    if (plat_on_focus)
        plat_on_focus(focused);
}

static void update_screen_mode(int notify)
{
    int w = 1280, h = 720;
#ifdef __SWITCH__
    // SDL на Switch всегда отдаёт 1920x1080; экран в руках — 1280x720
    if (appletGetOperationMode() == AppletOperationMode_Handheld) {
        w = 1280;
        h = 720;
    } else {
        w = 1920;
        h = 1080;
    }
#else
    SDL_DisplayMode m;
    if (SDL_GetDesktopDisplayMode(0, &m) == 0 && m.w > 0 && m.h > 0) {
        w = m.w;
        h = m.h;
    }
#endif
    int prev_w = atomic_exchange(&g_screen_w, w);
    int prev_h = atomic_exchange(&g_screen_h, h);
    if (w != prev_w || h != prev_h) {
#ifdef __SWITCH__
        SDL_Log("screen %dx%d (%s)", w, h, h == 720 ? "handheld" : "docked");
#else
        SDL_Log("screen %dx%d", w, h);
#endif
        if (notify) {
            atomic_store(&g_screen_mode_dirty, 1);
            if (plat_on_screen_mode)
                plat_on_screen_mode(w, h);
        }
    }
}

#ifdef __SWITCH__
// Хук жизненного цикла Horizon OS (libnx): вызывается из appletMainLoop() в главном потоке [проверить на Switch].
static void on_applet_hook(AppletHookType hook, void *param)
{
    (void)param;
    switch (hook) {
    case AppletHookType_OnFocusState: {
        AppletFocusState st = appletGetFocusState();
        handle_focus(st == AppletFocusState_InFocus);
        break;
    }
    case AppletHookType_OnResume:
        if (plat_on_resume)
            plat_on_resume();
        break;
    case AppletHookType_OnOperationMode:
        update_screen_mode(1);
        break;
    case AppletHookType_OnExitRequest:
        plat_quit();
        if (plat_on_exit_request)
            plat_on_exit_request();
        break;
    default:
        break;
    }
}
#endif

int plat_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return -1;
    }
    atomic_store(&g_quit, 0);
    atomic_store(&g_screen_mode_dirty, 0);
#ifdef __SWITCH__
    if (!g_applet_hooked) {
        appletHook(&g_applet_cookie, on_applet_hook, NULL);
        g_applet_hooked = 1;
    }
    atomic_store(&g_focused, appletGetFocusState() == AppletFocusState_InFocus ? 1 : 0);
#else
    atomic_store(&g_focused, 1);
#endif
    update_screen_mode(0);
    return 0;
}

static int create_window(void)
{
    if (g_win) {
#ifdef __SWITCH__
        // окно Switch — всегда весь экран; новое разрешение игры — только размер заднего буфера (растягивается
        // при показе). Менять окно SDL здесь нельзя: поверхность EGL занята потоком рендера
        return 0;
#endif
#ifdef __APPLE__
        if (!SDL_getenv("SNX_FULLSCREEN"))
            g_req.fullscreen = 0;
#endif
        SDL_SetWindowSize(g_win, g_req.w, g_req.h);
        SDL_SetWindowTitle(g_win, g_req.title);
        SDL_SetWindowFullscreen(g_win, g_req.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        return 0;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, SNX_GL_MAJOR);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, SNX_GL_MINOR);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    // кадр гостя рисуется во внеэкранный буфер и копируется в окно: у окна только цвет
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#ifdef __APPLE__
    // на маке полноэкранный режим игры — по SNX_FULLSCREEN=1 (иначе окно: удобнее отлаживать)
    if (!SDL_getenv("SNX_FULLSCREEN"))
        g_req.fullscreen = 0;
#endif
    if (g_req.fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    g_win = SDL_CreateWindow(g_req.title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, g_req.w, g_req.h, flags);
    if (!g_win) {
        SDL_Log("SDL_CreateWindow: %s", SDL_GetError());
        return -1;
    }
    g_ctx = SDL_GL_CreateContext(g_win);
    if (!g_ctx) {
        SDL_Log("SDL_GL_CreateContext: %s", SDL_GetError());
        return -1;
    }
    SDL_GL_MakeCurrent(g_win, NULL);  // контекст станет текущим в потоке гостя
    return 0;
}

void plat_run(void)
{
#ifdef __SWITCH__
    while (!atomic_load(&g_quit) && appletMainLoop()) {
#else
    while (!atomic_load(&g_quit)) {
#endif
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 5)) {
            do {
                if (ev.type == SDL_WINDOWEVENT) {
                    if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                        handle_focus(0);
                    else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                        handle_focus(1);
                }
                if (plat_on_event)
                    plat_on_event(&ev);
            } while (SDL_PollEvent(&ev));
        }
        if (plat_on_tick)
            plat_on_tick();
        pthread_mutex_lock(&g_lock);
        if (g_req.pending) {
            g_req.pending = 0;
            g_req.result = create_window();
            g_req.done = 1;
            pthread_cond_broadcast(&g_cv);
        }
        pthread_mutex_unlock(&g_lock);
    }
#ifdef __SWITCH__
    // appletMainLoop() вернул false без предварительного OnExitRequest — уведомить хост
    if (!atomic_exchange(&g_quit, 1) && plat_on_exit_request)
        plat_on_exit_request();
#endif
    // разбудить поток гостя, если он ждал создания окна во время выхода
    pthread_mutex_lock(&g_lock);
    if (g_req.pending) {
        g_req.pending = 0;
        g_req.result = -1;
        g_req.done = 1;
        pthread_cond_broadcast(&g_cv);
    }
    pthread_mutex_unlock(&g_lock);
}

void plat_quit(void)
{
    pthread_mutex_lock(&g_lock);
    atomic_store(&g_quit, 1);
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
}

#ifdef __SWITCH__
// Эксперимент (SNX_GLTHREAD=1): glthread Mesa — вызовы GL складываются в очередь, драйвер исполняет их в своём
// потоке (ядро процесса по умолчанию, 0). Mesa для Switch собрана с glthread, но EGL Switch его не включает;
// включаем сами и перепривязываем контекст, чтобы поток гостя получил таблицу маршалинга.
struct gl_context;
struct gl_context *_mesa_get_current_context(void);
void _mesa_glthread_init(struct gl_context *ctx);

static void start_glthread(void)
{
    struct gl_context *ctx = _mesa_get_current_context();
    if (!ctx)
        return;
    _mesa_glthread_init(ctx);
    SDL_GL_MakeCurrent(g_win, NULL);
    SDL_GL_MakeCurrent(g_win, g_ctx);
    SDL_Log("glthread on");
}
#endif

int plat_window_open(int w, int h, int fullscreen, const char *title)
{
    pthread_mutex_lock(&g_lock);
    if (atomic_load(&g_quit)) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    g_req.w = w;
    g_req.h = h;
    g_req.fullscreen = fullscreen;
    SDL_strlcpy(g_req.title, title ? title : "shrekNX", sizeof g_req.title);
    g_req.done = 0;
    g_req.pending = 1;
    while (!g_req.done && !atomic_load(&g_quit))
        pthread_cond_wait(&g_cv, &g_lock);
    int r = g_req.done ? g_req.result : -1;
    pthread_mutex_unlock(&g_lock);
    return r;
}

int plat_gl_bind(void)
{
    if (!g_win || !g_ctx)
        return -1;
    if (SDL_GL_MakeCurrent(g_win, g_ctx) != 0) {
        SDL_Log("SDL_GL_MakeCurrent: %s", SDL_GetError());
        return -1;
    }
    if (atomic_exchange(&g_gl_loaded, 1))
        return 0;
#if SNX_GL_NEEDS_LOADER
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        SDL_Log("gladLoadGL failed");
        atomic_store(&g_gl_loaded, 0);
        return -1;
    }
#endif
#ifdef __SWITCH__
    if (SDL_getenv("SNX_GLTHREAD"))
        start_glthread();
#endif
    SDL_GL_SetSwapInterval(SDL_getenv("SNX_NOVSYNC") ? 0 : 1);  // SNX_NOVSYNC=1 — для замеров
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(g_win, &dw, &dh);
    SDL_Log("GL %s | %s | GLSL %s | drawable %dx%d", glGetString(GL_VERSION), glGetString(GL_RENDERER),
            glGetString(GL_SHADING_LANGUAGE_VERSION), dw, dh);
    return 0;
}

void plat_gl_unbind(void)
{
    if (g_win)
        SDL_GL_MakeCurrent(g_win, NULL);
}

void plat_shutdown(void)
{
#ifdef __SWITCH__
    rt_platform_cpu_boost_reset();
    if (g_applet_hooked) {
        appletUnhook(&g_applet_cookie);
        g_applet_hooked = 0;
    }
#endif
    if (g_ctx) {
        SDL_GL_DeleteContext(g_ctx);
        g_ctx = NULL;
    }
    if (g_win) {
        SDL_DestroyWindow(g_win);
        g_win = NULL;
    }
    atomic_store(&g_gl_loaded, 0);
    atomic_store(&g_screen_w, 0);
    atomic_store(&g_screen_h, 0);
    SDL_Quit();
}

int plat_window_is_open(void) { return g_win != NULL; }

void plat_screen_size(int *w, int *h)
{
    // Читаем кеш, заполненный в plat_init() / OnOperationMode (главный поток), чтобы не звать SDL из других потоков
    int sw = atomic_load(&g_screen_w);
    int sh = atomic_load(&g_screen_h);
    if (sw <= 0 || sh <= 0) {
        update_screen_mode(0);
        sw = atomic_load(&g_screen_w);
        sh = atomic_load(&g_screen_h);
    }
    if (w)
        *w = sw;
    if (h)
        *h = sh;
}

int plat_take_screen_mode_changed(int *w, int *h)
{
    if (!atomic_exchange(&g_screen_mode_dirty, 0))
        return 0;
    if (w)
        *w = atomic_load(&g_screen_w);
    if (h)
        *h = atomic_load(&g_screen_h);
    return 1;
}

void plat_drawable_size(int *w, int *h)
{
    if (g_win)
        SDL_GL_GetDrawableSize(g_win, w, h);
    else
        *w = *h = 0;
}

void plat_swap(void)
{
    if (g_win)
        SDL_GL_SwapWindow(g_win);
}
