#!/usr/bin/env python3
"""Make a playable lttp-ds.nds from your USA A Link to the Past ROM.

All platforms — one script. No DevKitPro.

  1. Put zelda3.sfc in the same folder as this file
  2. Run:  python make_rom.py
     (Windows: py make_rom.py)

First run installs Pillow/PyYAML into a local venv if needed, and downloads
lttp-ds-blank.nds from GitHub Releases when it is not already here.
"""

from __future__ import print_function

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import urlopen

# ---------------------------------------------------------------------------
# Blank-cart asset slot (keep in sync with platform/nds/prepare_assets.py)
# ---------------------------------------------------------------------------
PLACEHOLDER_MAGIC = b"ALTTPDS_ASSET_PLACEHOLDER_v1\0"
ASSET_SLOT_SIZE = 768 * 1024

USA_SHA256 = "66871d66be19ad2c34c927d6b14cd8eb6fc3181965b6e517cb361f7316009cfb"
USA_SHA1 = "6D4F10A8B10E10DBE624CB23CF03B88BB8252973"
ASSETS_SIG = b"Zelda3_v0"

# GitHub release assets (maintainer uploads these after `python build.py blank`)
RELEASE_BLANK_URL = (
    "https://github.com/JimmyGlasscock/alttp-ds/releases/latest/download/"
    "lttp-ds-blank.nds"
)
RELEASE_TOOLS_URL = (
    "https://github.com/JimmyGlasscock/alttp-ds/releases/latest/download/"
    "make-rom-tools.zip"
)

HERE = Path(__file__).resolve().parent
# Git checkout: this file lives at the repo root. Release zip: same layout.
ROOT = HERE


def make_placeholder():
    data = bytearray(ASSET_SLOT_SIZE)
    data[: len(PLACEHOLDER_MAGIC)] = PLACEHOLDER_MAGIC
    return bytes(data)


def pack_assets_into_slot(assets):
    if len(assets) > ASSET_SLOT_SIZE:
        raise ValueError(
            "zelda3_assets.dat is %d bytes; blank slot holds %d. "
            "Rebuild the blank with a larger ASSET_SLOT_SIZE."
            % (len(assets), ASSET_SLOT_SIZE)
        )
    return assets + b"\0" * (ASSET_SLOT_SIZE - len(assets))


def fail(message):
    sys.stderr.write(message.rstrip() + "\n")
    return 1


def pause_if_windows_console():
    """Keep a double-clicked Windows console open so errors are readable."""
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


def venv_paths():
    venv = HERE / ".make_rom_venv"
    if os.name == "nt":
        return venv, venv / "Scripts" / "python.exe", venv / "Scripts" / "pip.exe"
    return venv, venv / "bin" / "python", venv / "bin" / "pip"


