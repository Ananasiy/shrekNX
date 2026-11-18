#!/bin/sh
# Обёртка компилятора для SNX_PGO=use (cmake/gen.cmake): с выносом холодного (-freorder-blocks-and-partition), а
# файл, где GCC aarch64 разнёс по секциям метки таблицы переходов switch («invalid operands (.text.unlikely.f_X
# and .text.f_X sections)»), — заново без выноса.
"$@" 2>/dev/null || exec "$@" -fno-reorder-blocks-and-partition
