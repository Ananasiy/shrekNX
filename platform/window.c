// Окно игры и GL-контекст поверх SDL2: главный поток — события и создание окна, поток гостя — рисование.
#include "platform/window.h"

#include <SDL.h>
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <pthread.h>

#include "platform/gl.h"

plat_event_fn plat_on_event;
void (*plat_on_tick)(void);

static SDL_Window *g_win;
static SDL_GLContext g_ctx;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static volatile int g_quit;

// запрос окна из потока гостя, исполняется главным потоком
static struct {
    int pending, done, result;
    int w, h, fullscreen;
    char title[128];
} g_req;

int plat_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return -1;
    }
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
    while (!g_quit) {
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 5)) {
            do {
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
}

void plat_quit(void) { g_quit = 1; }

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
    g_req.w = w;
    g_req.h = h;
    g_req.fullscreen = fullscreen;
    SDL_strlcpy(g_req.title, title ? title : "shrekNX", sizeof g_req.title);
    g_req.done = 0;
    g_req.pending = 1;
    while (!g_req.done)
        pthread_cond_wait(&g_cv, &g_lock);
    int r = g_req.result;
    pthread_mutex_unlock(&g_lock);
    return r;
}

int plat_gl_bind(void)
{
    static int loaded;
    if (SDL_GL_MakeCurrent(g_win, g_ctx) != 0) {
        SDL_Log("SDL_GL_MakeCurrent: %s", SDL_GetError());
        return -1;
    }
    if (loaded)
        return 0;
    loaded = 1;
#if SNX_GL_NEEDS_LOADER
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        SDL_Log("gladLoadGL failed");
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

int plat_window_is_open(void) { return g_win != NULL; }

void plat_screen_size(int *w, int *h)
{
#ifdef __SWITCH__
    // SDL на Switch всегда отдаёт 1920x1080; экран в руках — 1280x720
    if (appletGetOperationMode() == AppletOperationMode_Handheld) {
        *w = 1280;
        *h = 720;
    } else {
        *w = 1920;
        *h = 1080;
    }
    SDL_Log("screen %dx%d (%s)", *w, *h, *h == 720 ? "handheld" : "docked");
    return;
#endif
    SDL_DisplayMode m;
    if (SDL_GetDesktopDisplayMode(0, &m) == 0 && m.w > 0 && m.h > 0) {
        *w = m.w;
        *h = m.h;
    } else {
        *w = 1280;
        *h = 720;
    }
    SDL_Log("screen %dx%d", *w, *h);
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
