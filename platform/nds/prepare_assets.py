"""Put zelda3_assets.dat where the NDS link can incbin it.

The file has to already exist, or a USA ROM has to be present so restool
can extract one. The .nds links those bytes into the ARM9 binary.

Pass --blank to embed a fixed-size placeholder instead. That produces
lttp-ds-blank.nds for the easy make_rom.py flow (no DevKitPro needed
for end users).
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "easy"))
from asset_slot import ASSET_SLOT_SIZE, make_placeholder  # noqa: E402

HOME = Path.home()
EMBED = ROOT / "build" / "embed" / "zelda3_assets.dat"
CANONICAL = ROOT / "zelda3_assets.dat"
EXTRACTED = ROOT / "vendor" / "zelda3" / "zelda3_assets.dat"
SIG = b"Zelda3_v0"
USA_SHA256 = "66871d66be19ad2c34c927d6b14cd8eb6fc3181965b6e517cb361f7316009cfb"

DAT_CANDIDATES = [
    CANONICAL,
    ROOT / "data" / "zelda3_assets.dat",
    EXTRACTED,
    HOME / "Desktop" / "zelda3_assets.dat",
    HOME / "Desktop" / "ALTTP-DS" / "zelda3_assets.dat",
]

ROM_CANDIDATES = [
    ROOT / "vendor" / "zelda3" / "zelda3.sfc",
    ROOT / "vendor" / "zelda3" / "zelda3.smc",
    ROOT / "vendor" / "zelda3" / "assets" / "zelda3.sfc",
    ROOT / "vendor" / "zelda3" / "assets" / "zelda3.smc",
    ROOT / "zelda3.sfc",
    ROOT / "zelda3.smc",
    ROOT / "data" / "zelda3.sfc",
    ROOT / "data" / "zelda3.smc",
]


def fail(message):
    sys.stderr.write(message.rstrip() + "\n")
    sys.exit(1)


def first_file(paths):
    for path in paths:
        if path.is_file():
            return path
    return None


def valid_dat(path):
    if not path.is_file() or path.stat().st_size < 128:
        return False
    with path.open("rb") as handle:
        return handle.read(len(SIG)) == SIG


def publish(src):
    EMBED.parent.mkdir(parents=True, exist_ok=True)
    if EMBED.exists():
        src_stat = src.stat()
        dst_stat = EMBED.stat()
        if (src_stat.st_size == dst_stat.st_size and
                int(src_stat.st_mtime) == int(dst_stat.st_mtime)):
            return
    shutil.copy2(src, EMBED)


def restool_python():
    """restool uses syntax that needs Python 3.8. python on PATH may be older."""
    probe = (
        "import sys; assert sys.version_info >= (3, 8); "
        "import PIL, yaml; print(sys.executable)"
    )

    def supports(executable):
        try:
            out = subprocess.check_output(
                [executable, "-c", probe],
                stderr=subprocess.DEVNULL,
                universal_newlines=True,
            ).strip()
        except (OSError, subprocess.CalledProcessError):
            return None
        return out or None

    found = supports(sys.executable)
    if found:
        return found
    for ver in ("3.12", "3.11", "3.10", "3.9", "3.8"):
        try:
            out = subprocess.check_output(
                ["py", "-" + ver, "-c", "import sys; print(sys.executable)"],
                stderr=subprocess.DEVNULL,
                universal_newlines=True,
            ).strip()
        except (OSError, subprocess.CalledProcessError):
            continue
        found = supports(out) if out else None
        if found:
            return found
    if os.name == "nt":
        setup = "py -3 -m pip install Pillow PyYAML"
    else:
        setup = (
            "python3 -m venv .venv && "
            ".venv/bin/pip install Pillow PyYAML && "
            "python3 build.py"
        )
    fail(
        "error: asset extraction needs Python 3.8 or newer, plus Pillow and PyYAML.\n"
        "python on PATH is %s.\n"
        "Install them with: %s"
        % (sys.version.split()[0], setup)
    )


def extract(rom):
    restool = ROOT / "vendor" / "zelda3" / "assets" / "restool.py"
    if not restool.is_file():
        fail("error: vendor/zelda3 is missing. Run: git submodule update --init")
    python = restool_python()
    print("assets: extracting zelda3_assets.dat from %s" % rom)
    subprocess.check_call(
        [python, str(restool), "--extract-from-rom", "--rom", str(rom)],
        cwd=str(ROOT / "vendor" / "zelda3"),
    )
    if not valid_dat(EXTRACTED):
        fail("error: extraction did not write a valid zelda3_assets.dat")
    if EXTRACTED.resolve() != CANONICAL.resolve():
        shutil.copy2(EXTRACTED, CANONICAL)
    return CANONICAL


def write_blank():
    print(
        "assets: embedding blank placeholder (%d bytes) for make_rom.py"
        % ASSET_SLOT_SIZE
    )
    EMBED.parent.mkdir(parents=True, exist_ok=True)
    EMBED.write_bytes(make_placeholder())


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--blank" in argv:
        write_blank()
        return

    found = first_file(DAT_CANDIDATES)
    if found is not None:
        if not valid_dat(found):
            fail("error: %s is not a zelda3_assets.dat extracted from a USA ROM." % found)
        print("assets: using %s" % found)
        publish(found)
        return

    rom = first_file(ROM_CANDIDATES)
    if rom is None:
        fail(
            "error: this build needs game data from a USA A Link to the Past ROM you own.\n"
            "\n"
            "Copy your USA ROM to vendor/zelda3/assets/zelda3.sfc.\n"
            "USA ROM SHA-256: %s\n"
            "\n"
            "Or build a blank ROM for the easy packager:\n"
            "  python3 build.py blank\n"
            "\n"
            "The ROM stays on your PC and is not committed."
            % USA_SHA256
        )
    publish(extract(rom))


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        sys.exit(exc.returncode)
