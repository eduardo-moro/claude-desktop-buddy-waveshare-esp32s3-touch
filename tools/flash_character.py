#!/usr/bin/env python3
"""
Flash a prepped character pack via USB.

Workflow:
1. Validate character pack
2. Clear local data/characters staging area
3. Copy selected character into data/
4. Erase device flash
5. Upload LittleFS image

Usage:
    python3 tools/flash_character.py characters/bufo
"""

import json
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
DATA_DIR = PROJECT / "data" / "characters"

LITTLEFS_CAP = 1_800_000


def run(cmd: list[str]) -> None:
    print(f"\n> {' '.join(cmd)}")
    subprocess.run(cmd, cwd=PROJECT, check=True)


def get_manifest(src: Path) -> dict:
    manifest_path = src / "manifest.json"

    if not manifest_path.exists():
        sys.exit(
            f"ERROR: no manifest.json in {src}\n"
            f"Run tools/prep_character.py first."
        )

    return json.loads(manifest_path.read_text())


def get_directory_size(path: Path) -> int:
    return sum(
        f.stat().st_size
        for f in path.iterdir()
        if f.is_file()
    )


def stage_character(src: Path, name: str) -> int:
    total_size = get_directory_size(src)

    if total_size > LITTLEFS_CAP:
        sys.exit(
            f"ERROR: character size {total_size:,} bytes "
            f"exceeds LittleFS cap of {LITTLEFS_CAP:,} bytes"
        )

    # Remove old staged characters
    if DATA_DIR.exists():
        shutil.rmtree(DATA_DIR)

    dst = DATA_DIR / name
    shutil.copytree(src, dst)

    print(f"Staged character: {name}")
    print(f"Size: {total_size:,} bytes")
    print(f"Destination: {dst}")

    return total_size


def erase_flash() -> None:
    print("\nErasing flash (including LittleFS)...")
    run(["pio", "run", "-t", "erase"])


def upload_filesystem() -> None:
    print("\nUploading LittleFS image...")
    run(["pio", "run", "-t", "uploadfs"])


def flash(src: Path) -> None:
    manifest = get_manifest(src)
    name = manifest["name"]

    stage_character(src, name)

    # Full erase guarantees LittleFS is clean
    erase_flash()

    # Upload filesystem image
    upload_filesystem()

    print(
        "\nFlash complete.\n"
        "On device: hold A -> settings -> species -> GIF"
    )


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)

    flash(Path(sys.argv[1]).resolve())