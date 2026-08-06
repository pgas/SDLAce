# SDLAce — Jupiter ACE Emulator

```
     ___  ___  _    _             
    / __||   \| |  /_\  ___ ___  
    \__ \| |) | |_/ _ \/ __/ -_) 
    |___/|___/|____/_/ \_\__\___| 
```

**SDLAce** is a cross-platform Jupiter ACE emulator, based on the original
[xAce](https://github.com/epatel/xAce) by Edward Patel, ported from X11 to
SDL2 with a native macOS UI.

The [Jupiter ACE](https://en.wikipedia.org/wiki/Jupiter_Ace) (1982) is the
real *outsider* micro from the 80's — unlike every other home computer of its
era, it used **FORTH** instead of BASIC.  It was designed by Steven Vickers and
Richard Altwasser, who had previously worked on the Sinclair ZX81 and ZX
Spectrum.

---

## Features

- **SDL2 backend** — runs natively on macOS and Linux (no X11 required)
- **Resizable window** with aspect-ratio-preserving scaling and linear filtering
- **HiDPI / Retina** aware (uses renderer output size for correct pixel mapping)
- **Jupiter ACE bezel** — faithful recreation of the real hardware logo:
  cream-white background, *Jupiter* italic + ACE text, cascading red line fan
- **Native macOS menu bar** (Cocoa):
  - Edit → Paste from Host (`⌘V`)
  - Actions → Delete Line, Attach Tape…, Inverse Video, Graphics,
    Spool from File…, Reset, Break
- **Host clipboard paste** (`⌘V` or Edit menu) — text fed through the spooler
- **Full keyboard mapping** including Shift+digit and Shift+symbol keys
  (works around SDL2's macOS keycode behaviour)
- **macOS App Bundle** (`SDLAce.app`) with ROM bundled in Resources

---

## Requirements

| Dependency | macOS (Homebrew) | Linux |
|------------|-----------------|-------|
| SDL2       | `brew install sdl2` | `libsdl2-dev` |
| SDL2_ttf   | `brew install sdl2_ttf` | `libsdl2-ttf-dev` |
| CMake ≥ 3.14 | `brew install cmake` | `cmake` |

---

## Building

```bash
# Clone
git clone https://github.com/pgas/SDLAce.git
cd SDLAce

# Configure + build
mkdir build && cd build
cmake ..
make

# Run (macOS)
open src/SDLAce.app

# Run (Linux)
./src/sdlace
```

---

## Keyboard Controls

| Key | Action |
|-----|--------|
| F1 | Delete Line |
| F2 | Reset |
| F3 | Attach tape image (terminal prompt) |
| F4 | Inverse Video |
| F9 | Graphics mode |
| F11 | Spool from file (terminal prompt) |
| Esc | Break |
| ⌘V | Paste from host clipboard |
| Ctrl-Q | Quit |

On **macOS**, all of the above are also available from the **Actions** menu bar.

### Tape

Press **F3** (or Actions → Attach Tape Image…) and select a `.tap` file.
From that point all LOAD/SAVE operations use that file.

> **Note:** Saving truncates the rest of the tape file from the save point.

### Spooling

Press **F11** (or Actions → Spool from File…) to feed a text file into the
emulator as if it were typed.  Keyboard input is suspended during spooling.

You can also spool from the command line:

```bash
./sdlace -s input.fth       # normal speed
./sdlace -S input.fth       # fast
```

---

## The ROM

The distribution includes the Jupiter ACE ROM image.
See [`boldcomp.email.txt`](boldcomp.email.txt) for the legal position on
distributing it.

---

## Software

The [Jupiter Ace Resource Site](http://www.jupiter-ace.co.uk) is the best
source for `.TAP` files.

---

## Credits

| Role | Person |
|------|--------|
| Original Jupiter ACE hardware | Steven Vickers & Richard Altwasser |
| xz80 (ZX Spectrum emulator) | Ian Collier |
| xz81 (ZX81 emulator) | Russell Marks |
| xAce (original X11 emulator) | Edward Patel |
| xAce maintenance / CMake port | Lawrence Woodman |
| SDLAce (SDL2 port, macOS UI) | Pierre Gaston |

---

## License

GNU General Public License v2 — see [`COPYING`](COPYING).

---

*"An idiot with a computer is a faster, better idiot"* — Rich Julius
