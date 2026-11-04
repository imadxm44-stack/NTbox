#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# testlib.sh — عقد اختبار مشترك لكل سكربتات tools/test_m*.sh
#
# لماذا هذا الملف:
#   سكربتات test_m*.sh القديمة كانت:
#     - `set -u` فقط ⇒ فشل وسيط لا يوقف السكربت
#     - رمز الخروج = آخر أمر ⇒ فشل حقيقي ⇒ exit 0  (اكتذاب!)
#     - مسار `/tmp/opencode/scratchpad/Windows/Windows/...` مكرَّر ومكسور
#   الآن: كل خطوة تُسجَّل، و"نتيجة" صريحة، ورمز خروج حقيقي.
#
# العقد:
#   t_step  "<وصف>"      ⇒ بداية خطوة
#   t_ok    "<تفصيل>"    ⇒ خطوة نجحت
#   t_fail  "<سبب>"      ⇒ خطوة فشلت (تُحتسب)
#   t_skip  "<سبب>"      ⇒ خطوة لم تُنفَّذ (لا تُحتسب نجاحاً، وتُعلن)
#   t_finish              ⇒ يطبع الملخّص ويُعيد 0/1 حسب t_fails
#
# المتغيّرات:
#   T_NAME   اسم الاختبار (مثال M3) — يُستخدم في السجل
#   T_LOG    مسار ملف السجل
# ─────────────────────────────────────────────────────────────────────────────

set -uo pipefail

T_NAME="${T_NAME:-TEST}"
T_LOG="${T_LOG:-$ROOT/docs/${T_NAME,,}_test.log}"
T_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="${ROOT:-$T_ROOT}"
NTBOX_ROOT="${NTBOX_ROOT:-/tmp/opencode/win7root}"

t_pass=0
t_fails=0
t_skips=0

t_head() {
    local dir; dir="$(dirname "$T_LOG")"
    mkdir -p "$dir" 2>/dev/null || true
    # سجلات قديمة بملكية root ⇒ لا نقدر نكتب فوقها كمستخدم عادي
    if [ ! -w "$T_LOG" ] && [ -e "$T_LOG" ]; then
        local alt="$dir/${T_NAME,,}_test.$$.log"
        echo "  [WARN] $T_LOG غير قابل للكتابة ⇒ السجل في $alt"
        T_LOG="$alt"
    fi
    : > "$T_LOG" 2>/dev/null || true
    exec > >(tee -a "$T_LOG") 2>&1
    echo "=== $T_NAME test $(date) kernel=$(uname -r) host=$(hostname) ==="
}

t_step() { echo "--- $1 ---"; }
t_ok()   { t_pass=$((t_pass+1)); echo "  [OK]   ${1:-}"; }
t_fail() { t_fails=$((t_fails+1)); echo "  [FAIL] ${1:-}"; }
t_skip() { t_skips=$((t_skips+1)); echo "  [SKIP] ${1:-} (لا يُحسب نجاحاً)"; }

# t_run <label> <cmd...>  ⇒ ينفّذ، ويسجّل نجاح/فشل مع رمز الخروج
t_run() {
    local label="$1"; shift
    local rc=0
    "$@" || rc=$?
    if [ "$rc" -eq 0 ]; then
        t_ok "$label"
    else
        t_fail "$label (exit=$rc)"
    fi
    return $rc
}

# t_run_sh <label> <shell-string>
t_run_sh() {
    local label="$1"; shift
    local rc=0
    bash -c "$*" || rc=$?
    if [ "$rc" -eq 0 ]; then
        t_ok "$label"
    else
        t_fail "$label (exit=$rc)"
    fi
    return $rc
}

t_finish() {
    local verdict rc
    if [ "$t_fails" -gt 0 ]; then
        verdict=FAIL; rc=1
    elif [ "$t_pass" -eq 0 ] && [ "$t_skips" -gt 0 ]; then
        verdict=SKIP; rc=2
    else
        verdict=PASS; rc=0
    fi
    echo
    echo "=== $T_NAME SUMMARY: $t_pass passed / $t_fails failed / $t_skips skipped ==="
    echo "=== $T_NAME VERDICT: $verdict ==="
    return $rc
}

# ── مساعدات مشتركة ─────────────────────────────────────────────────────────

# t_need_root  ⇒ يطلب root ويخرج 2 (SKIP) إن لم يكن متوفراً
t_need_root() {
    if [ "$(id -u)" = "0" ]; then
        return 0
    fi
    if sudo -n true 2>/dev/null; then
        SUDO=(sudo -n)
        return 0
    fi
    if [ -n "${NTBOX_SUDO_PASS:-}" ]; then
        SUDO=(sudo -S)
        return 0
    fi
    echo "  [SKIP] لا صلاحيات root (sudo -n فشل و NTBOX_SUDO_PASS غير مضبوط)"
    return 2
}

# t_sudo <cmd...>  ⇒ ينفّذ بصلاحية root إن أمكن
t_sudo() {
    if [ "$(id -u)" = "0" ]; then
        "$@"
    elif [ "${SUDO_MODE:-none}" = "nopass" ]; then
        sudo -n "$@"
    elif [ "${SUDO_MODE:-pass}" ]; then
        printf '%s\n' "$NTBOX_SUDO_PASS" | sudo -S "$@"
    else
        return 1
    fi
}

t_setup_sudo() {
    if [ "$(id -u)" = "0" ]; then SUDO_MODE=root
    elif sudo -n true 2>/dev/null; then SUDO_MODE=nopass
    elif [ -n "${NTBOX_SUDO_PASS:-}" ]; then SUDO_MODE=pass
    else SUDO_MODE=none
    fi
}

# t_build_tool <out> <src> [flags...]  ⇒ يبني أداة عند غيابها
t_build_tool() {
    local out="$1" src="$2"; shift 2
    if [ -x "$out" ]; then return 0; fi
    gcc -O2 -Wall -o "$out" "$src" "$@" >/dev/null 2>&1
}
