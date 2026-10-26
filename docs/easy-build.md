# Easy build (one Python script)

No DevKitPro. One script on Windows, macOS, and Linux.

## Steps

1. Get `make_rom.py` (from this repo, or the **easy** zip on [Releases](https://github.com/JimmyGlasscock/alttp-ds/releases)).
2. Put your USA `zelda3.sfc` in the **same folder**.
3. Run:

```sh
python make_rom.py
```

On Windows: `py make_rom.py`. You can also double-click `make_rom.py` if `.py` files open with Python.

The script installs Pillow/PyYAML into a local folder on first run, downloads the blank cart and asset tools from Releases when they are not already present, then writes **`lttp-ds.nds`**.

USA ROM SHA-256: `66871d66be19ad2c34c927d6b14cd8eb6fc3181965b6e517cb361f7316009cfb`

## From a git clone

```sh
git clone --recurse-submodules https://github.com/JimmyGlasscock/alttp-ds.git
cd alttp-ds
# put zelda3.sfc here, then:
python make_rom.py
```

If `lttp-ds-blank.nds` is not in the folder yet, the script pulls it from the latest GitHub Release.

## Maintainers

```sh
python build.py blank                 # needs DevKitPro; no ROM data in the blank
python tools/pack_easy_release.py     # dist/lttp-ds-blank.nds, make-rom-tools.zip, alttp-ds-easy.zip
```

Upload those three files on the GitHub Release. End users only ever run `make_rom.py`.
