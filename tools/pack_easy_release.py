#!/usr/bin/env python3
"""Pack release files for the one-script make_rom.py flow.

Produces:
  dist/lttp-ds-blank.nds     — upload as a Release asset
  dist/make-rom-tools.zip    — asset extractor (no ROM data)
  dist/alttp-ds-easy.zip     — optional convenience zip:
                               make_rom.py + blank + tools already unpacked

Requires: python build.py blank  (needs DevKitPro, maintainer only)
"""

import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / "dist"

BLANK_CANDIDATES = [
    ROOT / "lttp-ds-blank.nds",
    ROOT / "easy" / "lttp-ds-blank.nds",
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
            "Build it first: python build.py blank"
        )

    assets_src = ROOT / "vendor" / "zelda3" / "assets"
    if not (assets_src / "restool.py").is_file():
        return fail(
            "error: vendor/zelda3 is missing. Run: git submodule update --init"
        )

    DIST.mkdir(parents=True, exist_ok=True)

    blank_out = DIST / "lttp-ds-blank.nds"
    shutil.copy2(blank, blank_out)

    # make-rom-tools.zip extracts to zelda3/assets/... next to make_rom.py
    tools_stage = DIST / "_tools_stage"
    if tools_stage.exists():
        shutil.rmtree(tools_stage)
    assets_dst = tools_stage / "zelda3" / "assets"
    assets_dst.mkdir(parents=True)
    (assets_dst / "sprites").mkdir(exist_ok=True)

    for name in ASSET_FILES:
        src = assets_src / name
        if not src.is_file():
            return fail("error: missing %s" % src)
        shutil.copy2(src, assets_dst / name)

    for pattern in ("*.txt", "*.yaml", "*.yml", "*.json"):
        for src in assets_src.glob(pattern):
            shutil.copy2(src, assets_dst / src.name)

    tools_zip = DIST / "make-rom-tools.zip"
    if tools_zip.exists():
        tools_zip.unlink()
    with zipfile.ZipFile(tools_zip, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(tools_stage.rglob("*")):
            if path.is_file():
                zf.write(path, path.relative_to(tools_stage).as_posix())

    # Convenience zip: drop zelda3.sfc in, run make_rom.py
    easy_stage = DIST / "alttp-ds-easy"
    if easy_stage.exists():
        shutil.rmtree(easy_stage)
    easy_stage.mkdir()
    shutil.copy2(ROOT / "make_rom.py", easy_stage / "make_rom.py")
    shutil.copy2(blank_out, easy_stage / "lttp-ds-blank.nds")
    shutil.copytree(tools_stage / "zelda3", easy_stage / "zelda3")
    readme = easy_stage / "README.txt"
    readme.write_text(
        "A Link to the Past DS\n"
        "=====================\n"
        "\n"
        "1. Put your USA zelda3.sfc in this folder\n"
        "2. Run:  python make_rom.py\n"
        "   Windows:  py make_rom.py\n"
        "3. Open the new lttp-ds.nds in melonDS or copy it to your flashcart\n"
        "\n"
        "Needs Python 3.8+. The script installs Pillow and PyYAML for you.\n"
        "USA ROM SHA-256:\n"
        "  66871d66be19ad2c34c927d6b14cd8eb6fc3181965b6e517cb361f7316009cfb\n"
        "\n"
        "You supply the ROM. This zip has no Nintendo game data.\n",
        encoding="utf-8",
    )

    easy_zip = DIST / "alttp-ds-easy.zip"
    if easy_zip.exists():
        easy_zip.unlink()
    with zipfile.ZipFile(easy_zip, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(easy_stage.rglob("*")):
            if path.is_file():
                zf.write(path, path.relative_to(DIST).as_posix())

    shutil.rmtree(tools_stage)

    print("Wrote:")
    print("  %s" % blank_out)
    print("  %s" % tools_zip)
    print("  %s" % easy_zip)
    print("Upload all three as GitHub Release assets.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
