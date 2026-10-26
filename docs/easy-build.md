# Easy build (no DevKitPro)

For people who only want a playable `lttp-ds.nds` and already own a USA *A Link to the Past* ROM.

## What you get

| File | Role |
|---|---|
| `lttp-ds-blank.nds` | Prebuilt DS cart with an empty game-data slot |
| `zelda3.sfc` | Your ROM (you supply this) |
| `easy/make_rom.py` | Extracts assets from the ROM and patches them into the blank cart |

The script writes **`lttp-ds.nds`**, ready for melonDS or a flashcart.

You still need **Python 3.8+** with **Pillow** and **PyYAML** (asset extraction). You do **not** need DevKitPro, make, or a C toolchain.

## From a Release zip

1. Download the **easy** zip from [GitHub Releases](https://github.com/JimmyGlasscock/alttp-ds/releases) (includes the blank `.nds` and the extractor).
2. Put your USA `zelda3.sfc` in that folder.
3. Install deps once, then run `make_rom.bat` (Windows) or `./make_rom.sh` (macOS/Linux).

See `easy/README.txt` inside the zip for the exact commands.

USA ROM SHA-256: `66871d66be19ad2c34c927d6b14cd8eb6fc3181965b6e517cb361f7316009cfb`

## From this git repo

```sh
git clone --recurse-submodules https://github.com/JimmyGlasscock/alttp-ds.git
cd alttp-ds
```

1. Download `lttp-ds-blank.nds` from Releases into the project root (or into `easy/`).
2. Copy `zelda3.sfc` next to `easy/make_rom.py`, or to `vendor/zelda3/assets/`.
3. Set up Python and run:

```sh
python3 -m venv .venv
.venv/bin/pip install Pillow PyYAML
python3 easy/make_rom.py
```

## Maintainers: shipping a blank cart

On a machine with DevKitPro (`nds-dev`):

```sh
python3 build.py blank
```

That writes `lttp-ds-blank.nds` (placeholder asset slot, no Nintendo data). Then pack a release zip:

```sh
python3 tools/pack_easy_release.py
```

The zip lands in `dist/` and contains the blank cart, `make_rom` scripts, and a copy of the zelda3 asset tools.

### How the placeholder works

The blank build embeds a fixed 768 KiB slot that starts with the magic bytes `ALTTPDS_ASSET_PLACEHOLDER_v1`. `make_rom.py` finds that marker, overwrites the slot with a real `zelda3_assets.dat` extracted from your ROM, and pads with zeros. The playable `.nds` is a single file, so flashcart menus that never reopen the cart still have the game data in RAM.
