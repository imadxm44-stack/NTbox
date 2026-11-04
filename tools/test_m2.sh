#!/usr/bin/env bash
# M2 end-to-end test — partitions + policy + attach + open-in-place + detach
# CONTRACT: exit 0 = PASS, 1 = FAIL, 2 = SKIP (no root)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T_NAME=M2
T_LOG="$ROOT/docs/m2_test.log"
. "$ROOT/tools/testlib.sh"

t_head
t_setup_sudo

KO="$ROOT/ntbox.ko"
NTSHELL="$ROOT/tools/ntshell/ntshell"
DEV="$ROOT"

t_step "build ntshell"
t_build_tool "$NTSHELL" "$ROOT/tools/ntshell/ntshell.c" || true
[ -x "$NTSHELL" ] && t_ok "ntshell جاهز" || t_fail "ntshell لم يُبنى"

if ! t_need_root; then
    t_finish; exit 2
fi

t_step "cleanup"
t_sudo rmmod ntbox 2>/dev/null || true
sleep 0.3

t_step "insmod"
if t_sudo insmod "$KO"; then t_ok "insmod"; else t_fail "insmod"; t_finish; exit 1; fi
sleep 0.5

t_step "partitions"
t_sudo "$NTSHELL" partitions

t_step "policy allow $DEV"
t_sudo "$NTSHELL" policy allow "$DEV"

t_step "attach $DEV C $DEV"
t_sudo "$NTSHELL" attach "$DEV" C "$DEV"

t_step "run dubltest.sys"
t_sudo "$NTSHELL" run 'C:\tools\build_dubltest\dubltest.sys'

t_step "result"
t_sudo "$NTSHELL" result

t_step "selftest summary"
t_sudo cat /proc/ntbox/selftest | tail -4

t_step "FAIL lamps (must be 0)"
NF=$(t_sudo cat /proc/ntbox/selftest | grep -c ' FAIL ' || true)
echo "FAIL lines: $NF"
[ "$NF" -eq 0 ] && t_ok "صفر مصباح FAIL" || t_fail "عدد مصابيح FAIL = $NF"

t_step "detach C"
t_sudo "$NTSHELL" detach C

t_step "rmmod"
if t_sudo rmmod ntbox; then t_ok "rmmod نظيف"; else t_fail "rmmod فشل"; fi

t_finish
