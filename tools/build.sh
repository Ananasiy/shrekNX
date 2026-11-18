#!/bin/sh
# Сборка: tools/build.sh mac|switch|mac-rus|switch-rus|switch-pgo [цели cmake...]
# -rus — русская версия (SNX_VARIANT=rus: код из build/gen-rus, каталог сборки build/<платформа>-rus).
# switch-pgo — Switch со счётчиками профиля (build/switch-pgo, SNX_PGO=gen). Switch собирается по профилю из
# build/pgo, если он есть (AGENTS.md, «PGO»; SNX_PGO= — без него): новый профиль (tools/pgo_fetch.py) — с нуля.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PLAT=${1:-mac}
[ $# -gt 0 ] && shift
case "$PLAT" in
  *-rus) VARIANT=rus ;;
  *)     VARIANT= ;;
esac
PGO=${SNX_PGO-auto}
if [ "$PLAT" = switch-pgo ]; then
  PGO=gen
elif [ "$PGO" = auto ]; then
  PGO=
  case "$PLAT" in switch*) [ -f "$ROOT/build/pgo/stamp" ] && PGO=use ;; esac
fi
CLEAN=
case "$PLAT" in
  mac|mac-rus)       cmake -S "$ROOT" -B "$ROOT/build/$PLAT" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
                           -DSNX_VARIANT="$VARIANT" >/dev/null ;;
  switch|switch-rus|switch-pgo)
                     : "${DEVKITPRO:=/opt/devkitpro}"; export DEVKITPRO; PATH="$DEVKITPRO/tools/bin:$PATH"; export PATH
                     cmake -S "$ROOT" -B "$ROOT/build/$PLAT" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
                           -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" -DSNX_VARIANT="$VARIANT" \
                           -DSNX_PGO="$PGO" >/dev/null
                     # профиль не зависимость объектников: другой профиль (или его смена с выключенным) — с нуля
                     STAMP="$ROOT/build/$PLAT/pgo.stamp"
                     WANT="$PGO"
                     [ "$PGO" = use ] && WANT="use $(cat "$ROOT/build/pgo/stamp" 2>/dev/null || echo none)"
                     if [ "$(cat "$STAMP" 2>/dev/null || true)" != "$WANT" ]; then
                       CLEAN=--clean-first
                     fi ;;
  *) echo "usage: $0 mac|switch|mac-rus|switch-rus|switch-pgo [targets]" >&2; exit 2 ;;
esac
if [ $# -gt 0 ]; then
  cmake --build "$ROOT/build/$PLAT" $CLEAN --target "$@"
else
  cmake --build "$ROOT/build/$PLAT" $CLEAN
fi
case "$PLAT" in
  switch*) printf '%s\n' "$WANT" > "$STAMP" ;;
esac
