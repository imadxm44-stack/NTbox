#!/usr/bin/env bash
# M7 test: Wayland window from user32/gdi32 inside ntboxd (M6c + M7 bootstrap)
# CONTRACT: exit 0 = PASS, 1 = FAIL, 2 = SKIP (no Wayland compositor)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T_NAME=M7
T_LOG="$ROOT/docs/m7_test.log"
. "$ROOT/tools/testlib.sh"

t_head

NTBOXD="$ROOT/tools/ntboxd/ntboxd"
APP="$ROOT/tools/build_dubltest/m7_window_test.exe"

t_step "build ntboxd (wayland)"
if [ ! -x "$NTBOXD" ]; then
    gcc -O2 -Wall -o "$NTBOXD" "$ROOT/tools/ntboxd/ntboxd.c" \
        "$ROOT/tools/ntboxwayland/xdg-shell-protocol.c" \
        -lpthread -lwayland-client -I"$ROOT/tools/ntboxwayland" || true
fi
[ -x "$NTBOXD" ] && t_ok "ntboxd جاهز" || t_fail "ntboxd لم يُبنى"

t_step "build m7_window_test.exe"
if [ ! -f "$APP" ] || [ "$ROOT/tools/build_dubltest/m7_window_test.c" -nt "$APP" ]; then
    x86_64-w64-mingw32-gcc -O1 -nostdlib -o "$APP" \
        "$ROOT/tools/build_dubltest/m7_window_test.c" -luser32 -lgdi32 -lkernel32 -ladvapi32 || true
fi
[ -f "$APP" ] && t_ok "m7_window_test.exe جاهز" || t_fail "لم يُبنى"

# ── compositor ───────────────────────────────────────────────────────────
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
WESTON_PID=""
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    if command -v weston >/dev/null 2>&1; then
        rm -f "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null || true
        weston --backend=x11-backend.so >"$ROOT/docs/weston_m7.log" 2>&1 &
        WESTON_PID=$!
        sleep 4
        for s in "$XDG_RUNTIME_DIR"/wayland-*; do
            [ -S "$s" ] && export WAYLAND_DISPLAY=$(basename "$s")
        done
    fi
fi
echo "WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-none}"

if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    t_skip "لا compositor Wayland ⇒ المسار البصري غير مُتحقَّق (لا يُحسب نجاحاً)"
    [ -n "$WESTON_PID" ] && kill "$WESTON_PID" 2>/dev/null
    t_finish; exit 2
fi
t_ok "compositor: $WAYLAND_DISPLAY"

t_step "run m7_window_test.exe"
RC=0
"$NTBOXD" "$APP" || RC=$?
echo "ntboxd exit=$RC"
[ "$RC" -eq 0 ] && t_ok "التطبيق انتهى برمز 0" || t_fail "التطبيق انتهى برمز $RC"

[ -n "$WESTON_PID" ] && kill "$WESTON_PID" 2>/dev/null
t_finish
