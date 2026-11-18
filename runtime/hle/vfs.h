// Пути гостя (Windows) -> пути хоста. Два корня: каталог игры (только чтение, эталон) и
// пользовательский каталог-оверлей (запись: ini, логи, сохранения). Чтение — сначала оверлей, потом игра;
// сравнение имён без учёта регистра (как в Windows), разделители и «..» нормализуются.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void vfs_init(const char *game_root, const char *user_root, const char *guest_root);

// Нормализованный путь гостя относительно корня игры ("System/Default.ini") или NULL, если путь вне корня.
// in — UTF-8, абсолютный ("C:\\...") или относительный к текущему каталогу гостя.
int vfs_rel(const char *in, char *rel, size_t cap);
// Хостовый путь существующего файла/каталога (оверлей, затем игра). 0 — не найден.
int vfs_find(const char *rel, char *host, size_t cap);
// То же только в каталоге игры (оригинал, даже если в оверлее есть своя копия).
int vfs_find_game(const char *rel, char *host, size_t cap);
// Хостовый путь для записи в оверлее (родительские каталоги создаются). copy_existing — скопировать
// туда файл из каталога игры, если в оверлее его ещё нет (открытие на изменение).
int vfs_for_write(const char *rel, char *host, size_t cap, int copy_existing);
// В оверлее что-то создано/удалено/переименовано: сбросить кеш его каталогов (поиск без учёта регистра
// кеширует листинги). vfs_for_write сбрасывает сам; создавший файл после него зовёт ещё раз.
void vfs_changed(void);

// Текущий каталог гостя (полный путь Windows).
const char *vfs_cwd(void);
int vfs_set_cwd(const char *in);
// Полный путь Windows для in (GetFullPathName).
void vfs_full(const char *in, char *out, size_t cap);

// Объединённый список каталога (оверлей + игра, без повторов). Возвращает число имён; *names — malloc,
// строки подряд через '\0'. -1 — каталога нет.
int vfs_list(const char *rel_dir, char **names);

#ifdef __cplusplus
}
#endif
