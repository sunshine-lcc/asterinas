#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
#
# Build the DRM/Wayland probes and the minimal Wayland text demo.
#
# Requirements: gcc, make, libdrm-dev, libwayland-dev, wayland-protocols.
# Run from the repository root or from this directory.

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR"

mkdir -p bin

XDG_SHELL_XML=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
if [ -f "$XDG_SHELL_XML" ]; then
    wayland-scanner client-header "$XDG_SHELL_XML" \
        xdg-shell-client-protocol.h
    wayland-scanner private-code "$XDG_SHELL_XML" \
        xdg-shell-client-protocol.c
fi

gcc -O2 -Wall -Wextra -o bin/fb-colorbar fb-colorbar.c
gcc -O2 -Wall -Wextra -I/usr/include/libdrm \
    -o bin/drm-info drm-info.c -ldrm
gcc -O2 -Wall -Wextra -I/usr/include/libdrm \
    -o bin/drm-pageflip drm-pageflip.c -ldrm
gcc -O2 -Wall -Wextra -o bin/wl-colorbar wl-colorbar.c \
    xdg-shell-client-protocol.c -lwayland-client
gcc -O2 -Wall -Wextra -o bin/wl-text-compositor wl-text-compositor.c \
    -lwayland-server
gcc -O2 -Wall -Wextra -o bin/wl-text-client wl-text-client.c \
    -lwayland-client

echo "built:"
ls -1 bin