def has_modules(executable=None):
    if executable is None:
        try:
            import PIL  # noqa: F401
            import yaml  # noqa: F401
        except ImportError:
            return False
        return True
    try:
        subprocess.check_call(
            [str(executable), "-c", "import PIL, yaml"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        return False
    return True


def ensure_interpreter():
    """Re-exec inside a local venv with Pillow/PyYAML if the current Python lacks them."""
    if has_modules():
        return None

    venv, python, pip = venv_paths()
    in_venv = Path(sys.prefix).resolve() == venv.resolve()

    if python.is_file() and not in_venv and has_modules(python):
        os.execv(str(python), [str(python), str(Path(__file__).resolve()), *sys.argv[1:]])

    print("Setting up Python packages (Pillow, PyYAML) — one-time…")
    try:
        if not python.is_file():
            subprocess.check_call([sys.executable, "-m", "venv", str(venv)])
        subprocess.check_call([str(pip), "install", "--upgrade", "pip"])
        subprocess.check_call([str(pip), "install", "Pillow", "PyYAML"])
    except (OSError, subprocess.CalledProcessError) as exc:
        return fail(
            "error: could not install Pillow/PyYAML.\n"
            "Install Python 3.8+ from https://www.python.org/ and try again.\n"
            "(%s)" % exc
        )

    if not has_modules(python):
        return fail("error: packages installed but still cannot import Pillow/PyYAML.")

    os.execv(str(python), [str(python), str(Path(__file__).resolve()), *sys.argv[1:]])


def first_file(paths):
    for path in paths:
        if path is not None and Path(path).is_file():
            return Path(path)
    return None


def download(url, dest):
    print("Downloading %s …" % url.rsplit("/", 1)[-1])
    try:
        with urlopen(url, timeout=120) as resp:
            data = resp.read()
    except HTTPError as exc:
        raise RuntimeError("download failed (%s): %s" % (exc.code, url)) from exc
    except URLError as exc:
        raise RuntimeError("download failed: %s\n%s" % (url, exc)) from exc
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    return dest


def find_rom(explicit):
    if explicit:
        path = Path(explicit)
        return path if path.is_file() else None
    names = ("zelda3.sfc", "zelda3.smc", "Zelda3.sfc", "Zelda3.smc")
    dirs = (
        HERE,
        Path.cwd(),
        HERE / "vendor" / "zelda3" / "assets",
        HERE / "data",
    )
    return first_file(d / n for d in dirs for n in names)


def find_blank(explicit):
    if explicit:
        path = Path(explicit)
        return path if path.is_file() else None
    found = first_file(
        [
            HERE / "lttp-ds-blank.nds",
            Path.cwd() / "lttp-ds-blank.nds",
            HERE / "easy" / "lttp-ds-blank.nds",
        ]
    )
    if found:
        return found
    dest = HERE / "lttp-ds-blank.nds"
    try:
        download(RELEASE_BLANK_URL, dest)
    except RuntimeError as exc:
        fail(
            "error: lttp-ds-blank.nds not found and download failed.\n"
            "%s\n"
            "\n"
            "Put lttp-ds-blank.nds next to make_rom.py, or build one with:\n"
            "  python build.py blank\n"
            "(that step needs DevKitPro; end users should not need it.)" % exc
        )
        return None
    if not dest.is_file() or dest.stat().st_size < 1000:
        fail("error: downloaded blank cart looks empty.")
        return None
    return dest


def find_restool():
    found = first_file(
        [
            HERE / "zelda3" / "assets" / "restool.py",
            HERE / "vendor" / "zelda3" / "assets" / "restool.py",
            HERE / "easy" / "zelda3" / "assets" / "restool.py",
        ]
    )
    if found:
        return found

    tools_zip = HERE / "make-rom-tools.zip"
    tools_dir = HERE / "zelda3"
    try:
        if not tools_zip.is_file():
            download(RELEASE_TOOLS_URL, tools_zip)
        print("Unpacking asset tools …")
        if tools_dir.exists():
            shutil.rmtree(tools_dir)
        with zipfile.ZipFile(tools_zip, "r") as zf:
            zf.extractall(HERE)
    except (RuntimeError, OSError, zipfile.BadZipFile) as exc:
        fail(
            "error: asset extractor not found and download failed.\n"
            "%s\n"
            "If you cloned the git repo, run: git submodule update --init" % exc
        )
        return None

    found = first_file([HERE / "zelda3" / "assets" / "restool.py"])
    if found is None:
        fail("error: make-rom-tools.zip did not contain zelda3/assets/restool.py")
    return found


def strip_smc_header(data):
    if (len(data) & 0xFFFFF) == 0x200:
        return data[0x200:]
    return data


def check_usa_rom(rom_path):
    body = strip_smc_header(rom_path.read_bytes())
    sha1 = hashlib.sha1(body).hexdigest().upper()
    sha256 = hashlib.sha256(body).hexdigest()
    if sha1 == USA_SHA1 or sha256 == USA_SHA256:
        return
    sys.stderr.write(
        "warning: %s may not be a USA A Link to the Past ROM.\n"
        "Expected SHA-256: %s\n"
        "Got SHA-256:      %s\n"
        "Continuing; the game may show \"Invalid assets file\".\n"
        % (rom_path.name, USA_SHA256, sha256)
    )


def extract_assets(restool, rom_path):
    assets_dir = restool.parent
    zelda3_dir = assets_dir.parent
    out = zelda3_dir / "zelda3_assets.dat"
    if out.is_file():
        out.unlink()
    print("Extracting game data from %s …" % rom_path.name)
    subprocess.check_call(
        [sys.executable, str(restool), "--extract-from-rom", "--rom", str(rom_path)],
        cwd=str(zelda3_dir),
    )
    if not out.is_file() or out.read_bytes()[: len(ASSETS_SIG)] != ASSETS_SIG:
        raise RuntimeError("extraction did not write a valid zelda3_assets.dat")
    return out.read_bytes()


def patch_blank(blank_path, assets, output_path):
    image = bytearray(blank_path.read_bytes())
    index = image.find(PLACEHOLDER_MAGIC)
    if index < 0:
        raise RuntimeError(
            "%s has no asset placeholder.\n"
            "Use an lttp-ds-blank.nds from a Release, or: python build.py blank"
            % blank_path.name
        )
    if image.find(PLACEHOLDER_MAGIC, index + 1) >= 0:
        raise RuntimeError("multiple asset placeholders found in %s" % blank_path.name)
    if index + ASSET_SLOT_SIZE > len(image):
        raise RuntimeError("asset slot does not fit inside %s" % blank_path.name)

    image[index : index + ASSET_SLOT_SIZE] = pack_assets_into_slot(assets)
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
            blank, i = argv[i + 1], i + 2
            continue
        if arg == "--rom" and i + 1 < len(argv):
            rom, i = argv[i + 1], i + 2
            continue
        if arg in ("-o", "--output") and i + 1 < len(argv):
            output, i = argv[i + 1], i + 2
            continue
        if arg.endswith(".nds") and blank is None:
            blank, i = arg, i + 1
            continue
        if arg.lower().endswith((".sfc", ".smc")) and rom is None:
            rom, i = arg, i + 1
            continue
        return "error", arg, None, None
    return "ok", blank, rom, output


def main(argv):
    status, blank_arg, rom_arg, output_arg = parse_args(argv)
    if status == "help":
        print(__doc__.strip())
        print()
        print("Usage: python make_rom.py [--rom FILE] [--blank FILE] [-o OUT.nds]")
        return 0
    if status == "error":
        code = fail("error: unrecognized argument: %s\nTry: python make_rom.py --help" % blank_arg)
        pause_if_windows_console()
        return code

    code = ensure_interpreter()
    if code:
        pause_if_windows_console()
        return code

    rom = find_rom(rom_arg)
    if rom is None:
        code = fail(
            "error: zelda3.sfc not found.\n"
            "\n"
            "Copy your USA A Link to the Past ROM into:\n"
            "  %s\n"
            "and name it zelda3.sfc\n"
            "\n"
            "USA ROM SHA-256:\n"
            "  %s" % (HERE, USA_SHA256)
        )
        pause_if_windows_console()
        return code

    blank = find_blank(blank_arg)
    if blank is None:
        pause_if_windows_console()
        return 1

    restool = find_restool()
    if restool is None:
        pause_if_windows_console()
        return 1

    check_usa_rom(rom)

    try:
        assets = extract_assets(restool, rom)
        print("Assets: %d bytes (slot %d)" % (len(assets), ASSET_SLOT_SIZE))
        out = Path(output_arg) if output_arg else (HERE / "lttp-ds.nds")
        if out.resolve() == blank.resolve():
            out = HERE / "lttp-ds.nds"
        patch_blank(blank, assets, out)
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as exc:
        if isinstance(exc, subprocess.CalledProcessError):
            code = fail("error: asset extraction failed (exit %d)." % exc.returncode)
        else:
            code = fail("error: %s" % exc)
        pause_if_windows_console()
        return code

    print("")
    print("Done → %s" % out.resolve())
    print("Copy that file to your flashcart or open it in melonDS.")
    pause_if_windows_console()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
