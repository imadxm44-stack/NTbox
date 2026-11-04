#!/usr/bin/env bash
# =============================================================================
# DublKernel NT-Box — تثبيت واختبار فوري على أي نواة Linux 6.8
# بيئة مستهدفة: Linux Mint — جهاز i5-3470 + RX 580 8GB + RAM 8GB
# (يتوافق أيضاً مع أي توزيعة apt/dnf/pacman أخرى)
#
# الاستخدام:   sudo ./tools/install_ntbox.sh
# الخيارات:    --force        تجاهل فحص إصدار النواة
#              --skip-sys     عدم بناء dubltest.sys
#              --repeat=N     عدد مرات تكرار الاختبار (الافتراضي 3)
# النتيجة:    PASS = [ntbox] NT-Box Demo Test: PASSED بدون Panic
# =============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

FORCE=0
SKIP_SYS=0
REPEAT=3
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        --skip-sys) SKIP_SYS=1 ;;
        --repeat=*) REPEAT="${arg#*=}" ;;
        *) echo "Unknown option: $arg"; exit 2 ;;
    esac
done

KERNEL="$(uname -r)"
KO="$ROOT/ntbox.ko"
SYS_PATH="$ROOT/tools/build_dubltest/dubltest.sys"
LOG="$ROOT/docs/last_run.log"

say() { echo -e "\n=== $* ==="; }
die() { echo "FAIL: $*"; exit 1; }

# ---------- 0. root ----------
[ "$EUID" -eq 0 ] || die "run as root: sudo $0"

# ---------- 0b. distro + hardware info ----------
say "Environment"
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    echo "Distro: ${NAME:-unknown} (${VERSION_ID:-?})"
else
    echo "Distro: unknown"
fi
echo "CPU: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ //')"
echo "RAM: $(awk '/MemTotal/{printf "%.1f GB\n", $2/1024/1024}' /proc/meminfo)"
if command -v lspci >/dev/null 2>&1; then
    lspci -k 2>/dev/null | grep -A3 -i vga | sed 's/^/VGA: /'
    if lspci -n 2>/dev/null | grep -qi "1002:6fdf\|1002:67df"; then
        echo "GPU: AMD RX 580 detected (Polaris — requires linux-firmware for amdgpu)"
    fi
    if lspci -k 2>/dev/null | grep -A3 -i vga | grep -q "Kernel driver in use"; then
        :
    else
        echo "WARN: no kernel driver bound to VGA (install: linux-firmware)"
    fi
else
    echo "WARN: lspci missing (install pciutils) — GPU check skipped"
fi

# ---------- 1. kernel version ----------
say "Kernel check: $KERNEL"
case "$KERNEL" in
    6.8.*) echo "OK: kernel is 6.8.x" ;;
    *)
        if [ "$FORCE" -eq 1 ]; then
            echo "WARN: not 6.8.x, continuing (--force)"
        else
            echo "Linux Mint 21.x hint: sudo apt install -y linux-generic-hwe-22.04 (then reboot)"
            echo "Linux Mint 22: 6.8 is the default kernel — check uname -r"
            die "kernel $KERNEL is not 6.8.x (use --force to override)"
        fi
        ;;
esac

# ---------- 2. secure boot ----------
if command -v mokutil >/dev/null 2>&1; then
    if mokutil --sb-state 2>/dev/null | grep -qi "enabled"; then
        echo "WARN: Secure Boot is ENABLED — unsigned ntbox.ko will fail to load."
        echo "      Disable it in BIOS or sign the module (out of scope for this plan)."
    fi
fi

# ---------- 3. dependencies ----------
say "Dependency check"
NEED_INSTALL=""
command -v make >/dev/null 2>&1 || NEED_INSTALL+=" make"
command -v gcc  >/dev/null 2>&1 || NEED_INSTALL+=" gcc"
[ -d "/lib/modules/$KERNEL/build" ] || NEED_INSTALL+=" headers"

if [ -n "$NEED_INSTALL" ]; then
    echo "Missing:$NEED_INSTALL — trying to install..."
    if command -v apt-get >/dev/null 2>&1; then
        apt-get update -y
        if [ ! -d "/lib/modules/$KERNEL/build" ]; then
            apt-get install -y "linux-headers-$KERNEL" || apt-get install -y linux-headers-generic || true
        fi
        apt-get install -y build-essential linux-firmware pciutils || true
    elif command -v dnf >/dev/null 2>&1; then
        dnf install -y kernel-devel-"$KERNEL" || dnf install -y kernel-devel
        dnf install -y gcc make
    elif command -v pacman >/dev/null 2>&1; then
        pacman -Sy --noconfirm linux-headers base-devel || pacman -Sy --noconfirm
    else
        die "no supported package manager; install: gcc make linux-headers-$KERNEL"
    fi
