// Ввод хоста (SDL) -> гость; вызывается в главном потоке для каждого события (plat_on_event).
#pragma once

void input_event(const void *sdl_event);
// главный поток, до цикла событий: настройки порта и раскладка геймпада <user_dir>/System/Snx.ini (нет файла —
// пишется по умолчанию)
void input_init(const char *user_dir);
// главный поток, каждый проход цикла: стики геймпада -> клавиши и мышь
void input_tick(void);
