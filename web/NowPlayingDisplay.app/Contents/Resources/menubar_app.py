#!/usr/bin/env python3
"""macOS menu bar app that bridges now-playing data to the ESP32 display.

Reads now-playing state via the bundled MediaRemoteAdapter.framework, loaded
in-process by `/usr/bin/python3` (a `com.apple.*`-signed binary — macOS 15.4+
only authorizes the private MediaRemote framework for callers with that
bundle id, so the bundled py2app Python can't talk to it directly). Auto-
detects the ESP32 over USB, pushes JSON state + RGB565 artwork, and forwards
touch commands back as media controls.
"""
import base64
import glob
import inspect
import io
import json
import os
import struct
import subprocess
import tempfile
import threading
import time
from pathlib import Path

import AppKit
import rumps
import serial
from PIL import Image, ImageDraw

ART_SIZE = 240
PYTHON = "/usr/bin/python3"

# ── Now-playing adapter ───────────────────────────────────────────

_HERE = Path(__file__).parent
_RP = os.environ.get("RESOURCEPATH")
for _b in (Path(_RP) if _RP else None, _HERE):
    if _b and (_b / "vendor" / "mediaremote-adapter").is_dir():
        _ADAPTER = _b / "vendor" / "mediaremote-adapter"
        break
else:
    raise RuntimeError("vendor/mediaremote-adapter not found")

FRAMEWORK = str(_ADAPTER / "Frameworks" / "MediaRemoteAdapter.framework")


def _adapter_helper():
    """Loaded into /usr/bin/python3 via inspect.getsource + `-c` (the bundled
    py2app Python isn't `com.apple.*`-signed so its MediaRemote reads come
    back empty). Receives args via sys.argv: FRAMEWORK_PATH OP [params...]."""
    import ctypes, os, sys
    fw = sys.argv[1]
    op = sys.argv[2]
    lib = ctypes.CDLL(fw + "/" + os.path.basename(fw).removesuffix(".framework"))
    if op == "get":
        for o in sys.argv[3:]:
            if o.startswith("--"):
                k, _, v = o[2:].partition("=")
                os.environ["MEDIAREMOTEADAPTER_OPTION_" + k.replace("-", "_")] = v
        lib.adapter_get_env()
    elif op == "send":
        lib.adapter_send.argtypes = [ctypes.c_int]
        lib.adapter_send(int(sys.argv[3]))
    elif op == "seek":
        lib.adapter_seek.argtypes = [ctypes.c_long]
        lib.adapter_seek(int(sys.argv[3]))


_HELPER_SRC = inspect.getsource(_adapter_helper) + f"\n{_adapter_helper.__name__}()\n"

# MRMediaRemoteCommand enum values
COMMANDS = {"play": 0, "pause": 1, "toggle": 2, "next": 4, "previous": 5}


# py2app sets PYTHONHOME/PYTHONPATH/DYLD_FRAMEWORK_PATH on its bundled Python;
# inheriting them into /usr/bin/python3 breaks its stdlib lookup. Strip them.
_CLEAN_ENV = {
    k: v for k, v in os.environ.items()
    if not k.startswith(("PYTHON", "DYLD_", "_PYI_"))
}


def _adapter(*args, timeout=3) -> bytes:
    return subprocess.run(
        [PYTHON, "-c", _HELPER_SRC, FRAMEWORK, *args],
        capture_output=True, timeout=timeout, env=_CLEAN_ENV,
    ).stdout


def _live_elapsed(p: dict) -> float:
    """Extrapolate current elapsed from MediaRemote's snapshot. The daemon
    only refreshes elapsedTime on play/pause/seek/track-change — during
    steady playback we have to advance it ourselves using the snapshot's
    timestamp + playback rate. Without this the device would see a frozen
    elapsed and continually reset its local interpolation."""
    snapshot = float(p.get("elapsedTimeMicros", 0)) / 1_000_000.0
    ts = float(p.get("timestampEpochMicros", 0)) / 1_000_000.0
    rate = float(p.get("playbackRate") or 0.0)
    if ts <= 0:
        return snapshot
    elapsed = snapshot + (time.time() - ts) * rate
    duration = float(p.get("durationMicros", 0)) / 1_000_000.0
    if duration > 0:
        elapsed = max(0.0, min(duration, elapsed))
    return elapsed


