#!/usr/bin/env bash
# M4 test: Tier-2/3 table + import classification (zero FAIL)
# CONTRACT: exit 0 = PASS, 1 = FAIL, 2 = SKIP (no root / no null.sys)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T_NAME=M4
T_LOG="$ROOT/docs/m4_test.log"
. "$ROOT/tools/testlib.sh"

t_head
t_setup_sudo

KO="$ROOT/ntbox.ko"
NTSHELL="$ROOT/tools/ntshell/ntshell"
PEIMPORTS="$ROOT/tools/peimports/peimports"
# ⚠️ الإصلاح: كان "/tmp/opencode/scratchpad/Windows/Windows/System32/drivers" (مكرَّر + غير موجود)
WINDRV="${NTBOX_WINDRV:-$NTBOX_ROOT/Windows/System32/drivers}"

t_step "build tools"
t_build_tool "$NTSHELL" "$ROOT/tools/ntshell/ntshell.c" || true
t_build_tool "$PEIMPORTS" "$ROOT/tools/peimports/peimports.c" || true

t_step "locate null.sys ($WINDRV)"
if [ -f "$WINDRV/null.sys" ]; then
    t_ok "null.sys موجود"
else
    t_skip "null.sys غير موجود — استخراجه من ISO مهمة الخطة T-015"
    t_finish; exit 2
fi

t_step "imports of null.sys"
t_sudo "$PEIMPORTS" "$WINDRV/null.sys"

t_step "classify null.sys"
if [ -x "$ROOT/tools/classify_imports.sh" ]; then
    t_sudo "$ROOT/tools/classify_imports.sh" "$WINDRV/null.sys"
else
    t_skip "classify_imports.sh غير موجود"
fi

if ! t_need_root; then
    t_finish; exit 2
fi

t_step "cleanup"
t_sudo rmmod ntbox 2>/dev/null || true
sleep 0.3

t_step "insmod"
if t_sudo insmod "$KO"; then t_ok "insmod"; else t_fail "insmod"; t_finish; exit 1; fi
sleep 0.5

t_step "selftest summary"
ST=$(t_sudo cat /proc/ntbox/selftest)
printf '%s\n' "$ST" | grep -E "SUMMARY:" || printf '%s\n' "$ST" | tail -5

t_step "FAIL count (must be 0)"
NF=$(printf '%s\n' "$ST" | grep -c ' FAIL ' || true)
echo "FAIL lines: $NF"
[ "$NF" -eq 0 ] && t_ok "صفر مصباح FAIL" || t_fail "عدد مصابيح FAIL = $NF"

t_step "rmmod"
if t_sudo rmmod ntbox; then t_ok "rmmod نظيف"; else t_fail "rmmod فشل"; fi

t_finish
