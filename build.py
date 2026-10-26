#!/usr/bin/env python3
"""Build lttp-ds.nds on Windows, macOS, or Linux."""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def fail(message):
    sys.stderr.write(message.rstrip() + "\n")
    return 1


def posix(path):
    text = str(path)
    if os.name == "nt":
        return text.replace("\\", "/")
    return text


def toolchain_ready(path):
    """A GBA-only devkitPro tree has ds_rules but cannot link an NDS ROM."""
    needed = (
        path / "devkitARM" / "ds_rules",
        path / "libnds" / "lib" / "libnds9.a",
        path / "libnds" / "lib" / "libfat.a",
        path / "calico" / "include" / "calico.h",
        path / "tools" / "bin" / "ndstool",
        path / "tools" / "bin" / "grit",
    )
    for item in needed:
        if item.is_file():
            continue
        if os.name == "nt" and item.with_suffix(".exe").is_file():
            continue
        return False
    return True


def devkitpro():
    candidates = []
    if os.environ.get("DEVKITPRO"):
        candidates.append(Path(os.environ["DEVKITPRO"]))
    if os.name == "nt":
        candidates.append(Path(r"C:\devkitPro"))
        home = os.environ.get("USERPROFILE")
        if home:
            candidates.append(Path(home) / "devkitPro")
    else:
        candidates.append(Path("/opt/devkitpro"))
        candidates.append(Path.home() / "devkitpro")

    seen = set()
    for path in candidates:
        key = os.path.normcase(str(path))
        if key in seen:
            continue
        seen.add(key)
        if toolchain_ready(path):
            return path
    return None


def venv_python():
    if os.name == "nt":
        candidate = ROOT / ".venv" / "Scripts" / "python.exe"
    else:
        candidate = ROOT / ".venv" / "bin" / "python"
    return candidate if candidate.is_file() else None


def has_asset_modules(executable):
    try:
        subprocess.check_call(
            [str(executable), "-c", "import PIL, yaml"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except (OSError, subprocess.CalledProcessError):
        return False
    return True


def current_has_asset_modules():
    try:
        import PIL  # noqa: F401
        import yaml  # noqa: F401
    except ImportError:
        return False
    return True


def ensure_interpreter():
    """Asset extraction and the banner tools import Pillow and PyYAML.

    Compare sys.prefix to the venv directory. Resolving .venv/bin/python
    follows Homebrew's symlink back to the framework binary, so it looks
    like the system interpreter.
    """
    if current_has_asset_modules():
        return None
    candidate = venv_python()
    in_venv = Path(sys.prefix).resolve() == (ROOT / ".venv").resolve()
    if candidate and not in_venv and has_asset_modules(candidate):
        script = str(Path(__file__).resolve())
        os.execv(str(candidate), [str(candidate), script, *sys.argv[1:]])
    if os.name == "nt":
        setup = (
            "From the project root:\n"
            "  py -3 -m venv .venv\n"
            "  .venv\\Scripts\\pip install Pillow PyYAML\n"
            "  py build.py"
        )
    else:
        setup = (
            "Homebrew Python blocks a normal pip install. From the project root:\n"
            "  python3 -m venv .venv\n"
            "  .venv/bin/pip install Pillow PyYAML\n"
            "  python3 build.py"
        )
    code = fail(
        "error: asset extraction needs Python 3.8 or newer, plus Pillow and PyYAML.\n"
        "This interpreter is %s and does not have those libraries.\n%s"
        % (sys.version.split()[0], setup)
    )
    return code


def pause_if_windows_console():
    """Keep a double-clicked Windows console open long enough to read errors."""
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


def main(argv):
    code = ensure_interpreter()
    if code:
        pause_if_windows_console()
        return code

    dk = devkitpro()
    if dk is None:
        code = fail(
            "error: devkitPro with the Nintendo DS libraries was not found.\n"
            "Install devkitPro from https://devkitpro.org/, then install nds-dev:\n"
            "  sudo dkp-pacman -S nds-dev\n"
            "devkitARM alone is not enough. The tree needs libnds, calico, and ndstool.\n"
            "Windows looks in C:\\devkitPro. macOS and Linux look in /opt/devkitpro\n"
            "and ~/devkitpro. Or set DEVKITPRO to that directory.\n"
            "\n"
            "If you only want a playable ROM and already have lttp-ds-blank.nds,\n"
            "you do not need DevKitPro — put zelda3.sfc next to easy/make_rom.py\n"
            "and run that script instead. See docs/easy-build.md."
        )
        pause_if_windows_console()
        return code

    env = os.environ.copy()
    env["DEVKITPRO"] = posix(dk)
    env["DEVKITARM"] = posix(dk / "devkitARM")
    env["PYTHON"] = posix(sys.executable)
    extra_path = [posix(dk / "devkitARM" / "bin"), posix(dk / "tools" / "bin")]
    make = "make"
    if os.name == "nt":
        make_exe = dk / "msys2" / "usr" / "bin" / "make.exe"
        if not make_exe.is_file():
            code = fail(
                "error: %s is missing. Reinstall devkitPro, including msys2."
                % make_exe
            )
            pause_if_windows_console()
            return code
        make = str(make_exe)
        extra_path.insert(0, posix(dk / "msys2" / "usr" / "bin"))
    env["PATH"] = os.pathsep.join(extra_path + [env.get("PATH", "")])

    make_argv = list(argv)
    building_blank = "blank" in make_argv
    try:
        result = subprocess.call([make] + make_argv, cwd=str(ROOT), env=env)
    except OSError as exc:
        code = fail("error: could not run make: %s" % exc)
        pause_if_windows_console()
        return code

    cleaning = "clean" in argv
    blank_nds = ROOT / "lttp-ds-blank.nds"
    playable = ROOT / "lttp-ds.nds"
    if result == 0 and not cleaning:
        print("")
        if building_blank and blank_nds.is_file():
            print("Built %s" % blank_nds)
            print("Ship that file with easy/make_rom.py (see docs/easy-build.md).")
        elif playable.is_file():
            print("Built %s" % playable)
    elif result != 0 and not cleaning:
        print("")
        print("Build failed. lttp-ds.nds was not created.")
        rom = ROOT / "vendor" / "zelda3" / "assets" / "zelda3.sfc"
        dat = ROOT / "zelda3_assets.dat"
        if not building_blank and not rom.is_file() and not dat.is_file():
            print("Put your USA ROM at vendor/zelda3/assets/zelda3.sfc.")
            print("Or run: python3 build.py blank")
    pause_if_windows_console()
    return result


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
