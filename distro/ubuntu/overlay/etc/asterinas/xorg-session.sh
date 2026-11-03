#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
#
# Xorg fallback session for the Ubuntu graphical test image. It runs the
# modesetting driver over simpledrm and starts an xterm with the same
# screenfetch/uname payload used by the Wayland session.

set -u

export HOME=/root
export DISPLAY=:0
export XDG_RUNTIME_DIR=/run/user/0

mkdir -p /run/user/0 /tmp/.X11-unix
chmod 700 /run/user/0
chmod 1777 /tmp/.X11-unix
: > /tmp/asterinas-xorg.log

log() {
    echo "[xorg-session] $*" | tee -a /tmp/asterinas-xorg.log > /dev/ttyS0 2>/dev/null || true
}

log "START"

if [ ! -e /dev/dri/card0 ]; then
    log "NO-DRM: /dev/dri/card0 is missing"
    exit 1
fi

Xorg :0 vt1 -ac -keeptty -novtswitch -noreset \
    -logverbose 3 -logfile /tmp/xorg-server.log \
    >>/tmp/asterinas-xorg.log 2>&1 &
XORG_PID=$!

i=0
while [ "$i" -lt 60 ]; do
    if [ -S /tmp/.X11-unix/X0 ]; then
        break
    fi
    if ! kill -0 "$XORG_PID" 2>/dev/null; then
        break
    fi
    sleep 0.5
    i=$((i + 1))
done

if [ ! -S /tmp/.X11-unix/X0 ]; then
    log "XORG-FAILED"
    while IFS= read -r line; do
        echo "[xorg-log] $line" > /dev/ttyS0
    done < /tmp/xorg-server.log
    exit 1
fi

log "XORG-READY"
log "PTMX: $(ls -l /dev/ptmx 2>&1)"
log "PTS: $(ls -l /dev/pts 2>&1 | head -n 5 | tr '\n' ' ')"
log "PTY-MOUNT: $(grep pts /proc/mounts 2>&1 | tr '\n' ' ')"
PTY_TEST=$(script -q -c 'echo PTY-OK' /dev/null 2>&1 | tr -d '\r')
log "PTY-TEST: $PTY_TEST"

xsetroot -solid "#101820" >>/tmp/asterinas-xorg.log 2>&1 || true

script -q -e -c "xterm -geometry 110x40 -fa 'DejaVu Sans Mono' -fs 10 -e /usr/local/bin/asterinas-terminal" /dev/null \
    >>/tmp/asterinas-xorg.log 2>&1 &
XTERM_PID=$!
sleep 2
if kill -0 "$XTERM_PID" 2>/dev/null; then
    log "XTERM-RUNNING"
else
    log "XTERM-EXITED"
fi
tail -n 40 /tmp/asterinas-xorg.log | while IFS= read -r line; do
    echo "[xorg-log] $line" > /dev/ttyS0
done

wait "$XORG_PID"
log "XORG-EXIT"
kill "$XTERM_PID" 2>/dev/null || true
