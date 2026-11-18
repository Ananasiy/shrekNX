// Нативные замены функций msvcr70 (таблица — recomp/overrides.py). Математика CRT передаёт x87-стек
// нестандартно и статически не переводится; разбор чисел проще и надёжнее сделать на хосте.
// cdecl: [esp] — адрес возврата, аргументы с esp+4; double — в c->st_ret (ST0), int — в eax.
// Ограничение: errno гостя не выставляется.
#include <float.h>
#include <stdio.h>
#include <stdlib.h>

#include "runtime/crt_math.h"

#define ARG(i) (c->esp + 4 + 4 * (u32)(i)) // гостевой адрес i-го 4-байтового слова аргументов
#define F64(i) rdf64(g_mem, ARG(i))
#define U32(i) rd32(g_mem, ARG(i))

static void ret_f64(CPU *c, double v)
{
    c->st_ret = v;
    c->esp += 4;
}

// --- математика ------------------------------------------------------------------
#define UNARY(name, expr)                                                                                   \
    void ovr_crt_##name(CPU *c)                                                                             \
    {                                                                                                       \
        double x = F64(0);                                                                                  \
        ret_f64(c, crt_nan(expr, x, 0.0));                                                                  \
    }
#define BINARY(name, expr)                                                                                  \
    void ovr_crt_##name(CPU *c)                                                                             \
    {                                                                                                       \
        double x = F64(0), y = F64(2);                                                                      \
        ret_f64(c, crt_nan(expr, x, y));                                                                    \
    }

UNARY(acos, acos(x))
UNARY(asin, asin(x))
UNARY(atan, atan(x))
UNARY(ceil, ceil(x))
UNARY(cos, cos(x))
UNARY(cosh, cosh(x))
UNARY(exp, crt_exp(x))
UNARY(fabs, fabs(x))
UNARY(floor, floor(x))
UNARY(log, log(x))
UNARY(log10, log10(x))
UNARY(sin, sin(x))
UNARY(sinh, sinh(x))
UNARY(sqrt, sqrt(x))
UNARY(tan, tan(x))
UNARY(tanh, tanh(x))
UNARY(logb, isinf(x) ? x : logb(x)) // как в MSVC: _logb(-inf) = -inf
UNARY(y0, y0(x))
UNARY(y1, y1(x))
BINARY(atan2, crt_atan2(x, y)) // atan2(y, x): первый аргумент — y
BINARY(fmod, crt_fmod(x, y))
BINARY(pow, crt_pow(x, y))
BINARY(nextafter, nextafter(x, y))

void ovr_crt_frexp(CPU *c) // double frexp(double, int *)
{
    double x = F64(0);
    int e = -1;
    double r = crt_nan(NAN, x, 0.0); // MSVC: frexp(±inf/nan) — NaN и показатель -1
    if (x == 0.0) {
        e = 0;
        r = 0.0; // MSVC: frexp(-0) = +0
    } else if (isfinite(x))
        r = frexp(x, &e);
    wr32(g_mem, U32(2), (u32)e);
    ret_f64(c, crt_nan(r, x, 0.0));
}

void ovr_crt_ldexp(CPU *c) // double ldexp(double, int)
{
    double x = F64(0);
    ret_f64(c, crt_nan(ldexp(x, (int)(s32)U32(2)), x, 0.0));
}

void ovr_crt_modf(CPU *c) // double modf(double, double *)
{
    double x = F64(0), ip = 0;
    double r = modf(x, &ip);
    wrf64(g_mem, U32(2), crt_nan(ip, x, 0.0));
    ret_f64(c, crt_nan(r, x, 0.0));
}

void ovr_crt_yn(CPU *c) // double _yn(int, double)
{
    double x = F64(1);
    ret_f64(c, crt_nan(yn((int)(s32)U32(0), x), x, 0.0));
}

// --- разбор чисел ----------------------------------------------------------------
// Символ гостевой строки: узкой (байт) или широкой (UTF-16).
static u32 gch(u32 s, int wide, u32 i) { return wide ? rd16(g_mem, s + 2 * i) : g_mem[s + i]; }
static int is_ws(u32 ch) { return ch == ' ' || (ch >= 9 && ch <= 13); }
static int is_dig(u32 ch) { return ch >= '0' && ch <= '9'; }

#define MAX_DIGITS 21 // CRT MSVC учитывает не больше 21 значащей цифры, остальные отбрасывает

typedef struct {
    u32 len;   // длина принятого текста с начальными пробелами (0 — числа нет)
    int over;  // переполнение double (v = ±inf)
    int under; // антипереполнение double: денормал или 0 из ненулевых цифр
    double v;  // значение, которое CRT получает до проверки флагов
} num_t;

