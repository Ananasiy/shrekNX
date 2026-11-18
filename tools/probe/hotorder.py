"""Порядок горячих функций для компоновщика по профилю SNX_PROF (мак или Switch): самые частые в выборках (лист)
функции идут подряд в начале .text — плотнее в кеше инструкций и TLB (A57: L1i 48 КБ, машинного кода ≈18 МБ, профиль
плоский). Вывод — фрагмент для `ld --section-ordering-file` (GNU ld ≥ 2.43; `-ffunction-sections` уже включён):

    tools/.venv/bin/python -I tools/probe/hotorder.py --bin build/mac/host/boot/snx_boot prof.txt [prof2.txt] \
        --out build/hot_order.ld
    tools/build.sh switch   # с -DSNX_SECTION_ORDER=build/hot_order.ld (см. CMakeLists.txt)

Имена функций — как в C (`f_<адрес>`, рантайм, HLE), поэтому профиль с мака годится для сборки Switch.
"""
import argparse
import bisect
import collections
import os
import re
import subprocess

NM_ELF = os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "devkitA64", "bin", "aarch64-none-elf-nm")


def symbols(path, slide):
    elf = open(path, "rb").read(4) == b"\x7fELF"
    out = subprocess.run([NM_ELF if elf else "nm", "-n", "--defined-only", path], capture_output=True,
                         text=True).stdout
    syms = []
    for ln in out.split("\n"):
        p = ln.split()
        if len(p) == 3 and p[1] in "tTwW":
            name = p[2] if elf else p[2][1:]  # Mach-O: ведущее подчёркивание
            syms.append((int(p[0], 16) + slide, name))
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="бинарник, на котором снят профиль (для имён)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--threads", default="gr", help="чьи выборки считать: g — гость, r — рендер")
    ap.add_argument("--cover", type=float, default=99.5, help="процент выборок, который должны покрыть функции")
    ap.add_argument("profs", nargs="+")
    args = ap.parse_args()
    cnt = collections.Counter()
    total = 0
    for prof in args.profs:
        lines = open(prof).read().split("\n")
        m = re.match(r"base ([0-9a-f]+) slide ([0-9a-f]+)", lines[0])
        syms = symbols(args.bin, int(m.group(2), 16))
        starts = [a for a, _ in syms]
        for ln in lines[1:]:
            p = ln.split()
            if len(p) < 2 or p[0] not in args.threads:
                continue
            pc = int(p[1], 16)
            i = bisect.bisect_right(starts, pc) - 1
            if i >= 0 and pc - starts[i] < (1 << 20):  # дальше — системные библиотеки (мак), не наш образ
                cnt[syms[i][1]] += 1
                total += 1
    order, acc = [], 0
    for name, n in cnt.most_common():
        if 100 * acc / total >= args.cover:
            break
        order.append(name)
        acc += n
    with open(args.out, "w") as f:
        # фрагмент встаёт в начало .text раньше скрипта платформы: crt0 NRO обязан остаться по смещению 0 (там точка
        # входа и место под заголовок, который пишет elf2nro), иначе hbloader прыгает в чужую функцию
        f.write(".text : {\n  KEEP (*(.crt0))\n")
        for name in order:  # .text.hot.<имя> — та же функция, собранная по профилю (SNX_PGO=use)
            f.write(f"  *(.text.{name} .text.hot.{name})\n")
        f.write("}\n")
    guest = sum(1 for n in order if re.match(r"f[ic]?_[0-9a-f]{8}$", n))
    print(f"{total} выборок в образе, {len(cnt)} функций; {len(order)} покрывают {100 * acc / total:.1f}% "
          f"(сгенерированных {guest}) -> {args.out}")


if __name__ == "__main__":
    main()
