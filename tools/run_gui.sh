#!/usr/bin/env bash
# Run any Windows GUI EXE with NT-Box and automatically spawn Weston Wayland compositor
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXE="${1:-}"

if [ -z "$EXE" ]; then
    echo "Usage: $0 <path/to/app.exe>"
    exit 1
fi

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
WESTON_PID=""

if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    if command -v weston >/dev/null 2>&1; then
        rm -f "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null || true
        weston --backend=x11-backend.so >/tmp/weston_gui.log 2>&1 &
        WESTON_PID=$!
        sleep 2
        for s in "$XDG_RUNTIME_DIR"/wayland-*; do
            [ -S "$s" ] && export WAYLAND_DISPLAY=$(basename "$s")
        done
    fi
fi

echo "=================================================="
echo " Starting Windows App via NT-Box GUI Pipeline"
echo " EXE: $EXE"
echo " WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-none}"
echo "=================================================="

"$ROOT/tools/ntboxd/ntboxd" "$EXE" || true

if [ -n "$WESTON_PID" ]; then
    kill "$WESTON_PID" 2>/dev/null || true
fi
echo "Done."
