#!/usr/bin/env bash
# M4 — classify PE imports as TRANSLATED (in ntoskrnl_map) or DEFERRED.
# Usage: classify_imports.sh <pe-file>
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PE="$ROOT/tools/peimports/peimports"
[ -x "$PE" ] || gcc -O2 -o "$PE" "$ROOT/tools/peimports/peimports.c"
[ -f "$1" ] || { echo "file not found: $1"; exit 1; }

SUPP="$(mktemp)"
grep -oE '\{ "[^"]+"' "$ROOT/src/pe/ntbox_imports.c" | sed 's/{ "//;s/"$//' | sort -u > "$SUPP"

tmp="$(mktemp)"
"$PE" "$1" 2>/dev/null | grep -E '^   [A-Za-z]' | sed 's/^ *//' | sort -u > "$tmp"

n=0; t=0; d=0
while IFS= read -r fn; do
    [ -z "$fn" ] && continue
    n=$((n+1))
    if grep -qx "$fn" "$SUPP"; then
        echo "TRANSLATED $fn"
        t=$((t+1))
    else
        echo "DEFERRED  $fn"
        d=$((d+1))
    fi
done < "$tmp"

echo "----"
echo "total=$n translated=$t deferred=$d unknown=0"
rm -f "$SUPP" "$tmp"
