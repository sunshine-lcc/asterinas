#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
#
# Start a native Wayland session on the simpledrm DRM device.
#
# Weston's DRM backend is tried first because it provides a desktop shell.
# If it cannot acquire a seat or the DRM device, fall back to cage, which uses
# wlroots' legacy KMS path (WLR_DRM_NO_ATOMIC=1) and a fullscreen terminal.

set -u

export HOME=/root
export XDG_RUNTIME_DIR=/run/user/0
export XDG_SESSION_TYPE=wayland
export WAYLAND_DISPLAY=wayland-0
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export LD_LIBRARY_PATH=/usr/local/lib:${LD_LIBRARY_PATH:-}

# Software rendering avoids requiring GBM/EGL on simpledrm.
export WLR_RENDERER=pixman
export WLR_DRM_NO_ATOMIC=1
export WLR_DRM_DEVICES=/dev/dri/card0
export WLR_LIBINPUT_NO_DEVICES=1
export WLR_BACKENDS=drm
export WLR_LOG=debug

LOG=/tmp/asterinas-wayland.log
WESTON_LOG=/tmp/asterinas-weston.log
CAGE_LOG=/tmp/asterinas-cage.log
mkdir -p /run/user/0
chmod 700 /run/user/0
: > "$LOG"
: > "$WESTON_LOG"
: > "$CAGE_LOG"

log() {
    echo "[wayland-session] $*" | tee -a "$LOG" > /dev/ttyS0 2>/dev/null || true
}

log "START"

for path in /dev/fb0 /dev/dri/card0 /dev/input/event0 /dev/input/event1 /dev/tty0; do
    if [ -e "$path" ]; then
        log "DEVICE-PRESENT: $path"
    else
        log "DEVICE-MISSING: $path"
    fi
done

if [ -e /dev/fb0 ]; then
    log "FB0: $(ls -l /dev/fb0 2>&1)"
fi
if [ -d /dev/dri ]; then
    log "DRI: $(ls -l /dev/dri 2>&1 | tr '\n' ' ')"
fi

if [ ! -e /dev/dri/card0 ]; then
    log "NO-DRM: /dev/dri/card0 is missing"
    exit 1
fi

if [ -x /usr/local/bin/drm-info ]; then
    /usr/local/bin/drm-info 2>&1 | while IFS= read -r line; do
        log "$line"
    done
fi

if [ -x /usr/local/bin/drm-pageflip ]; then
    log "DRM-PAGEFLIP: running"
    /usr/local/bin/drm-pageflip 2>&1 | tee -a "$LOG" > /dev/ttyS0
    log "DRM-PAGEFLIP: exit=$?"
fi

# systemd-logind is a placeholder in the Ubuntu rootfs, so libseat cannot use
# the logind backend. The distro's seatd.service normally provides the socket;
# only start a private daemon if it is absent.
if [ -S /run/seatd.sock ]; then
    export SEATD_SOCK=/run/seatd.sock
    export LIBSEAT_BACKEND=seatd
    log "SEATD: using existing socket /run/seatd.sock"
elif command -v seatd >/dev/null 2>&1; then
    seatd -g root -s /run/seatd.sock >>"$LOG" 2>&1 &
    SEATD_PID=$!
    export SEATD_SOCK=/run/seatd.sock
    export LIBSEAT_BACKEND=seatd
    sleep 1
    log "SEATD: pid=$SEATD_PID socket=$SEATD_SOCK"
fi

dump_log() {
    local path=$1
    [ -f "$path" ] || return 0
    while IFS= read -r line; do
        echo "[session-log] $line" > /dev/ttyS0
    done < "$path"
}

try_weston() {
    log "WESTON: starting"
    weston --backend=drm-backend.so --renderer=pixman --idle-time=0 \
        --log="$WESTON_LOG" &
    WESTON_PID=$!

    i=0
    while [ "$i" -lt 30 ]; do
        if [ -S /run/user/0/wayland-0 ]; then
            return 0
        fi
        if ! kill -0 "$WESTON_PID" 2>/dev/null; then
            return 1
        fi
        sleep 0.5
        i=$((i + 1))
    done
    return 1
}

start_terminal() {
    # weston-terminal only uses core Wayland, xdg-shell, shm, and seat. It is
    # a better fit for the legacy DRM backend than foot's newer protocol set.
    weston-terminal --maximized \
        --shell=/usr/local/bin/asterinas-terminal
}

if try_weston; then
    log "WESTON-READY: socket=/run/user/0/wayland-0"
    WAYLAND_DISPLAY=wayland-0 start_terminal >>"$LOG" 2>&1 &
    TERMINAL_PID=$!
    wait "$WESTON_PID"
    log "WESTON-EXIT"
    kill "$TERMINAL_PID" 2>/dev/null || true
else
    log "WESTON-FAILED: falling back to cage"
    dump_log "$WESTON_LOG"
    kill "$WESTON_PID" 2>/dev/null || true
    wait "$WESTON_PID" 2>/dev/null || true
    log "CAGE: starting"
    # cage/wlroots autodetects backends from the environment. Drop the Weston
    # socket name so wlroots does not try its nested Wayland backend first.
    CAGE_BIN=/usr/bin/cage
    if [ -x /usr/local/bin/cage-debug ]; then
        CAGE_BIN=/usr/local/bin/cage-debug
    fi
    timeout 30 env -u WAYLAND_DISPLAY "$CAGE_BIN" -- \
        weston-terminal --maximized \
        --shell=/usr/local/bin/asterinas-terminal \
        2>&1 | tee -a "$CAGE_LOG" > /dev/ttyS0
    log "CAGE-EXIT"
    dump_log "$CAGE_LOG"
fi

if [ -n "${SEATD_PID:-}" ]; then
    kill "$SEATD_PID" 2>/dev/null || true
fi
