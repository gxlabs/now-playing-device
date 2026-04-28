"""
py2app build script for the Now Playing Bridge menu bar app.

Build:
    python setup.py py2app

The resulting .app is in dist/Now Playing Bridge.app
"""
from setuptools import setup

APP = ["menubar_app.py"]
# nowplaying-cli is intentionally NOT bundled: macOS only authorizes
# MediaRemote access for the binary at its installed Homebrew path.
# A copy at any other location returns empty data. server.py resolves
# /opt/homebrew/bin/nowplaying-cli at runtime.
DATA_FILES = [("", ["server.py"])]
OPTIONS = {
    "argv_emulation": False,
    "includes": ["rumps", "serial", "PIL"],
    "packages": ["PIL"],
    "plist": {
        "CFBundleName": "Now Playing Bridge",
        "CFBundleDisplayName": "Now Playing Bridge",
        "CFBundleIdentifier": "com.glen.nowplayingbridge",
        "CFBundleVersion": "1.0.0",
        "CFBundleShortVersionString": "1.0.0",
        "LSUIElement": True,
        "NSHumanReadableCopyright": "",
    },
}

setup(
    name="Now Playing Bridge",
    app=APP,
    data_files=DATA_FILES,
    options={"py2app": OPTIONS},
    setup_requires=["py2app"],
)
