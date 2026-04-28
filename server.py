#!/usr/bin/env python3
"""Local HTTP server exposing macOS 'Now Playing' state + controls.

Endpoints:
  GET  /              -> browser UI
  GET  /now           -> JSON metadata (no artwork blob)
  GET  /artwork.jpg   -> current artwork as JPEG
  POST /play /pause /toggle /next /previous
  POST /seek?t=SECONDS

Data source: `nowplaying-cli` (Homebrew), which wraps the private
MediaRemote.framework.
"""
import base64
import hashlib
import io
import json
import os
import shutil
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

from PIL import Image

HERE = Path(__file__).parent
PORT = 8787
ARTWORK_KEY = "kMRMediaRemoteNowPlayingInfoArtworkData"


def _find_nowplaying_cli() -> str:
    # Bundled .app processes don't inherit Homebrew's PATH. Prefer a copy
    # bundled by py2app (Contents/Resources/bin/nowplaying-cli), then known
    # Homebrew prefixes, then a PATH search.
    candidates = []
    rp = os.environ.get("RESOURCEPATH")
    if rp:
        candidates.append(Path(rp) / "bin" / "nowplaying-cli")
    candidates.append(HERE / "bin" / "nowplaying-cli")
    candidates += [
        Path("/opt/homebrew/bin/nowplaying-cli"),
        Path("/usr/local/bin/nowplaying-cli"),
    ]
    for c in candidates:
        if c.exists():
            return str(c)
    return shutil.which("nowplaying-cli") or "nowplaying-cli"


NOWPLAYING_CLI = _find_nowplaying_cli()

# Anchor the server's idea of "elapsed" because MediaRemote only snapshots it
# at play/pause/seek/track-change, not continuously. `anchor` holds the last
# observed (elapsed, rate, wall_time); we recompute whenever the snapshot
# moves (track identity changes, or the reported elapsed jumps — e.g. seek).
_anchor_lock = threading.Lock()
_anchor = {"id": None, "elapsed": 0.0, "rate": 0.0, "t": 0.0}


def get_raw() -> dict:
    r = subprocess.run(
        [NOWPLAYING_CLI, "get-raw"],
        capture_output=True, text=True, timeout=3,
    )
    if r.returncode != 0 or not r.stdout.strip():
        return {}
    try:
        return json.loads(r.stdout)
    except json.JSONDecodeError:
        return {}


def _compute_elapsed(reported_elapsed: float, rate: float, track_id) -> float:
    now = time.monotonic()
    with _anchor_lock:
        track_changed = track_id != _anchor["id"]
        # Reset anchor on track change, or when the raw snapshot has actually
        # moved (seek / natural resync), or when rate changes.
        if (
            track_changed
            or abs(reported_elapsed - _anchor["elapsed"]) > 0.5
            or rate != _anchor["rate"]
        ):
            _anchor.update(id=track_id, elapsed=reported_elapsed, rate=rate, t=now)
        dt = now - _anchor["t"]
        return _anchor["elapsed"] + dt * _anchor["rate"]


def _artwork_id(raw: dict) -> str:
    data = raw.get(ARTWORK_KEY)
    if not data:
        return ""
    return hashlib.md5(data.encode() if isinstance(data, str) else data).hexdigest()[:12]


def get_info() -> dict:
    raw = get_raw()

    def k(name):
        return raw.get(f"kMRMediaRemoteNowPlayingInfo{name}")

    title = k("Title")
    if not title:
        return {"playing": False, "trackId": "", "artworkId": ""}

    reported = float(k("ElapsedTime") or 0.0)
    rate = float(k("PlaybackRate") or 0.0)
    uid = k("UniqueIdentifier")
    track_id = str(uid) if uid is not None else f"{k('Artist')}|{title}"
    elapsed = _compute_elapsed(reported, rate, track_id)

    return {
        "playing": True,
        "title": title,
        "artist": k("Artist") or "",
        "album": k("Album") or "",
        "duration": float(k("Duration") or 0.0),
        "elapsed": elapsed,
        "playbackRate": rate,
        "bundleId": k("ClientBundleIdentifier") or "",
        "trackId": track_id,
        "artworkId": _artwork_id(raw),
    }


def get_artwork_bytes() -> bytes | None:
    data = get_raw().get(ARTWORK_KEY)
    if not data:
        return None
    try:
        return base64.b64decode(data)
    except Exception:
        return None


_resize_cache: dict[tuple[str, int], bytes] = {}


def get_artwork_resized(width: int) -> bytes | None:
    raw = get_raw()
    data = raw.get(ARTWORK_KEY)
    if not data:
        return None
    art_id = _artwork_id(raw)
    key = (art_id, width)
    cached = _resize_cache.get(key)
    if cached is not None:
        return cached
    try:
        jpeg = base64.b64decode(data)
        img = Image.open(io.BytesIO(jpeg))
        img.thumbnail((width, width), Image.LANCZOS)
        if img.mode != "RGB":
            img = img.convert("RGB")
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=82, optimize=True)
        out = buf.getvalue()
        # keep cache small: evict older entries when it grows
        if len(_resize_cache) > 8:
            _resize_cache.clear()
        _resize_cache[key] = out
        return out
    except Exception:
        return None


CONTROLS = {
    "play": "play",
    "pause": "pause",
    "toggle": "togglePlayPause",
    "next": "next",
    "previous": "previous",
}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a, **kw):
        pass

    def _json(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            html = (HERE / "index.html").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            self.wfile.write(html)
        elif path == "/now":
            self._json(get_info())
        elif path == "/artwork.jpg":
            qs = parse_qs(urlparse(self.path).query)
            w_raw = qs.get("w", [None])[0]
            data = None
            if w_raw:
                try:
                    w = max(16, min(1024, int(w_raw)))
                    data = get_artwork_resized(w)
                except ValueError:
                    pass
            if data is None:
                data = get_artwork_bytes()
            if not data:
                self.send_response(404); self.end_headers(); return
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(data)
        else:
            self.send_response(404); self.end_headers()

    def do_POST(self):
        parsed = urlparse(self.path)
        action = parsed.path.lstrip("/")
        if action == "seek":
            qs = parse_qs(parsed.query)
            t = qs.get("t", [None])[0]
            if t is None:
                self._json({"error": "missing t"}, 400); return
            subprocess.run([NOWPLAYING_CLI, "seek", t], capture_output=True, timeout=3)
            self._json({"ok": True, "action": "seek", "t": t})
            return
        cmd = CONTROLS.get(action)
        if not cmd:
            self._json({"error": "unknown action"}, 404); return
        subprocess.run([NOWPLAYING_CLI, cmd], capture_output=True, timeout=3)
        self._json({"ok": True, "action": action})


def main():
    print(f"Now Playing server: http://localhost:{PORT}")
    ThreadingHTTPServer.allow_reuse_address = True
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
