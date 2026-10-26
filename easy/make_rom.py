#!/usr/bin/env python3
"""Build a playable lttp-ds.nds from the blank cart + your USA ALTTP ROM.

No DevKitPro. You only need:
  - lttp-ds-blank.nds  (from a GitHub Release, or `python3 build.py blank`)
  - zelda3.sfc         (USA A Link to the Past you own)
  - Python 3.8+ with Pillow and PyYAML

Put the blank .nds and your .sfc in this folder (or the project root) and run:

  python3 make_rom.py

On Windows you can double-click make_rom.bat instead.
"""

from __future__ import print_function

import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

EASY = Path(__file__).resolve().parent
# Repo root when running from a git checkout; same as EASY for a release zip.
ROOT = EASY.parent if (EASY.parent / "platform" / "nds").is_dir() else EASY

sys.path.insert(0, str(EASY))
from asset_slot import (  # noqa: E402
    ASSET_SLOT_SIZE,
    PLACEHOLDER_MAGIC,
    pack_assets_into_slot,
)

USA_SHA256 = "66871d66be19ad2c34c927d6b14cd8eb6fc3181965b6e517cb361f7316009cfb"
USA_SHA1 = "6D4F10A8B10E10DBE624CB23CF03B88BB8252973"
ASSETS_SIG = b"Zelda3_v0"


def fail(message):
    sys.stderr.write(message.rstrip() + "\n")
    return 1


def pause_if_windows_console():
    if os.name != "nt":
        return
    try:
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.windll.kernel32
        processes = (wintypes.DWORD * 16)()
        count = kernel32.GetConsoleProcessList(processes, len(processes))
    except (AttributeError, OSError):
        return
    if count != 1:
        return
    try:
        input("Press Enter to close.")
    except EOFError:
        pass


def venv_python():
    for base in (EASY, ROOT):
        if os.name == "nt":
            candidate = base / ".venv" / "Scripts" / "python.exe"
        else:
            candidate = base / ".venv" / "bin" / "python"
        if candidate.is_file():
            return candidate
    return None


