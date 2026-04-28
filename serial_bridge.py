#!/usr/bin/env python3
"""USB serial bridge — pushes now-playing state to the ESP32 display.

Protocol (Mac → ESP32):
  0x01 <2-byte BE len> <JSON>      state update (~1 Hz)
  0x02 <4-byte BE len> <RGB565>    artwork (on track change)

Protocol (ESP32 → Mac):
  "CMD:<action>\n"                  touch commands (toggle/next/previous)

Usage:
  python serial_bridge.py /dev/cu.usbmodemXXXXX
"""
import io
import json
import struct
import subprocess
import sys
import threading
import time

import serial
from PIL import Image

from server import get_info, get_artwork_bytes, CONTROLS, NOWPLAYING_CLI

ART_SIZE = 240


def find_device():
    """Probe USB serial ports for our ESP32 device (ping/ack handshake)."""
    import glob
    for path in glob.glob("/dev/cu.usbmodem*"):
        try:
            port = serial.Serial(path, 115200, timeout=0.5)
            port.reset_input_buffer()
            port.write(b"\x00")
            time.sleep(0.2)
            resp = port.read(port.in_waiting or 20)
            if b"NP:ACK" in resp:
                port.close()
                return path
            port.close()
        except Exception:
            pass
    return None


def jpeg_to_rgb565(jpeg_bytes: bytes) -> bytes:
    """Decode JPEG, resize to ART_SIZE, convert to little-endian RGB565."""
    img = Image.open(io.BytesIO(jpeg_bytes))
    img = img.resize((ART_SIZE, ART_SIZE), Image.LANCZOS)
    if img.mode != "RGB":
        img = img.convert("RGB")
    px = img.tobytes()
    out = bytearray(ART_SIZE * ART_SIZE * 2)
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        j = (i // 3) * 2
        out[j] = c & 0xFF
        out[j + 1] = (c >> 8) & 0xFF
    return bytes(out)


def send_state(port: serial.Serial, info: dict):
    data = json.dumps(info, separators=(",", ":")).encode()
    port.write(b"\x01" + struct.pack(">H", len(data)) + data)


def send_artwork(port: serial.Serial, rgb565: bytes):
    port.write(b"\x02" + struct.pack(">I", len(rgb565)) + rgb565)


def reader_thread(port: serial.Serial):
    """Read lines from ESP32, execute media commands."""
    buf = b""
    while True:
        chunk = port.read(port.in_waiting or 1)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", errors="ignore").strip()
            if text.startswith("CMD:"):
                action = text[4:]
                cmd = CONTROLS.get(action)
                if cmd:
                    subprocess.run(
                        [NOWPLAYING_CLI, cmd],
                        capture_output=True, timeout=3,
                    )
                    print(f"  → {action}")


def main():
    explicit_port = sys.argv[1] if len(sys.argv) >= 2 else None
    port = None
    reader = None
    last_art_id = ""

    while True:
        # Connect / reconnect
        if port is None or not port.is_open:
            last_art_id = ""
            if explicit_port:
                path = explicit_port
            else:
                path = find_device()
            if path is None:
                print("  waiting for device...")
                time.sleep(2.0)
                continue
            try:
                port = serial.Serial(path, 115200, timeout=0.1)
                reader = threading.Thread(
                    target=reader_thread, args=(port,), daemon=True
                )
                reader.start()
                print(f"Serial bridge: {port.name}")
            except Exception:
                port = None
                time.sleep(2.0)
                continue

        # Push data
        try:
            info = get_info()
            send_state(port, info)

            art_id = info.get("artworkId", "")
            if art_id and art_id != last_art_id:
                jpeg = get_artwork_bytes()
                if jpeg:
                    rgb565 = jpeg_to_rgb565(jpeg)
                    send_artwork(port, rgb565)
                    print(f"  artwork: {len(rgb565)} bytes")
                last_art_id = art_id
        except Exception:
            print("  device disconnected")
            port = None

        time.sleep(1.0)


if __name__ == "__main__":
    main()
