"""Build a release and publish it to the browser installer in docs/install.

    python3 tools/build_installer.py v1.0.0

The firmware is built from scratch with that version (shown by ESP-IDF at boot), and the
merged image (bootloader at 0x2000, partition table at 0x8000, app at 0x10000) is copied to
docs/install/ as one file that is flashed at offset 0x0. manifest.json tells ESP Web Tools
which file to write to an ESP32-P4. Without a version argument, `git describe` is used.
"""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
INSTALL_DIR = PROJECT_DIR / "docs" / "install"
ENV = "m5stack-tab5"
IMAGE = "MagicPilot_LIVE-m5stack-tab5.bin"


def find_platformio():
    for candidate in (shutil.which("pio"), Path.home() / ".platformio" / "penv" / "bin" / "pio"):
        if candidate and Path(candidate).exists():
            return str(candidate)
    sys.exit("PlatformIO not found: install PlatformIO Core or the VS Code extension")


def git_version():
    try:
        return subprocess.check_output(
            ["git", "-C", str(PROJECT_DIR), "describe", "--tags", "--always", "--dirty"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "dev"


def main():
    version = sys.argv[1] if len(sys.argv) > 1 else git_version()
    pio = find_platformio()
    env = dict(os.environ, MAGICPILOT_VERSION=version)
    # Clean first: the version is fixed when CMake configures the build
    subprocess.check_call([pio, "run", "-d", str(PROJECT_DIR), "-e", ENV, "-t", "clean"], env=env)
    subprocess.check_call([pio, "run", "-d", str(PROJECT_DIR), "-e", ENV], env=env)

    merged = PROJECT_DIR / ".pio" / "build" / ENV / "firmware.factory.bin"
    INSTALL_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(merged, INSTALL_DIR / IMAGE)
    manifest = {
        "name": "MagicPilot LIVE",
        "version": version,
        "new_install_improv_wait_time": 0,
        "builds": [{"chipFamily": "ESP32-P4", "parts": [{"path": IMAGE, "offset": 0}]}],
    }
    (INSTALL_DIR / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    size = (INSTALL_DIR / IMAGE).stat().st_size
    print("Installer updated in docs/install: %s, %s (%.2f MB)" % (version, IMAGE, size / 1e6))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        print("Build failed; installer files were not changed.", file=sys.stderr)
        sys.exit(error.returncode)