fi

[ -d "/lib/modules/$KERNEL/build" ] || die "kernel headers not found: /lib/modules/$KERNEL/build"
command -v gcc >/dev/null 2>&1 || die "gcc not found"
command -v make >/dev/null 2>&1 || die "make not found"

# ---------- 4. build dubltest.sys (Windows ABI test driver) ----------
if [ "$SKIP_SYS" -eq 0 ] && [ ! -f "$SYS_PATH" ]; then
    say "Building dubltest.sys"
    if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
        echo "mingw not found — trying to install..."
        if command -v apt-get >/dev/null 2>&1; then
            apt-get install -y gcc-mingw-w64-x86-64
        elif command -v pacman >/dev/null 2>&1; then
            pacman -Sy --noconfirm mingw-w64-gcc
        elif command -v dnf >/dev/null 2>&1; then
            dnf install -y mingw64-gcc
        fi
    fi
    if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
        DDK_INC="$(dirname "$(command -v x86_64-w64-mingw32-gcc)")/../x86_64-w64-mingw32/include/ddk"
        [ -d "$DDK_INC" ] || DDK_INC=""
        ( cd "$ROOT/tools/build_dubltest" && \
          x86_64-w64-mingw32-gcc -shared -Wl,--entry,DriverEntry -Wl,--subsystem,native \
          ${DDK_INC:+-I"$DDK_INC"} -O0 -o dubltest.sys dubltest.c -lntoskrnl ) \
            || die "dubltest.sys build failed"
        echo "built: $SYS_PATH"
    else
        echo "WARN: mingw missing, dubltest.sys not built."
        echo "      Build it elsewhere and copy to: $SYS_PATH"
    fi
fi

# ---------- 5. unload previous instance ----------
say "Cleanup previous instance"
lsmod | grep -q '^ntbox' && { rmmod ntbox || die "rmmod of old ntbox failed"; }

# ---------- 6. build module ----------
say "Building ntbox.ko"
make -C "/lib/modules/$KERNEL/build" M="$ROOT" clean >/dev/null 2>&1 || true
make -C "/lib/modules/$KERNEL/build" M="$ROOT" modules || die "module build failed"
[ -f "$KO" ] || die "ntbox.ko not produced"
echo "built: $KO"

# ---------- 7. test loop ----------
PASS_RUNS=0
for i in $(seq 1 "$REPEAT"); do
    say "Run #$i/$REPEAT"
    dmesg -C 2>/dev/null || true

    insmod "$KO" || { echo "insmod failed"; dmesg | tail -20; continue; }
    sleep 0.3
    lsmod | grep -q '^ntbox' || { echo "lsmod: ntbox not loaded"; continue; }

    if [ ! -f "$SYS_PATH" ]; then
        echo "WARN: $SYS_PATH missing — test will fail at file load"
    fi
    echo "$SYS_PATH" > /proc/ntbox/load || echo "proc write returned error (see dmesg)"
    sleep 0.3

    dmesg | tee -a "$LOG" | tail -30

    if dmesg | grep -q "NT-Box Demo Test: PASSED"; then
        if dmesg | grep -qiE "kernel panic|oops|bug:"; then
            echo "FAIL: PASSED printed but panic/oops detected"
        else
            echo "RESULT: run #$i = PASS"
            PASS_RUNS=$((PASS_RUNS + 1))
        fi
    else
        echo "RESULT: run #$i = FAIL (PASSED not found)"
    fi

    rmmod ntbox || echo "WARN: rmmod failed"
    sleep 0.3
done

# ---------- 8. final verdict ----------
say "Final verdict"
echo "Passed runs: $PASS_RUNS/$REPEAT"
dmesg | grep -iE "ntbox|panic|oops|bug:|error|warn" | tail -40

if [ "$PASS_RUNS" -eq "$REPEAT" ]; then
    echo "=========================================="
    echo "  NT-BOX TEST: PASSED ($PASS_RUNS/$REPEAT)"
    echo "=========================================="
    exit 0
else
    echo "=========================================="
    echo "  NT-BOX TEST: FAIL ($PASS_RUNS/$REPEAT)"
    echo "=========================================="
    exit 1
fi