// Число по грамматике CRT MSVC 7: [пробелы][знак]цифры[.цифры][(e|E|d|D)[знак]цифры]; без inf/nan/hex.
// Без цифр len = 0, а v — ноль со знаком (так его отдаёт _atodbl).
static num_t parse_num(u32 s, int wide)
{
    num_t r = {0, 0, 0, 0.0};
    u32 i = 0, ch;
    int neg = 0, n = 0, ndig = 0;
    long exp10 = 0;
    char buf[MAX_DIGITS + 32];
    while (is_ws(gch(s, wide, i)))
        i++;
    if ((ch = gch(s, wide, i)) == '+' || ch == '-') {
        neg = ch == '-';
        i++;
    }
    for (; is_dig(ch = gch(s, wide, i)); i++, ndig++) {
        if (n == 0 && ch == '0')
            continue;
        if (n < MAX_DIGITS)
            buf[n++] = (char)ch;
        else
            exp10++;
    }
    if (ch == '.') {
        for (i++; is_dig(ch = gch(s, wide, i)); i++, ndig++) {
            if (n == 0 && ch == '0')
                exp10--;
            else if (n < MAX_DIGITS) {
                buf[n++] = (char)ch;
                exp10--;
            }
        }
    }
    if (!ndig) {
        r.v = neg ? -0.0 : 0.0;
        return r;
    }
    if (ch == 'e' || ch == 'E' || ch == 'd' || ch == 'D') {
        u32 j = i + 1;
        int eneg = 0;
        if ((ch = gch(s, wide, j)) == '+' || ch == '-') {
            eneg = ch == '-';
            j++;
        }
        if (is_dig(gch(s, wide, j))) {
            long e = 0;
            for (; is_dig(ch = gch(s, wide, j)); j++)
                if (e < 100000)
                    e = e * 10 + (long)(ch - '0');
            exp10 += eneg ? -e : e;
            i = j;
        }
    }
    r.len = i;
    if (n) {
        snprintf(buf + n, 32, "e%ld", exp10);
        r.v = strtod(buf, NULL); // в buf только цифры и экспонента — от локали не зависит
        if (isinf(r.v))
            r.over = 1;
        else if (r.v < DBL_MIN) {
            // ниже минимального денормала long double (3.6e-4951) CRT молча даёт ноль без флага
            buf[n] = 0;
            r.under = log10(strtod(buf, NULL)) + (double)exp10 >= -4950.4383;
        }
    }
    if (neg)
        r.v = -r.v;
    return r;
}

// strtod/wcstod (strict: антипереполнение -> +0; endptr, если не 0) и atof/_wtof (значение как есть).
static double str_to_d(u32 s, u32 endp, int wide, int strict)
{
    num_t r = parse_num(s, wide);
    if (endp)
        wr32(g_mem, endp, s + r.len * (wide ? 2 : 1));
    return !r.len || (strict && r.under) ? 0.0 : r.v;
}

void ovr_crt_strtod(CPU *c) { ret_f64(c, str_to_d(U32(0), U32(1), 0, 1)); }
void ovr_crt_wcstod(CPU *c) { ret_f64(c, str_to_d(U32(0), U32(1), 1, 1)); }
void ovr_crt_atof(CPU *c) { ret_f64(c, str_to_d(U32(0), 0, 0, 0)); }
void ovr_crt_wtof(CPU *c) { ret_f64(c, str_to_d(U32(0), 0, 1, 0)); }

// int _atodbl(_CRT_DOUBLE *, char *): 0 — успех, 1 — переполнение, 2 — антипереполнение.
void ovr_crt_atodbl(CPU *c)
{
    num_t r = parse_num(U32(1), 0);
    wrf64(g_mem, U32(0), r.v);
    c->eax = r.over ? 1 : r.under ? 2 : 0;
    c->esp += 4;
}

// --- rand/srand ------------------------------------------------------------------
// Генератор MSVC (_holdrand · 214013 + 2531011, биты 16–30), состояние своё у потока, начальное — 1. У оригинала
// оно в данных потока CRT, и каждый rand — _getptd с тремя вызовами HLE (GetLastError, TlsGetValue,
// SetLastError): частицы зовут appFrand тысячи раз за кадр — ≈3 % времени гостя на Switch.
static __thread u32 t_holdrand = 1;

void ovr_crt_rand(CPU *c)
{
    t_holdrand = t_holdrand * 214013u + 2531011u;
    c->eax = (t_holdrand >> 16) & 0x7FFFu;
    c->esp += 4;
}

void ovr_crt_srand(CPU *c)
{
    t_holdrand = U32(0);
    c->esp += 4;
}
