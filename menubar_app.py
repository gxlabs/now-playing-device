#!/usr/bin/env python3
"""macOS menu bar app that bridges now-playing data to the ESP32 display.

Runs the USB serial bridge in the background, auto-detects the device,
and shows connection state in the menu bar icon.
"""
import glob
import io
import json
import struct
import subprocess
import tempfile
import threading
import time

import AppKit
import rumps
import serial
from PIL import Image, ImageDraw

from server import get_info, get_artwork_bytes, CONTROLS, NOWPLAYING_CLI

ART_SIZE = 240

# ── Menu bar icon (circle with check or cross) ────────────────────

def _make_icon(connected: bool) -> str:
    """Generate a 22x22 template icon: thick circle + check (connected) or cross."""
    sz = 44  # 2x for retina
    img = Image.new("RGBA", (sz, sz), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    color = (255, 255, 255, 255) if connected else (160, 160, 160, 220)
    stroke = 4
    pad = 3
    d.ellipse([pad, pad, sz - pad - 1, sz - pad - 1], outline=color, width=stroke)

    cx, cy = sz // 2, sz // 2
    if connected:
        # Checkmark — two segments
        d.line([(cx - 8, cy + 1), (cx - 2, cy + 7)], fill=color, width=stroke)
        d.line([(cx - 2, cy + 7), (cx + 9, cy - 5)], fill=color, width=stroke)
    else:
        # Cross
        d.line([(cx - 7, cy - 7), (cx + 7, cy + 7)], fill=color, width=stroke)
        d.line([(cx - 7, cy + 7), (cx + 7, cy - 7)], fill=color, width=stroke)

    path = tempfile.mktemp(suffix=".png")
    img.save(path)
    return path


_icon_connected = None
_icon_disconnected = None


def get_icon(connected: bool) -> str:
    global _icon_connected, _icon_disconnected
    if connected:
        if _icon_connected is None:
            _icon_connected = _make_icon(True)
        return _icon_connected
    else:
        if _icon_disconnected is None:
            _icon_disconnected = _make_icon(False)
        return _icon_disconnected


# ── Serial helpers ────────────────────────────────────────────────

def jpeg_to_rgb565(jpeg_bytes: bytes) -> bytes:
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


def send_state(port, info):
    data = json.dumps(info, separators=(",", ":")).encode()
    port.write(b"\x01" + struct.pack(">H", len(data)) + data)


def send_artwork(port, rgb565):
    port.write(b"\x02" + struct.pack(">I", len(rgb565)) + rgb565)


def find_device():
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


def read_commands(port):
    buf = b""
    while True:
        try:
            chunk = port.read(port.in_waiting or 1)
        except Exception:
            return
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


# ── App ───────────────────────────────────────────────────────────

class NowPlayingBridge(rumps.App):
    def __init__(self):
        super().__init__("", icon=get_icon(False), quit_button="Quit")
        self.port = None
        self.port_path = None
        self.reader_thread = None
        self.last_art_id = ""
        self._was_connected = False
        self._port_item = rumps.MenuItem("No device")
        self._track_item = rumps.MenuItem("Nothing playing")
        self._hide_item = rumps.MenuItem("Hide icon", callback=self._hide_icon)
        self.menu = [
            self._port_item,
            None,
            self._track_item,
            None,
            self._hide_item,
            None,
        ]
        self._hidden_icon = self._make_blank_icon()
        self._real_icon_connected = get_icon(True)
        self._real_icon_disconnected = get_icon(False)
        self._icon_hidden = False

    @staticmethod
    def _make_blank_icon():
        # 1×1 transparent PNG — collapses the status item to its minimum width
        # so the icon visually disappears until the user relaunches the app.
        img = Image.new("RGBA", (1, 1), (0, 0, 0, 0))
        path = tempfile.mktemp(suffix=".png")
        img.save(path)
        return path

    def _hide_icon(self, _):
        # Remove the NSStatusItem entirely so the menu bar slot disappears.
        # The serial bridge keeps running in the background; user must relaunch
        # the app to bring the icon back.
        self._icon_hidden = True
        AppKit.NSStatusBar.systemStatusBar().removeStatusItem_(
            self._nsapp.nsstatusitem
        )

    @rumps.timer(1)
    def tick(self, _):
        # Auto-connect
        connected = self.port is not None and self.port.is_open
        if not connected:
            self._connect()
            connected = self.port is not None

        # Update menu bar UI on state change (skipped while hidden)
        if not self._icon_hidden:
            if connected != self._was_connected:
                self.icon = get_icon(connected)
                self._was_connected = connected
            self._port_item.title = self.port_path if connected else "No device"

        if not connected:
            if not self._icon_hidden:
                self._track_item.title = "Disconnected"
            return

        try:
            info = get_info()
        except Exception:
            return

        if not self._icon_hidden:
            if info.get("playing"):
                title = info.get("title", "")
                artist = info.get("artist", "")
                self._track_item.title = f"{title} — {artist}" if artist else title
            else:
                self._track_item.title = "Nothing playing"

        # Send to device
        try:
            send_state(self.port, info)

            art_id = info.get("artworkId", "")
            if art_id and art_id != self.last_art_id:
                jpeg = get_artwork_bytes()
                if jpeg:
                    rgb565 = jpeg_to_rgb565(jpeg)
                    send_artwork(self.port, rgb565)
                self.last_art_id = art_id
        except Exception:
            self.port = None
            self.port_path = None
            self.last_art_id = ""

    def _connect(self):
        path = find_device()
        if not path:
            return
        try:
            self.port = serial.Serial(path, 115200, timeout=0.1)
            self.port_path = path
            self.reader_thread = threading.Thread(
                target=read_commands, args=(self.port,), daemon=True
            )
            self.reader_thread.start()
        except Exception:
            self.port = None
            self.port_path = None


if __name__ == "__main__":
    NowPlayingBridge().run()
