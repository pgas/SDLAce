# Changelog

All notable changes to SDLAce are documented here.

---

## [0.8] — 2026

### Added
- **Internal Speaker sound support**: Emulated the Jupiter Ace CPU-driven buzzer via host-side SDL Audio. Reading or writing to even I/O ports moves the speaker diaphragm. Synthesized and queued square-wave audio at 44100 Hz.
- **Audio DC Blocker Filter**: Implemented a first-order high-pass filter (decay coefficient `0.995f`, cutoff ~35 Hz) to eliminate clicks and crackles when the speaker is idle or when audio buffer underflows occur.

---

## [0.7] — 2026

### Added
- **Rewind Tape** shortcut: `F6` / `⌘6` to re-attach (rewind) the current tape.

### Fixed
- **Tape loading memory mirroring**: Fixed an issue where tape blocks loaded into mirrored memory regions (e.g., `0x2000-0x23FF`) were incorrectly bypassing the Z80 memory mirror logic. `bload` and Forth dictionary auto-execution now work accurately.

---

## [0.6-sdl] — 2026 (SDL2 port by Pierre Gaston)

### Added
- **SDL2 backend** — full replacement of the X11 backend with SDL2.
  Builds and runs natively on macOS and Linux without X11.
- **Resizable window** with aspect-ratio-preserving scaling and bilinear
  (`linear`) filtering via `SDL_HINT_RENDER_SCALE_QUALITY`.
- **HiDPI / Retina display** support: render coordinates computed with
  `SDL_GetRendererOutputSize` rather than `SDL_GetWindowSize`, so the
  emulated screen is crisp and correctly positioned on Retina displays.
- **Jupiter ACE bezel**: the window border is styled to reproduce the real
  hardware — very light grey background, *Jupiter* in large bold italic,
  *ACE* below in regular weight, cascading fan of 9 red horizontal lines.
  Font rendered via SDL2_ttf (Helvetica Neue on macOS, DejaVu/Liberation
  on Linux), sizes scaled for HiDPI.
- **Native macOS menu bar** (`macos_ui.m`, Objective-C/Cocoa):
  - *Edit → Paste from Host* (`⌘V`)
  - *Actions* menu: Delete Line, Attach Tape… (NSOpenPanel), Inverse Video,
    Graphics, Spool from File… (NSOpenPanel), Reset, Break
- **Host clipboard paste**: `SDL_GetClipboardText()` writes to a temp file
  fed through the existing spooler; triggered by `⌘V` or Edit menu.
- **macOS App Bundle** (`SDLAce.app`): `MACOSX_BUNDLE` with ROM bundled in
  `Contents/Resources/`.
- **SDL2-only build**: `CMakeLists.txt` simplified — no `USE_SDL` flag
  needed, always SDL2.  `SDL2_ttf` added as a required dependency.
- **Objective-C language** enabled in root `CMakeLists.txt` so `macos_ui.m`
  compiles correctly.

### Changed
- Project renamed **SDLAce** throughout (CMake target, bundle name, window
  title, help text).
- **Reset moved from F12 → F2** (keyboard, menu, help text).
- `keyboard.h` simplified: always SDL2 types, X11 `#ifdef` removed.
- `tests/CMakeLists.txt` updated to reference `SDLAce_SOURCE_DIR`.

### Fixed
- **Shift+digit keys** (Shift+1…0): SDL2 on macOS reports `SDLK_1 +
  KMOD_SHIFT` rather than `SDLK_EXCLAIM`.  Added `is_digit_key()` to
  activate ACE Symbol Shift (port 0, 0xfd) alongside the digit key.
- **Shift+symbol keys** (`-`, `=`, `[`, `]`, `;`, `'`, `,`, `.`, `/`, `` ` ``):
  same SDL2 macOS behaviour.  Added `shifted_symbol_response[]` table and
  `keyboard_get_shifted_symbol()` lookup; `keyboard_keypress/keyrelease`
  now dispatch through three paths (letter / digit / symbol).
- `spooler.h` / `spooler.c`: removed direct X11 includes.

### Removed
- **X11 backend** entirely: `src/xmain.c` and `src/keyboard.c` deleted.
- The `USE_SDL` CMake option (SDL2 is now the only backend).

---

## [0.5] — 22nd December 2012 (Lawrence Woodman)

- Switch build method from IMake to CMake.
- Support attaching of tape files.
- Add file spooling.
- Create proper checksum when saving files.

---

## [0.4.1] — 29th June 2010

- Adds support for 32bpp displays.
- Frees more allocated memory when finished with.
- Corrects assignment of strings to variables with no allocated storage.
- Adds more file I/O error detection.
- Improves portability.
- Switched to just using `.tap` instead of `.dic` and `.byt` when loading
  and saving files.
- Changes keybindings.

---

## [0.4] — 1997 (Edward Patel)

- Written by Edward Patel, based on xz81 by Russell Marks (itself based on
  Ian Collier's xz80, a ZX Spectrum emulator for X).
