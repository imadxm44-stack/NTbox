#!/bin/sh
# prepare_win7.sh — إعادة بناء بيئة ويندوز 7 من الـ ISO (Workstream 1 · T-013)
#
# لماذا سكربت بدل الذاكرة؟
#   شجرة Win7 (7GB) كانت تُستخرج يدوياً إلى /tmp ⇒ أي إعادة تشغيل للجهاز
#   تمسحها ⇒ كل الاختبارات الحقيقية (ntdll/kernel32 الحقيقيين) تنهار.
#   ⇒ البيئة يجب أن تكون **قابلة لإعادة البناء بأمر واحد**.
#
# الاستخدام:
#   tools/prepare_win7.sh            # يبني الشجرة إن لم تكن موجودة
#   WIN7_ISO=... tools/prepare_win7.sh
#
# المخرجات:
#   <project>/win7full/   الشجرة المستخرجة (Windows/  Program\ Files/  ...)
#   <project>/win7root -> win7full      (رابط رمزي للراحة)
#   /tmp/opencode/win7root -> <project>/win7root   (للإ Paths القديمة)

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ISO="${WIN7_ISO:-$ROOT/win7iso/Windows 7 Professional.iso}"
WIM="$ROOT/win7iso/install.wim"
TREE="${WIN7_TREE:-$ROOT/win7full}"
LINK="$ROOT/win7root"

say() { printf '%s\n' "$*"; }

if [ ! -f "$ISO" ]; then
    say "ERROR: ISO not found: $ISO"
    say "       set WIN7_ISO=/path/to/Windows7.iso"
    exit 1
fi

if [ -f "$TREE/Windows/System32/ntdll.dll" ]; then
    say "OK: tree already present: $TREE"
else
    if [ ! -f "$WIM" ]; then
        say "==> extracting install.wim from the ISO (a few minutes)"
        7z e "$ISO" "sources/install.wim" -o"$(dirname "$WIM")" -y >/dev/null
    else
        say "==> using the cached WIM: $WIM"
    fi

    if [ ! -f "$WIM" ]; then
        say "ERROR: install.wim extraction failed"
        exit 1
    fi

    say "==> extracting the tree into $TREE (7GB, several minutes)"
    rm -rf "$TREE"
    mkdir -p "$TREE"
    7z x "$WIM" -o"$TREE" -y >/dev/null
fi

if [ ! -f "$TREE/Windows/System32/ntdll.dll" ]; then
    say "ERROR: ntdll.dll missing under $TREE — extraction did not produce a tree"
    exit 1
fi

ln -sfn "$(basename "$TREE")" "$LINK"

# المسار القديم في /tmp (للاستخدامات المؤقتة) — يُعاد إن مُسح
mkdir -p /tmp/opencode
ln -sfn "$LINK" /tmp/opencode/win7root

say "OK: win7 tree ready"
say "    tree : $TREE"
say "    link : $LINK"
say "    ntdll: $LINK/Windows/System32/ntdll.dll"