def has_modules(executable):
    try:
        subprocess.check_call(
            [str(executable), "-c", "import PIL, yaml"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        return False
    return True


def current_has_modules():
    try:
        import PIL  # noqa: F401
        import yaml  # noqa: F401
    except ImportError:
        return False
    return True


def ensure_interpreter():
    if current_has_modules():
        return None
    candidate = venv_python()
    in_venv = False
    for base in (EASY, ROOT):
        if Path(sys.prefix).resolve() == (base / ".venv").resolve():
            in_venv = True
            break
    if candidate and not in_venv and has_modules(candidate):
        script = str(Path(__file__).resolve())
        os.execv(str(candidate), [str(candidate), script, *sys.argv[1:]])
    if os.name == "nt":
        setup = (
            "From this folder:\n"
            "  py -3 -m venv .venv\n"
            "  .venv\\Scripts\\pip install Pillow PyYAML\n"
            "  py make_rom.py"
        )
    else:
        setup = (
            "From this folder:\n"
            "  python3 -m venv .venv\n"
            "  .venv/bin/pip install Pillow PyYAML\n"
            "  python3 make_rom.py"
        )
    return fail(
        "error: need Python 3.8+ with Pillow and PyYAML.\n"
        "This interpreter is %s and is missing those libraries.\n%s"
        % (sys.version.split()[0], setup)
    )


def first_existing(paths):
    for path in paths:
        if path is not None and path.is_file():
            return path
    return None


def find_blank(explicit):
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            fail("error: blank NDS not found: %s" % path)
            return None
        return path
    return first_existing(
        [
            EASY / "lttp-ds-blank.nds",
            ROOT / "lttp-ds-blank.nds",
            Path.cwd() / "lttp-ds-blank.nds",
        ]
    )


def find_rom(explicit):
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            fail("error: ROM not found: %s" % path)
            return None
        return path
    names = ("zelda3.sfc", "zelda3.smc", "Zelda3.sfc", "Zelda3.smc")
    dirs = (EASY, ROOT, Path.cwd(), ROOT / "vendor" / "zelda3" / "assets", ROOT / "data")
    candidates = []
    for directory in dirs:
        for name in names:
            candidates.append(directory / name)
    return first_existing(candidates)


def find_restool():
    candidates = [
        EASY / "zelda3" / "assets" / "restool.py",
        ROOT / "vendor" / "zelda3" / "assets" / "restool.py",
    ]
    return first_existing(candidates)


def strip_smc_header(data):
    if (len(data) & 0xFFFFF) == 0x200:
        return data[0x200:]
    return data


def check_usa_rom(rom_path):
    raw = rom_path.read_bytes()
    body = strip_smc_header(raw)
    sha1 = hashlib.sha1(body).hexdigest().upper()
    sha256 = hashlib.sha256(body).hexdigest()
    if sha1 == USA_SHA1 or sha256 == USA_SHA256:
        return True
    sys.stderr.write(
        "warning: %s does not look like a USA A Link to the Past ROM.\n"
        "Expected SHA-256: %s\n"
        "Got SHA-256:      %s\n"
        "Continuing anyway; the game may show \"Invalid assets file\".\n"
        % (rom_path, USA_SHA256, sha256)
    )
    return False


def extract_assets(restool, rom_path):
    assets_dir = restool.parent
    zelda3_dir = assets_dir.parent
    out = zelda3_dir / "zelda3_assets.dat"
    if out.is_file():
        out.unlink()
    print("Extracting game data from %s ..." % rom_path.name)
    subprocess.check_call(
        [sys.executable, str(restool), "--extract-from-rom", "--rom", str(rom_path)],
        cwd=str(zelda3_dir),
    )
    if not out.is_file() or out.read_bytes()[: len(ASSETS_SIG)] != ASSETS_SIG:
        raise RuntimeError("restool did not write a valid zelda3_assets.dat")
    return out.read_bytes()


def patch_blank(blank_path, assets, output_path):
    image = bytearray(blank_path.read_bytes())
    index = image.find(PLACEHOLDER_MAGIC)
    if index < 0:
        raise RuntimeError(
            "%s has no asset placeholder.\n"
            "Use an lttp-ds-blank.nds built with: python3 build.py blank"
            % blank_path
        )
    if image.find(PLACEHOLDER_MAGIC, index + 1) >= 0:
        raise RuntimeError("multiple asset placeholders found in %s" % blank_path)
    if index + ASSET_SLOT_SIZE > len(image):
        raise RuntimeError("asset slot does not fit inside %s" % blank_path)

    slot = pack_assets_into_slot(assets)
    image[index : index + ASSET_SLOT_SIZE] = slot
    output_path.write_bytes(image)
    return output_path


def parse_args(argv):
    blank = None
    rom = None
    output = None
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg in ("-h", "--help"):
            return "help", None, None, None
        if arg == "--blank" and i + 1 < len(argv):
            blank = argv[i + 1]
            i += 2
            continue
        if arg == "--rom" and i + 1 < len(argv):
            rom = argv[i + 1]
            i += 2
            continue
        if arg in ("-o", "--output") and i + 1 < len(argv):
            output = argv[i + 1]
            i += 2
            continue
        if arg.endswith(".nds") and blank is None:
            blank = arg
            i += 1
            continue
        if arg.lower().endswith((".sfc", ".smc")) and rom is None:
            rom = arg
            i += 1
            continue
        return "error", arg, None, None
    return "ok", blank, rom, output


def main(argv):
    status, blank_arg, rom_arg, output_arg = parse_args(argv)
    if status == "help":
        print(__doc__.strip())
        print()
        print("Usage: python3 make_rom.py [--blank FILE] [--rom FILE] [-o OUT.nds]")
        return 0
    if status == "error":
        code = fail("error: unrecognized argument: %s\nTry: python3 make_rom.py --help" % blank_arg)
        pause_if_windows_console()
        return code

    code = ensure_interpreter()
    if code:
        pause_if_windows_console()
        return code

    blank = find_blank(blank_arg)
    if blank is None:
        code = fail(
            "error: lttp-ds-blank.nds not found.\n"
            "\n"
            "Download it from the project's GitHub Releases and put it in this folder,\n"
            "or build one yourself (needs DevKitPro):\n"
            "  python3 build.py blank\n"
            "\n"
            "See docs/easy-build.md for the full steps."
        )
        pause_if_windows_console()
        return code

    rom = find_rom(rom_arg)
    if rom is None:
        code = fail(
            "error: zelda3.sfc not found.\n"
            "\n"
            "Copy your legally obtained USA A Link to the Past ROM here and name it\n"
            "zelda3.sfc.\n"
            "USA ROM SHA-256: %s" % USA_SHA256
        )
        pause_if_windows_console()
        return code

    restool = find_restool()
    if restool is None:
        code = fail(
            "error: asset extractor not found.\n"
            "Expected easy/zelda3/assets/restool.py or vendor/zelda3/assets/restool.py.\n"
            "If you cloned the git repo, run: git submodule update --init"
        )
        pause_if_windows_console()
        return code

    check_usa_rom(rom)

    try:
        assets = extract_assets(restool, rom)
        print("Assets: %d bytes (slot holds %d)" % (len(assets), ASSET_SLOT_SIZE))
        out = Path(output_arg) if output_arg else (EASY / "lttp-ds.nds")
        if out.resolve() == blank.resolve():
            out = EASY / "lttp-ds.nds"
        patch_blank(blank, assets, out)
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as exc:
        if isinstance(exc, subprocess.CalledProcessError):
            code = fail("error: asset extraction failed (exit %d)." % exc.returncode)
        else:
            code = fail("error: %s" % exc)
        pause_if_windows_console()
        return code

    print("")
    print("Done. Playable ROM:")
    print("  %s" % out.resolve())
    print("Copy that file to your flashcart or open it in melonDS.")
    pause_if_windows_console()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
