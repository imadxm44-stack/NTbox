#!/usr/bin/env bash
# M3 real-PE test (Windows 7 null.sys) — DriverEntry + IRP CREATE/CLOSE
# CONTRACT: exit 0 = PASS, 1 = FAIL, 2 = SKIP (no root / no null.sys)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T_NAME=M3
T_LOG="$ROOT/docs/m3_test.log"
. "$ROOT/tools/testlib.sh"

t_head
t_setup_sudo

KO="$ROOT/ntbox.ko"
NTSHELL="$ROOT/tools/ntshell/ntshell"
PEIMPORTS="$ROOT/tools/peimports/peimports"
# مسار التعريفات داخل جذر Win7 (قابل للتجاوز بمتغير البيئة)
WINDRV="${NTBOX_WINDRV:-$NTBOX_ROOT/Windows/System32/drivers}"

t_step "build tools"
t_build_tool "$NTSHELL" "$ROOT/tools/ntshell/ntshell.c" || true
t_build_tool "$PEIMPORTS" "$ROOT/tools/peimports/peimports.c" || true

t_step "locate null.sys ($WINDRV)"
if [ -f "$WINDRV/null.sys" ]; then
    t_ok "null.sys موجود"
else
    t_skip "null.sys غير موجود — استخراجه من ISO مهمة الخطة T-015"
    echo "          (المتوقع: 7z x 'win7iso/Windows 7 Professional.iso' sources/install.wim)"
    t_finish; exit 2
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

t_step "policy allow $WINDRV"
t_sudo "$NTSHELL" policy allow "$WINDRV"

t_step "attach $WINDRV W $WINDRV"
t_sudo "$NTSHELL" attach "$WINDRV" W "$WINDRV"

t_step "imports of null.sys"
t_sudo "$PEIMPORTS" "$WINDRV/null.sys"

t_step "run W:\\null.sys"
t_sudo "$NTSHELL" run 'W:\null.sys'

t_step "result"
t_sudo "$NTSHELL" result

t_step "dmesg tail"
t_sudo dmesg | grep '\[ntbox]' | tail -40

t_step "detach W"
t_sudo "$NTSHELL" detach W

t_step "rmmod"
if t_sudo rmmod ntbox; then t_ok "rmmod نظيف"; else t_fail "rmmod فشل"; fi

t_finish
