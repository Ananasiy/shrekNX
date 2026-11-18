// Слой интерфейса порта поверх кадра игры: текст (FreeType; на Switch — системный шрифт) и значки кнопок
// геймпада Switch. Список прямоугольников строит поток гостя при показе кадра (d3d8_present), рисует поток
// рендера прямо в задний буфер перед выводом на экран — координаты в пикселях заднего буфера, начало вверху
// слева (как у Bounds элементов меню игры).
#pragma once
#include "runtime/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OV_A, OV_B, OV_X, OV_Y, OV_L, OV_R, OV_ZL, OV_ZR, OV_PLUS, OV_MINUS, OV_LSTICK, OV_RSTICK,
    OV_DPAD, OV_DPAD_UP, OV_DPAD_DOWN, OV_DPAD_LEFT, OV_DPAD_RIGHT, OV_BUTTON_N
} ov_button;

void ov_begin(void);                                                   // новый список (поток гостя)
float ov_text(float x, float y, float h, u32 rgba, const char *s);     // y — верх строки, h — кегль; -> ширина
float ov_text_width(float h, const char *s);
float ov_icon(float x, float y, float h, ov_button b);                 // значок высотой h; -> ширина
float ov_icon_width(float h, ov_button b);
void ov_rect(float x, float y, float w, float h, u32 rgba);            // залитый прямоугольник
// Нарисовать список в FBO fbo размером w x h (поток гостя ставит в очередь рендера; пустой — ничего).
void ov_flush(u32 fbo, int w, int h);

// rgba: 0xAABBGGRR (байты R, G, B, A по возрастанию адреса)
#define OV_RGBA(r, g, b, a) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16 | (u32)(a) << 24)

#ifdef __cplusplus
}
#endif
