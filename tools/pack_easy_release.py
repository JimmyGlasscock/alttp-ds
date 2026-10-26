#!/usr/bin/env python3
"""Pack dist/alttp-ds-easy.zip for GitHub Releases.

Requires a previously built lttp-ds-blank.nds (python3 build.py blank).
Copies the make_rom scripts and the zelda3 asset extractor (no ROM data).
"""

import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EASY = ROOT / "easy"
DIST = ROOT / "dist"
STAGE = DIST / "alttp-ds-easy"
ZIP_PATH = DIST / "alttp-ds-easy.zip"

BLANK_CANDIDATES = [
    ROOT / "lttp-ds-blank.nds",
    EASY / "lttp-ds-blank.nds",
]

ASSET_FILES = [
    "restool.py",
    "util.py",
    "extract_resources.py",
    "extract_music.py",
    "compile_resources.py",
    "compile_music.py",
    "decode_music.py",
    "sprite_sheets.py",
    "sprite_sheet_info.py",
    "tables.py",
    "text_compression.py",
    "palette_usage.bin",
]


def fail(message):
    sys.stderr.write(message.rstrip() + "\n")
    return 1


def find_blank():
    for path in BLANK_CANDIDATES:
        if path.is_file():
            return path
    return None


def main():
    blank = find_blank()
    if blank is None:
        return fail(
            "error: lttp-ds-blank.nds not found.\n"
            "Build it first: python3 build.py blank"
        )

    assets_src = ROOT / "vendor" / "zelda3" / "assets"
    if not (assets_src / "restool.py").is_file():
        return fail(
            "error: vendor/zelda3 is missing. Run: git submodule update --init"
        )

    if STAGE.exists():
        shutil.rmtree(STAGE)
    DIST.mkdir(parents=True, exist_ok=True)

    assets_dst = STAGE / "zelda3" / "assets"
    assets_dst.mkdir(parents=True)
    (assets_dst / "sprites").mkdir(exist_ok=True)

    for name in ASSET_FILES:
        src = assets_src / name
        if not src.is_file():
            return fail("error: missing %s" % src)
        shutil.copy2(src, assets_dst / name)

    # Optional tables / text dumps restool may already have in-tree.
    for pattern in ("*.txt", "*.yaml", "*.yml", "*.json"):
        for src in assets_src.glob(pattern):
            shutil.copy2(src, assets_dst / src.name)

    shutil.copy2(blank, STAGE / "lttp-ds-blank.nds")
    for name in (
        "make_rom.py",
        "make_rom.bat",
        "make_rom.sh",
        "README.txt",
        "asset_slot.py",
        "requirements.txt",
    ):
        shutil.copy2(EASY / name, STAGE / name)

    # make_rom looks for easy/zelda3 when ROOT==EASY (release layout).
    # In the staged zip, EASY is the zip root, so zelda3/ sits beside make_rom.py.

    if ZIP_PATH.exists():
        ZIP_PATH.unlink()
    with zipfile.ZipFile(ZIP_PATH, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(STAGE.rglob("*")):
            if path.is_file():
                zf.write(path, path.relative_to(DIST).as_posix())

    print("Wrote %s" % ZIP_PATH)
    print("Upload that zip (and lttp-ds-blank.nds) on the GitHub Release.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
