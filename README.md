# Adventure Vision Emulator

A cycle-accurate emulator for the **Entex Adventure Vision** (1982), the only table-top game console with a rotating LED mirror display. Written as a single portable C file with SDL2.

## The Hardware

The Adventure Vision was Entex's attempt at a home game console. It used a spinning mirror and a column of 40 red LEDs to produce a 150×40 pixel display — a principle closer to a CRT than to a Game Boy. Only five cartridges were ever released, making it one of the rarest consoles in history.

| Component | Details |
|---|---|
| CPU | Intel 8048 @ 733 KHz (11 MHz ÷ 15) |
| RAM | 64B internal + 4 × 256B external (bank-switched) |
| Sound | National COP411L 4-bit MCU @ 54.4 kHz |
| Display | 150 × 40 red LEDs, mirror-scanned at 15 fps |
| Input | 4 buttons on each side of the unit |

## Features

**Emulation accuracy**

- Full Intel 8048 CPU with proper cycle counting, conditional timing, and interrupt handling
- Column-by-column display rendering synchronized with mirror rotation and dynamic T1 pulse timing
- Complete COP411L behavioral sound emulation: LFSR noise generator, pitch slides, multi-segment volume envelopes, all 13 documented sound effects
- P2-based cartridge bank switching and button matrix decoding

**Interface**

- Game selector menu with embedded box art and game descriptions
- Alphabetical sorting and mouse support (click to select, double-click to launch)
- LED display rendering with per-pixel phosphor persistence and warm red color ramp
- Save states (F5/F7), pause (P), reset (R), volume control (+/−)
- Fullscreen toggle (double-click during gameplay)
- On-screen display for status messages

## Complete Game Library

| Game | Developer | Genre |
|---|---|---|
| Defender | Entex / Williams | Horizontal shoot'em up |
| Super Cobra | Entex / Konami | Horizontal shoot'em up |
| Space Force | Entex | Fixed-screen shooter |
| Turtles | Entex / Stern / Konami | Maze / rescue |
| Table Tennis | Entex | Sports / Pong |

## Building

The emulator is a single C file. Only **SDL2** and standard math are required.

```bash
# Basic build
gcc -O2 -DUSE_SDL -o advision adventure_vision.c -lSDL2 -lm

# With embedded ROMs and box art (self-contained binary)
gcc -O2 -DUSE_SDL -DEMBED_ROMS -DEMBED_COVERS -o advision adventure_vision.c -lSDL2 -lm

# Headless mode (no SDL, for testing/debugging)
gcc -O2 -o advision adventure_vision.c -lm
```

### Build flags

| Flag | Effect |
|---|---|
| `-DUSE_SDL` | Enable SDL2 display, audio, and input |
| `-DEMBED_ROMS` | Embed BIOS and game ROMs into the binary (requires `embedded_roms.h`) |
| `-DEMBED_COVERS` | Embed box art cover images (requires `cover_art.h`) |

### Dependencies

- **SDL2** ≥ 2.0.18 — `apt install libsdl2-dev` (Debian/Ubuntu) or `brew install sdl2` (macOS)
- A C99 compiler (GCC, Clang, MSVC)

## Usage

```bash
# Launch with game selector (scans current directory for .bin/.rom files)
./advision

# Direct launch with explicit ROM files
./advision bios.rom game.rom
```

Place the 1 KB BIOS ROM and game cartridge ROMs (4 KB `.bin` or `.rom`) in the working directory. The menu will list all detected games automatically.

## Controls

### In-game

| Key | Action |
|---|---|
| ↑ ↓ ← → | D-pad |
| Z | Button 1 (fire) |
| X | Button 2 |
| A | Button 3 |
| S | Button 4 |
| P | Pause / resume |
| R | Reset |
| +/− | Volume up / down |
| F5 | Save state |
| F7 | Load state |
| F1 | Toggle debug output |
| Esc | Back to menu |
| Double-click | Toggle fullscreen |

### Menu

| Key / Action | Effect |
|---|---|
| ↑ ↓ | Navigate game list |
| Enter / Z | Launch selected game |
| Click | Select game |
| Double-click | Launch game |
| Esc | Quit |

## Architecture

Everything lives in a single `adventure_vision.c` (~2400 lines):

- **Intel 8048 core** — full instruction set, bank switching, T0/T1 inputs, interrupt controller
- **Memory subsystem** — IRAM, 4-bank XRAM with AV-specific decode, cartridge ROM (up to 4 KB)
- **Display engine** — 150×40 pixel matrix with column-synchronized mirror rotation, phosphor decay simulation
- **COP411L sound** — behavioral emulation of the 4-bit sound MCU including phase-accurate tone generation, LFSR noise, and effect sequencing
- **SDL2 frontend** — windowed/fullscreen rendering, audio callback, input mapping, save states
- **Game menu** — ROM scanner, embedded box art, game info database, mouse interaction with render-target based UI

## Technical References

- Dan Boris — Adventure Vision hardware documentation
- MEGA — Extended hardware research and display timing
- National Semiconductor COP410L/COP411L datasheet

## License

This is an independent open-source project for educational and preservation purposes.