def _payload(with_artwork: bool = False) -> dict:
    args = ["get", "--micros"]
    if not with_artwork:
        args.append("--no-artwork")
    out = _adapter(*args)
    if not out.strip():
        return {}
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return {}


def get_info() -> dict:
    p = _payload(with_artwork=False)
    title = p.get("title")
    if not title:
        return {"playing": False, "trackId": "", "artworkId": ""}

    duration = float(p.get("durationMicros", 0)) / 1_000_000.0
    rate = float(p.get("playbackRate") or 0.0)
    uid = p.get("uniqueIdentifier")
    track_id = str(uid) if uid is not None else f"{p.get('artist','')}|{title}"
    elapsed = _live_elapsed(p)

    # `playing` on the wire means "has track loaded" — the firmware uses it
    # to decide whether to show the controls overlay vs the idle screen.
    # Actual play-vs-pause state is driven by `playbackRate` (0 = paused).
    return {
        "playing": True,
        "title": title,
        "artist": p.get("artist") or "",
        "album": p.get("album") or "",
        "duration": duration,
        "elapsed": elapsed,
        "playbackRate": rate,
        "bundleId": p.get("bundleIdentifier") or "",
        "trackId": track_id,
        "artworkId": str(p.get("contentItemIdentifier") or ""),
    }


def get_artwork_bytes() -> bytes | None:
    data = _payload(with_artwork=True).get("artworkData")
    if not data:
        return None
    try:
        return base64.b64decode(data)
    except Exception:
        return None


def send_command(action: str) -> bool:
    code = COMMANDS.get(action)
    if code is None:
        return False
    _adapter("send", str(code))
    return True


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


def send_state(port, info, lock):
    data = json.dumps(info, separators=(",", ":")).encode()
    with lock:
        port.write(b"\x01" + struct.pack(">H", len(data)) + data)


def send_artwork(port, rgb565, lock):
    with lock:
        port.write(b"\x02" + struct.pack(">I", len(rgb565)) + rgb565)


def send_heartbeat(port, lock):
    with lock:
        port.write(b"\x03")


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
                send_command(text[4:])


# ── App ───────────────────────────────────────────────────────────

class NowPlayingBridge(rumps.App):
    def __init__(self):
        super().__init__("", icon=get_icon(False), quit_button="Quit")
        self.port = None
        self.port_path = None
        self.port_lock = threading.Lock()
        self.reader_thread = None
        self.heartbeat_thread = None
        self.last_art_id = ""
        self._was_connected = False
        self._port_item = rumps.MenuItem("No device")
        self._track_item = rumps.MenuItem("Nothing playing")
        self._hide_item = rumps.MenuItem("Hide", callback=self._hide_icon)
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
            send_state(self.port, info, self.port_lock)

            art_id = info.get("artworkId", "")
            if art_id and art_id != self.last_art_id:
                jpeg = get_artwork_bytes()
                if jpeg:
                    rgb565 = jpeg_to_rgb565(jpeg)
                    send_artwork(self.port, rgb565, self.port_lock)
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
            self.heartbeat_thread = threading.Thread(
                target=self._heartbeat_loop, daemon=True
            )
            self.heartbeat_thread.start()
        except Exception:
            self.port = None
            self.port_path = None

    def _heartbeat_loop(self):
        # Independent of the main tick — keeps the device's disconnect
        # watchdog quiet even when MediaRemote queries stall and tick
        # falls behind.
        port = self.port
        while port is self.port and port is not None and port.is_open:
            try:
                send_heartbeat(port, self.port_lock)
            except Exception:
                return
            time.sleep(1.0)


if __name__ == "__main__":
    NowPlayingBridge().run()
