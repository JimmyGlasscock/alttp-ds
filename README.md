# A Link To The Past DS

*The Legend of Zelda: A Link to the Past* Dual Screen port for the Nintendo DS / DS Lite.

<p align="center">
  <img width="1200" alt="alttp" src="docs/images/showcase.jpg" />
</p>

ALTTP DS is a port of [snesrev/zelda3](https://github.com/snesrev/zelda3), a C reimplementation of the SNES game. The game logic runs natively on the DS's ARM9. The game has been optimized for the DS hardware, and has options to use the touchscreen for the HUD and map.

> **You supply the ROM.** This repo contains no Nintendo ROM and no game assets. Put a legally obtained USA `zelda3.sfc` in `vendor/zelda3/assets` and run `build.py`. This project is not affiliated with or endorsed by Nintendo.

## Features

### A native port

- **Full game.** The entire game is playable, running zelda3's reimplemented game logic.
- **Optimized for DS screen size.** The SNES picture is 224 lines and the DS screen is 192. The overworld and dungeon cameras scroll a little past room and area edges, so Link and the action stay in view.
- **Lid handling.** Closing the DS lid pauses the game.

### Bottom-screen HUD and touch controls

Set **HUD** to **Bottom Screen** in Settings and the status bar moves off the top screen, so the whole top screen is the game. The bottom screen then shows:

- Hearts, magic meter, rupee, bomb and arrow counters, and the equipped item.
- A **live map**: the world map outdoors and the dungeon map inside a dungeon. It stays black in caves and houses.
- An **Items** button, which opens and closes the game's item menu.
- A **Gear** button, which opens a panel with your equipment, bottles, pendants, crystals, hearts, and heart pieces.
- **Touch to equip.** Tap an item on the item panel to equip it.

When no game is running (title, file select, boot), the bottom screen shows a Triforce idle picture.

### Quality-of-life additions

- **Settings menu.** Open it by pressing select (see [Settings](#settings)).
- **Title skip.** Pressing a button during the intro jumps to the sword drop, just like the GBA version.
- **Fast dialogue.** Hold **A** to fast forward through cutscene dialogue.

### Settings

Press **Select** in game to open the save menu. It has three rows: **Continue**, **Save and Quit**, and **Settings**. Settings are saved in your `.sav` file.

| Setting | Options | What it does |
|---|---|---|
| HUD | Top Screen / Bottom Screen | Where the HUD lives (life, magic meter, etc) |
| Low HP Indicator | On / Off | The low-health beep |
| Spawn | Normal / Dungeon | A save made inside a dungeon loads at that dungeon's entrance |
| Magic Mirror | Normal / Free | Removes magic mirror use restrictions |
| Bomb Capacity | 10 / 99 | Maximum bombs you can carry |
| Arrow Capacity | 30 / 99 | Maximum arrows you can carry |
| Run Speed | Normal / Double / Triple | How fast Link walks, dashes, and swims |
| Dark Rooms | Normal / Bright | Bright keeps dark rooms lit, so the lantern is optional |
| Hair Color | Pink / Blonde | Link's hair color |
| Button Mapping | | Allows you to remap the controls |

## Controls
You can remap the controls however you like in the settings menu.

## Getting started

> **You supply the ROM.** This project ships no Nintendo ROM and no game assets.

### Easy path (no DevKitPro)

One script, all platforms:

1. Put your USA `zelda3.sfc` next to `make_rom.py` (repo root, or the easy zip from [Releases](https://github.com/JimmyGlasscock/alttp-ds/releases)).
2. Run:

```sh
python make_rom.py
```

That writes **`lttp-ds.nds`**. First run sets up Pillow/PyYAML and fetches the blank cart from Releases if needed. More detail: [docs/easy-build.md](docs/easy-build.md).

### Full build (developers)

#### What you need

- A legally obtained **USA** *A Link to the Past* ROM, named `zelda3.sfc`.
  SHA-256: `66871d66be19ad2c34c927d6b14cd8eb6fc3181965b6e517cb361f7316009cfb`
- [devkitPro](https://devkitpro.org/) with the **nds-dev** group: libnds 2.0, libfat, calico, and ndstool. On macOS and Linux, after the installer:

  ```sh
  sudo dkp-pacman -S nds-dev
  ```

  On Windows, select nds-dev in the installer, or run `dkp-pacman -S nds-dev` from a devkitPro shell. `build.py` looks at `DEVKITPRO`, then `C:\devkitPro` or `/opt/devkitpro`, then `~/devkitpro`. Set `DEVKITPRO` if you installed it somewhere else.
- [Python 3.8](https://www.python.org/) or newer, with `Pillow` and `PyYAML`. Homebrew Python blocks a normal `pip install`, so from the project root:

  ```sh
  python3 -m venv .venv
  .venv/bin/pip install Pillow PyYAML
  ```

  On Windows: `py -3 -m venv .venv`, then `.venv\Scripts\pip install Pillow PyYAML`. `python3 build.py` uses that `.venv` when the Python on `PATH` does not already have those libraries. On a very new Python, PyYAML may compile from source. On macOS that needs the Xcode command line tools (`xcode-select --install`).
- Git.
- To run it: [melonDS](https://melonds.melonemu.org/), or a DS, DS Lite, or DSi with a DLDI flashcart or Twilight Menu++.

#### 1. Clone

The game logic is in the `vendor/zelda3` submodule, so clone with submodules:

```sh
git clone --recurse-submodules https://github.com/JimmyGlasscock/alttp-ds.git
cd alttp-ds
```

If you already cloned without them: `git submodule update --init`.

#### 2. Add your ROM

Copy `zelda3.sfc` to `vendor/zelda3/assets/`.

#### 3. Build

From the project root:

```sh
python3 build.py
```

On Windows, `py build.py` does the same thing. Either command uses `.venv` when the default Python is missing Pillow or PyYAML. `python3 build.py -j8` compiles in parallel. When it finishes, **`lttp-ds.nds`** is in the project root. If `zelda3.sfc` is missing, the build stops and does not write that file. Double-clicking `build.py` on Windows leaves the window open so you can read the error.

To ship the easy-build blank cart: `python3 build.py blank`, then `python3 tools/pack_easy_release.py` (upload the files in `dist/` to a GitHub Release).

If make stops with `target pattern contains no '%'`, dependency files in `platform/nds/arm7/build/` were written on Windows and still contain `C:` paths. Run `python3 build.py clean` and build again. The ARM7 makefile drops those files and the objects they belonged to, then compiles the voice driver again. An ARM7 object left behind without a dependency file is rebuilt too, so a Mac link cannot keep a Windows mixer.

#### 4. Run

`lttp-ds.nds` is generated with the game data inside it. If that data is not from a USA ROM, the bottom screen says `Invalid assets file` and the game waits there.

## Saves

- On a flashcart the save is an 8 KB `.sav` in TWiLight Menu++'s save location, named after the ROM. It holds the three game slots and your Settings. melonDS folder sync still writes `lttp-ds.sav` in the synced folder.
- Settings are stored in unused space past the save slots, so they travel with the file.
- A save from an older build at the card root, or in `saves/sram.dat`, is still read and written to that location on the next save.

## Not included

These zelda3 extras are left out on purpose, because the DS cannot run them or has no use for them: shaders, widescreen, 240-line mode, hi-res Mode 7, MSU-1 / Opus audio, OpenGL, and the ROM comparison mode.

## Project layout

```
alttp-ds/
  vendor/zelda3/        game logic, PPU and audio code (git submodule)
  platform/nds/
    source/             ARM9: host, video, HUD, map, save, audio
    arm7/               ARM7: sound-channel driver
    bottom-screen/      HUD artwork (PNG + grit)
    include/            stubs that replace SDL / Opus
    prepare_assets.py   reads vendor/zelda3/assets/zelda3.sfc and stages the game data for the link
  make_rom.py           one-script packager: blank cart + your .sfc → lttp-ds.nds
  build.py, Makefile    full DevKitPro build
```

## Credits

- [snesrev/zelda3](https://github.com/snesrev/zelda3) and its contributors, for the C reimplementation, the asset tool, and the PPU and DSP code this port builds on.
- [zelda-alttp-3ds](https://github.com/EstebanPdN/zelda-alttp-3ds) I based much of my UI off the designs in this port.
- [devkitPro](https://devkitpro.org/), libnds, and Calico, for the DS toolchain.
- [melonDS](https://melonds.melonemu.org/), for the emulator used throughout development.

*The Legend of Zelda* and *A Link to the Past* are trademarks of Nintendo. No Nintendo code or assets are distributed with this project.
