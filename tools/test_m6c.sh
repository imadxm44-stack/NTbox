#!/usr/bin/env bash
# M6c test: user-mode PE loader + CS/TLS/VirtualProtect + user32/gdi32 stubs
# CONTRACT: exit 0 = PASS, 1 = FAIL
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T_NAME=M6c
T_LOG="$ROOT/docs/m6c_test.log"
. "$ROOT/tools/testlib.sh"

t_head

NTBOXD="$ROOT/tools/ntboxd/ntboxd"
APP="$ROOT/tools/build_dubltest/m6c_test.exe"

t_step "build ntboxd"
if [ ! -x "$NTBOXD" ]; then
    gcc -O2 -Wall -o "$NTBOXD" "$ROOT/tools/ntboxd/ntboxd.c" -lpthread || true
fi
[ -x "$NTBOXD" ] && t_ok "ntboxd جاهز" || t_fail "ntboxd لم يُبنى"

t_step "build m6c_test.exe"
if [ ! -f "$APP" ] || [ "$ROOT/tools/build_dubltest/m6c_test.c" -nt "$APP" ]; then
    x86_64-w64-mingw32-gcc -O1 -nostdlib -o "$APP" \
        "$ROOT/tools/build_dubltest/m6c_test.c" -lkernel32 -ladvapi32 || true
fi
[ -f "$APP" ] && t_ok "m6c_test.exe جاهز" || t_fail "m6c_test.exe لم يُبنى"

t_step "run m6c_test.exe"
RC=0
"$NTBOXD" "$APP" || RC=$?
echo "ntboxd exit=$RC"
[ "$RC" -eq 0 ] && t_ok "التطبيق انتهى برمز 0" || t_fail "التطبيق انتهى برمز $RC"

t_step "expected markers"
LOGOUT="$ROOT/docs/m6c_test.log"
for marker in "file write OK" "thread OK" "vmem ok" "GetProcAddress OK"; do
    if grep -q "$marker" "$LOGOUT" 2>/dev/null; then
        t_ok "وُجد: $marker"
    else
        t_fail "مفقود: $marker"
    fi
done

t_finish
