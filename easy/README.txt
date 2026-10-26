A Link to the Past DS — easy ROM builder
========================================

You do NOT need DevKitPro or a compiler.

What you need
-------------
1. This folder (from a GitHub Release "easy" zip, or the easy/ folder in the repo)
2. lttp-ds-blank.nds in this folder
3. Your legally obtained USA A Link to the Past ROM, named zelda3.sfc
4. Python 3.8 or newer, with Pillow and PyYAML

USA ROM SHA-256:
  66871d66be19ad2c34c927d6b14cd8eb6fc3181965b6e517cb361f7316009cfb

Steps
-----
1. Put zelda3.sfc in this folder (next to make_rom.py).
2. Make sure lttp-ds-blank.nds is also in this folder.
3. Install Python libraries once:

   Windows:
     py -3 -m venv .venv
     .venv\Scripts\pip install Pillow PyYAML

   macOS / Linux:
     python3 -m venv .venv
     .venv/bin/pip install Pillow PyYAML

4. Build the game:

   Windows:  double-click make_rom.bat
   macOS / Linux:  ./make_rom.sh
                 or: python3 make_rom.py

5. Copy the new lttp-ds.nds to your flashcart or open it in melonDS.

This project does not include Nintendo's ROM or game assets. You supply the ROM.
