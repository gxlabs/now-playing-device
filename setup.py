"""
py2app build script for the NowPlayingDisplay menu bar app.

Build:
    python setup.py py2app

The resulting .app is in dist/NowPlayingDisplay.app, with the bundled
mediaremote-adapter framework + perl helper under Contents/Resources/vendor/.
"""
import shutil
import subprocess
from pathlib import Path
from setuptools import setup

from py2app.build_app import py2app


class py2app_with_vendor(py2app):
    """Run py2app, then copy the vendor/ tree into the bundle preserving
    the framework's symlinks (which a naïve setuptools data_files copy
    would flatten, breaking MediaRemoteAdapter.framework's structure)."""

    def run(self):
        super().run()
        app_name = self.distribution.get_name()
        bundle = Path(self.dist_dir) / f"{app_name}.app"
        dest = bundle / "Contents" / "Resources" / "vendor"
        if dest.exists():
            shutil.rmtree(dest)
        subprocess.check_call(["cp", "-R", "vendor", str(dest)])


APP = ["menubar_app.py"]
OPTIONS = {
    "argv_emulation": False,
    "includes": ["rumps", "serial", "PIL"],
    "packages": ["PIL"],
    # Trim ~14 MB of stdlib + tooling we don't use at runtime. SSL/hashlib are
    # only pulled in by network code; this app talks to a local USB serial
    # device + an in-process framework, so neither is reachable.
    "excludes": [
        "setuptools", "pip", "wheel",
        "ssl", "_ssl", "hashlib", "_hashlib",
        "test", "unittest", "tkinter",
    ],
    "plist": {
        "CFBundleName": "NowPlayingDisplay",
        "CFBundleDisplayName": "NowPlayingDisplay",
        "CFBundleIdentifier": "com.gxlabs.nowplayingdisplay",
        "CFBundleVersion": "1.0.0",
        "CFBundleShortVersionString": "1.0.0",
        "LSUIElement": True,
        "NSHumanReadableCopyright": "",
    },
}

setup(
    name="NowPlayingDisplay",
    app=APP,
    options={"py2app": OPTIONS},
    setup_requires=["py2app"],
    cmdclass={"py2app": py2app_with_vendor},
)
