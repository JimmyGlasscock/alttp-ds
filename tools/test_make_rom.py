#!/usr/bin/env python3
"""Smoke tests for make_rom.py (no ROM, no DevKitPro, no network)."""

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_make_rom():
    path = ROOT / "make_rom.py"
    spec = importlib.util.spec_from_file_location("make_rom", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class AssetSlotTests(unittest.TestCase):
    def test_placeholder_size_and_magic(self):
        mod = load_make_rom()
        blob = mod.make_placeholder()
        self.assertEqual(len(blob), mod.ASSET_SLOT_SIZE)
        self.assertTrue(blob.startswith(mod.PLACEHOLDER_MAGIC))
        self.assertNotEqual(blob[:8], b"Zelda3_v0")

    def test_pack_rejects_oversized(self):
        mod = load_make_rom()
        with self.assertRaises(ValueError):
            mod.pack_assets_into_slot(b"x" * (mod.ASSET_SLOT_SIZE + 1))

    def test_patch_roundtrip(self):
        mod = load_make_rom()
        prefix = b"NDSHDR" + b"\0" * 100
        suffix = b"TAIL" + b"\xee" * 50
        blank = prefix + mod.make_placeholder() + suffix
        assets = b"Zelda3_v0     \n\0" + b"\x11" * 200

        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            blank_path = tmp / "lttp-ds-blank.nds"
            out_path = tmp / "lttp-ds.nds"
            blank_path.write_bytes(blank)
            mod.patch_blank(blank_path, assets, out_path)
            patched = out_path.read_bytes()

        self.assertEqual(len(patched), len(blank))
        self.assertEqual(patched[: len(prefix)], prefix)
        self.assertEqual(patched[-len(suffix) :], suffix)
        slot = patched[len(prefix) : len(prefix) + mod.ASSET_SLOT_SIZE]
        self.assertTrue(slot.startswith(assets))
        self.assertEqual(slot[len(assets) :], b"\0" * (mod.ASSET_SLOT_SIZE - len(assets)))
        self.assertEqual(slot.find(mod.PLACEHOLDER_MAGIC), -1)


class PrepareBlankTests(unittest.TestCase):
    def test_prepare_assets_blank(self):
        mod = load_make_rom()
        prepare = ROOT / "platform" / "nds" / "prepare_assets.py"
        embed = ROOT / "build" / "embed" / "zelda3_assets.dat"
        if embed.is_file():
            embed.unlink()
        subprocess.check_call([sys.executable, str(prepare), "--blank"])
        data = embed.read_bytes()
        self.assertEqual(len(data), mod.ASSET_SLOT_SIZE)
        self.assertTrue(data.startswith(mod.PLACEHOLDER_MAGIC))


class HelpTests(unittest.TestCase):
    def test_help_needs_no_deps(self):
        out = subprocess.check_output(
            [sys.executable, str(ROOT / "make_rom.py"), "--help"],
            universal_newlines=True,
        )
        self.assertIn("make_rom.py", out)


if __name__ == "__main__":
    unittest.main()
