#!/usr/bin/env bash
# M8 test: GPU D3D/DXGI translation path inside ntboxd
# ⚠️诚实: M8 اليوم = تصميم + stubs، وليس zero-copy حقيقي.
#    المسار الحقيقي (GBM/dmabuf ⇒ Wayland) = الخطة W9a (T-201…T-223).
# CONTRACT: exit 0 = PASS, 1 = FAIL, 2 = SKIP (no Wayland compositor)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T_NAME=M8
T_LOG="$ROOT/docs/m8_test.log"
. "$ROOT/tools/testlib.sh"

t_head

NTBOXD="$ROOT/tools/ntboxd/ntboxd"
APP="$ROOT/tools/build_dubltest/m8_d3d_test.exe"

t_step "build ntboxd (wayland)"
if [ ! -x "$NTBOXD" ]; then
    gcc -O2 -Wall -o "$NTBOXD" "$ROOT/tools/ntboxd/ntboxd.c" \
        "$ROOT/tools/ntboxwayland/xdg-shell-protocol.c" \
        -lpthread -lwayland-client -I"$ROOT/tools/ntboxwayland" || true
fi
[ -x "$NTBOXD" ] && t_ok "ntboxd جاهز" || t_fail "ntboxd لم يُبنى"

t_step "build m8_d3d_test.exe"
if [ ! -f "$APP" ] || [ "$ROOT/tools/build_dubltest/m8_d3d_test.c" -nt "$APP" ]; then
    x86_64-w64-mingw32-gcc -O1 -nostdlib -o "$APP" \
        "$ROOT/tools/build_dubltest/m8_d3d_test.c" -lkernel32 -ldxgi -ld3d11 -luser32 -lgdi32 -ladvapi32 || true
fi
[ -f "$APP" ] && t_ok "m8_d3d_test.exe جاهز" || t_fail "لم يُبنى"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
WESTON_PID=""
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    if command -v weston >/dev/null 2>&1; then
        rm -f "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null || true
        weston --backend=x11-backend.so >"$ROOT/docs/weston_m8.log" 2>&1 &
        WESTON_PID=$!
        sleep 4
        for s in "$XDG_RUNTIME_DIR"/wayland-*; do
            [ -S "$s" ] && export WAYLAND_DISPLAY=$(basename "$s")
        done
    fi
fi
echo "WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-none}"

if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    t_skip "لا compositor Wayland ⇒ المسار البصري غير مُتحقَّق"
    [ -n "$WESTON_PID" ] && kill "$WESTON_PID" 2>/dev/null
    t_finish; exit 2
fi
t_ok "compositor: $WAYLAND_DISPLAY"

t_step "run m8_d3d_test.exe"
RC=0
"$NTBOXD" "$APP" || RC=$?
echo "ntboxd exit=$RC"
[ "$RC" -eq 0 ] && t_ok "التطبيق انتهى برمز 0" || t_fail "التطبيق انتهى برمز $RC"

t_step "GPU: صفر نسخ CPU (المطلوب GFX-007)"
if grep -qiE 'zero.?copy|dmabuf|GBM' "$ROOT/docs/m8_test.log" 2>/dev/null; then
    t_ok "المسار يذكر dmabuf/GBM"
else
    t_fail "المسار ما زال نسخ CPU (memcpy) — التنفيذ في W9a (T-201…T-223)"
fi

[ -n "$WESTON_PID" ] && kill "$WESTON_PID" 2>/dev/null
t_finish
