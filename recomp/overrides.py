"""Нативные замены гостевых функций: "модуль!экспорт" -> (C-символ, глубина x87 на выходе).

Реализации — runtime/overrides_*.c. Вызовы и dispatch по адресу экспорта идут в замену.
Обёртки (WRAPS) — такие же замены, но оригинал остаётся доступен замене как `ovr_orig_<символ>`.
Неэкспортированная функция — "модуль!=имя" с байтами её начала в SIGS (? — любой байт): адрес — единственное
совпадение в коде модуля (английская и русская версии DLL — разные сборки).
"""
import re

OVERRIDES = {}
WRAPS = {}
SIGS = {}

# Core: TFieldIterator<UProperty>::IterateToNext — пропуск не-свойств (функций, состояний, перечислений) в цепочке
# полей класса, кеш (runtime/overrides_core.c)
SIGS["field_next"] = "56 8B F1 83 3E 00 74 ? 57 BF 00 80 00 00 8B FF 8B 46 04 85 C0 74 ? EB"
OVERRIDES["Core.dll!=field_next"] = ("ovr_core_field_next", 0)
WRAPS["Core.dll!?CollectGarbage@UObject@@SAXK@Z"] = ("ovr_core_collect_garbage", 0)
WRAPS["Core.dll!?Link@UStruct@@UAEXAAVFArchive@@H@Z"] = ("ovr_core_struct_link", 0)
# memcpy из статического CRT — своя копия у каждого модуля, хвосты — переходами по вычисленному адресу
SIGS["memcpy"] = "55 8B EC 53 56 57 A1 ? ? ? ? 83 38 00 0F 84 ? ? ? ? 8B 4D 10 8B 7D 08 8B 75 0C 8B D9 FC"
for _m in ["Engine.dll", "D3DDrv.dll", "ALAudio.dll", "Fire.dll", "GUI.dll", "IpDrv.dll", "KWGame.dll", "Window.dll",
           "game.exe"]:
    OVERRIDES[f"{_m}!=memcpy"] = ("ovr_memcpy", 0)
# game.exe: FArchiveFileWriter::Serialize — копия в буфер архива без вызовов гостя
SIGS["file_write"] = "53 8B 5C 24 0C 55 8B 6C 24 0C 56 8B F1 8B 56 48 8B 4E 4C 57 BF 00 10 00 00"
WRAPS["game.exe!=file_write"] = ("ovr_launch_file_write", 0)

# Engine: Hor+ для широкого экрана (runtime/overrides_engine.c)
WRAPS["Engine.dll!?UpdateMatrices@FCameraSceneNode@@UAEXXZ"] = ("ovr_engine_camera_matrices", 0)
# Engine: без сброса ресурсов рендера сразу после сохранения (runtime/overrides_engine.c)
WRAPS["Engine.dll!?SaveGame@UGameEngine@@UAEXH@Z"] = ("ovr_engine_save_game", 0)
WRAPS["Engine.dll!?Flush@UEngine@@UAEXH@Z"] = ("ovr_engine_flush", 0)

# msvcr70: математика (внутри — x87-трюки с нестандартной передачей стека, статически не переводится)
for _n in ["acos", "asin", "atan", "atan2", "ceil", "cos", "cosh", "exp", "fabs", "floor", "fmod", "frexp",
           "ldexp", "log", "log10", "modf", "pow", "sin", "sinh", "sqrt", "tan", "tanh", "_logb", "_nextafter",
           "_y0", "_y1", "_yn", "_wtof", "wcstod", "atof", "strtod", "_atodbl"]:
    OVERRIDES[f"msvcr70.dll!{_n}"] = (f"ovr_crt_{_n.lstrip('_')}", 0 if _n == "_atodbl" else 1)
# msvcr70: rand/srand без _getptd (три вызова HLE на каждое число; runtime/overrides_crt.c)
OVERRIDES["msvcr70.dll!rand"] = ("ovr_crt_rand", 0)
OVERRIDES["msvcr70.dll!srand"] = ("ovr_crt_srand", 0)


def _find_sig(img, sig):
    rx = re.compile(b"".join(b"." if p == "?" else re.escape(bytes([int(p, 16)])) for p in sig.split()), re.S)
    hits = []
    for s in img.code_sections():
        o = s.va - img.base
        hits += [s.va + m.start() for m in rx.finditer(bytes(img.data[o:o + s.vsize]))]
    if len(hits) != 1:
        raise SystemExit(f"overrides: signature {sig!r} in {img.name}: {len(hits)} matches")
    return hits[0]


def _addr(prog, key):
    mod, name = key.split("!")
    img = prog.images.get(mod.lower())
    if img is None:
        return None
    if name.startswith("="):
        return _find_sig(img, SIGS[name[1:]])
    return img.exports.get(name)


def resolve(prog):
    """-> {guest addr: (sym, ret_depth)} для загруженных модулей (замены и обёртки)."""
    out = {}
    for key, val in list(OVERRIDES.items()) + list(WRAPS.items()):
        a = _addr(prog, key)
        if a is not None:
            out[a] = val
    return out


def wrapped(prog):
    """-> {guest addr: sym} обёрток: таблица модуля определяет для них ovr_orig_<sym> = f_<addr>."""
    out = {}
    for key, (sym, _) in WRAPS.items():
        a = _addr(prog, key)
        if a is not None:
            out[a] = sym
    return out
