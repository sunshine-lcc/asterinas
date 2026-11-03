#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
#
# Minimal Wayland text session: a tiny wl_shm compositor writes directly to
# /dev/fb0, and a wl_shm client renders terminal-style text.

set -u

export HOME=/root
export XDG_RUNTIME_DIR=/run/user/0
export WAYLAND_DISPLAY=wayland-0

mkdir -p /run/user/0
chmod 700 /run/user/0

LOG=/tmp/asterinas-wltext.log
: > "$LOG"

log() {
    echo "[wltext] $*" | tee -a "$LOG" > /dev/ttyS0 2>/dev/null || true
}

log "START"

/usr/local/bin/wl-text-compositor >>"$LOG" 2>&1 &
COMPOSITOR_PID=$!

i=0
while [ "$i" -lt 50 ]; do
    if [ -S /run/user/0/wayland-0 ]; then
        break
    fi
    if ! kill -0 "$COMPOSITOR_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
    i=$((i + 1))
done

if [ ! -S /run/user/0/wayland-0 ]; then
    log "COMPOSITOR-FAILED"
    while IFS= read -r line; do
        echo "[wltext-log] $line" > /dev/ttyS0
    done < "$LOG"
    exit 1
fi

log "COMPOSITOR-READY"
WAYLAND_DISPLAY=wayland-0 /usr/local/bin/wl-text-client >>"$LOG" 2>&1 &
CLIENT_PID=$!

wait "$CLIENT_PID"
log "CLIENT-EXIT"

kill "$COMPOSITOR_PID" 2>/dev/null || true
