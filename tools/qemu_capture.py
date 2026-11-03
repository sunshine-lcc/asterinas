#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Capture the QEMU graphical console through QMP and check pixels.

QEMU's `screendump` command writes a PPM image of the current VGA scanout.
This script talks to a QMP Unix socket, requests a dump, converts the PPM to
PNG with the Python standard library, and optionally asserts that a few
sample points have the expected colors.

Example:
    python3 tools/qemu_capture.py \
        --socket /tmp/asterinas-qmp.sock \
        --output test-artifacts/phase0.png \
        --expect 100,100,ff0000 \
        --expect 300,100,00ff00
"""

import argparse
import json
import os
import socket
import struct
import sys
import time
import zlib


def connect_qmp(path, timeout=60.0):
    deadline = time.monotonic() + timeout
    while True:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(10.0)
        try:
            sock.connect(path)
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.1)


def wait_for_id(stream, wanted_id):
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError("QMP connection closed")
        message = json.loads(line)
        if message.get("id") == wanted_id:
            return message
        # Ignore asynchronous events such as RESET or SHUTDOWN.
        if "event" not in message:
            raise RuntimeError(f"unexpected QMP message: {message}")


def qmp_command(stream, command, arguments=None, command_id=1):
    request = {"execute": command, "id": command_id}
    if arguments is not None:
        request["arguments"] = arguments
    stream.write(json.dumps(request) + "\n")
    stream.flush()
    return wait_for_id(stream, command_id)


def parse_ppm(path):
    with open(path, "rb") as file:
        data = file.read()

    position = 0

    def next_token():
        nonlocal position
        while position < len(data):
            byte = data[position:position + 1]
            if byte in b" \t\r\n":
                position += 1
                continue
            if byte == b"#":
                while position < len(data) and data[position:position + 1] not in b"\r\n":
                    position += 1
                continue
            break
        start = position
        while position < len(data) and data[position:position + 1] not in b" \t\r\n":
            position += 1
        return data[start:position]

    if next_token() != b"P6":
        raise RuntimeError("expected a P6 PPM image")
    width = int(next_token())
    height = int(next_token())
    max_value = int(next_token())
    if max_value != 255:
        raise RuntimeError(f"unsupported PPM max value: {max_value}")

    while position < len(data) and data[position:position + 1] in b" \t\r\n":
        position += 1

    pixels = data[position:position + width * height * 3]
    if len(pixels) != width * height * 3:
        raise RuntimeError("truncated PPM pixel data")
    return width, height, pixels


def write_png(path, width, height, pixels):
    raw = bytearray()
    stride = width * 3
    for row in range(height):
        raw.append(0)
        raw.extend(pixels[row * stride:(row + 1) * stride])

    def chunk(kind, payload):
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")

    with open(path, "wb") as file:
        file.write(png)


def sample_color(width, height, pixels, x, y, radius=3):
    total = [0, 0, 0]
    count = 0
    for yy in range(max(0, y - radius), min(height, y + radius + 1)):
        for xx in range(max(0, x - radius), min(width, x + radius + 1)):
            offset = (yy * width + xx) * 3
            total[0] += pixels[offset]
            total[1] += pixels[offset + 1]
            total[2] += pixels[offset + 2]
            count += 1
    return tuple(value // count for value in total)


def parse_expectation(text):
    try:
        x_text, y_text, color_text = text.split(",", 2)
        color = color_text.removeprefix("#")
        if len(color) != 6:
            raise ValueError
        return int(x_text), int(y_text), tuple(int(color[i:i + 2], 16) for i in (0, 2, 4))
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            f"expected X,Y,RRGGBB, got {text!r}"
        ) from error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, help="QMP Unix socket path")
    parser.add_argument("--output", required=True, help="PNG output path")
    parser.add_argument("--ppm", help="keep the raw PPM at this path")
    parser.add_argument(
        "--expect",
        action="append",
        default=[],
        type=parse_expectation,
        help="assert X,Y,RRGGBB at a sample point; repeatable",
    )
    parser.add_argument(
        "--tolerance",
        type=int,
        default=24,
        help="per-channel tolerance for --expect checks (default: 24)",
    )
    parser.add_argument(
        "--min-nonblack",
        type=float,
        default=0.0,
        help="minimum fraction of sampled non-black pixels (default: 0)",
    )
    args = parser.parse_args()

    output_path = os.path.abspath(args.output)
    ppm_path = os.path.abspath(args.ppm or (args.output + ".ppm"))
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    os.makedirs(os.path.dirname(ppm_path) or ".", exist_ok=True)

    sock = connect_qmp(args.socket)
    stream = sock.makefile("rw")
    # The QMP greeting is the first line.
    json.loads(stream.readline())
    qmp_command(stream, "qmp_capabilities", command_id=1)

    if os.path.exists(ppm_path):
        os.unlink(ppm_path)
    response = qmp_command(
        stream, "screendump", {"filename": ppm_path}, command_id=2
    )
    if "error" in response:
        raise RuntimeError(f"QEMU screendump failed: {response['error']}")

    deadline = time.monotonic() + 10.0
    while not os.path.exists(ppm_path):
        if time.monotonic() >= deadline:
            raise RuntimeError("QEMU did not write the screenshot")
        time.sleep(0.05)

    width, height, pixels = parse_ppm(ppm_path)
    write_png(output_path, width, height, pixels)

    sampled = 0
    nonblack = 0
    stride = width * 3
    for y in range(0, height, max(1, height // 40)):
        for x in range(0, width, max(1, width // 40)):
            offset = y * stride + x * 3
            sampled += 1
            if pixels[offset] or pixels[offset + 1] or pixels[offset + 2]:
                nonblack += 1

    failures = []
    for x, y, expected in args.expect:
        if not (0 <= x < width and 0 <= y < height):
            failures.append(f"sample ({x},{y}) is outside {width}x{height}")
            continue
        actual = sample_color(width, height, pixels, x, y)
        if any(abs(a - e) > args.tolerance for a, e in zip(actual, expected)):
            failures.append(
                f"pixel ({x},{y}) is {actual}, expected {expected}"
            )

    nonblack_fraction = nonblack / sampled if sampled else 0.0
    if nonblack_fraction < args.min_nonblack:
        failures.append(
            f"non-black fraction is {nonblack_fraction:.3f}, "
            f"expected at least {args.min_nonblack:.3f}"
        )

    print(
        json.dumps(
            {
                "png": output_path,
                "ppm": ppm_path,
                "width": width,
                "height": height,
                "nonblack_fraction": round(nonblack_fraction, 4),
                "failures": failures,
            },
            indent=2,
        )
    )

    if failures:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
