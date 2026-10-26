#!/usr/bin/env python3
"""Smoke tests for the blank-slot packager (no ROM, no DevKitPro)."""

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

EASY = Path(__file__).resolve().parent
sys.path.insert(0, str(EASY))

from asset_slot import (  # noqa: E402
    ASSET_SLOT_SIZE,
    PLACEHOLDER_MAGIC,
    make_placeholder,
    pack_assets_into_slot,
)


def load_make_rom():
    path = EASY / "make_rom.py"
    spec = importlib.util.spec_from_file_location("make_rom", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class AssetSlotTests(unittest.TestCase):
    def test_placeholder_size_and_magic(self):
        blob = make_placeholder()
        self.assertEqual(len(blob), ASSET_SLOT_SIZE)
        self.assertTrue(blob.startswith(PLACEHOLDER_MAGIC))
        self.assertNotEqual(blob[:8], b"Zelda3_v0")

    def test_pack_rejects_oversized(self):
        with self.assertRaises(ValueError):
            pack_assets_into_slot(b"x" * (ASSET_SLOT_SIZE + 1))

    def test_patch_roundtrip(self):
        make_rom = load_make_rom()
        prefix = b"NDSHDR" + b"\0" * 100
        suffix = b"TAIL" + b"\xee" * 50
        blank = prefix + make_placeholder() + suffix
        # Fake assets header; size well under the slot.
        assets = b"Zelda3_v0     \n\0" + b"\x11" * 200

        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            blank_path = tmp / "lttp-ds-blank.nds"
            out_path = tmp / "lttp-ds.nds"
            blank_path.write_bytes(blank)
            make_rom.patch_blank(blank_path, assets, out_path)
            patched = out_path.read_bytes()

        self.assertEqual(len(patched), len(blank))
        self.assertEqual(patched[: len(prefix)], prefix)
        self.assertEqual(patched[-len(suffix) :], suffix)
        slot = patched[len(prefix) : len(prefix) + ASSET_SLOT_SIZE]
        self.assertTrue(slot.startswith(assets))
        self.assertEqual(slot[len(assets) :], b"\0" * (ASSET_SLOT_SIZE - len(assets)))
        self.assertEqual(slot.find(PLACEHOLDER_MAGIC), -1)


class PrepareBlankTests(unittest.TestCase):
    def test_prepare_assets_blank(self):
        root = EASY.parent
        prepare = root / "platform" / "nds" / "prepare_assets.py"
        embed = root / "build" / "embed" / "zelda3_assets.dat"
        if embed.is_file():
            embed.unlink()
        import subprocess

        subprocess.check_call([sys.executable, str(prepare), "--blank"])
        data = embed.read_bytes()
        self.assertEqual(len(data), ASSET_SLOT_SIZE)
        self.assertTrue(data.startswith(PLACEHOLDER_MAGIC))


if __name__ == "__main__":
    unittest.main()
