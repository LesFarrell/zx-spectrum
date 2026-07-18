# ZX Spectrum Emulator

Small ZX Spectrum emulator in C for Windows. It uses the Win32 API for display, input, and audio, and currently builds with `zig rc` and `zig cc`.

## Current scope

- 48K model
- 128K model with ROM switching and RAM paging through port `0x7FFD`
- Spectrum +3 model with four ROM banks, `0x7FFD`/`0x1FFD` paging,
  all-RAM modes, AY audio, and uPD765 floppy-controller emulation
- ULA screen rendering with border and flash attributes
- Standard ULAplus 64-colour palette mode on ports `0xBF3B`/`0xFF3B`,
  including scanline palette changes and colour-register readback
- Memory-side ULA contention timing for 48K and 128K RAM accesses
- Keyboard matrix input
- Kempston joystick input from an XInput-compatible controller
- 48K beeper sound through the Windows audio device
- 128K AY sound through the Windows audio device
- `.tap` tape loading
- `.tzx` v1.20 tape loading, including direct recording, RLE CSW,
  generalized data, and loop/jump/call/select control flow
- Standard and extended `.dsk` disk images for the Spectrum +3, including
  sector reads, catalogs, bootable disks, and in-memory sector writes
- `.z80` snapshot loading for 48K and 128K snapshots
- `.sna` snapshot loading for 48K and 128K snapshots
- `.szx` ZX-State snapshot loading for original 48K and 128K machines,
  including zlib-compressed RAM pages, 128K paging, AY state, and ULAplus
  `PLTT` palette state

## Not implemented

- Z-RLE-compressed CSW blocks and an interactive choice UI for TZX select
  blocks (the first select option is used automatically)
- 128K SNA snapshots captured with the TR-DOS ROM paged in
- Full ULA I/O contention timing
- Optional ULAplus Timex hi-colour and hi-resolution video modes
- Non-48K/128K `.z80` snapshot models
- SZX machine variants other than the original 48K and 128K models; SZX
  peripheral blocks are skipped while the core machine state is restored
- Creating or formatting DSK tracks, and saving in-memory disk changes back
  to the source `.dsk` file

## Build

```powershell
.\src\build.bat
```

## Run

With no arguments, the emulator looks for `plus3.rom`, `128.rom`, and
`48.rom` in `.\src`. It restores the last selected model when that ROM is
available; on a fresh configuration it prefers +3, then 128K, then 48K.

```powershell
.\src\zxspecemu.exe
```

48K:

```powershell
.\src\zxspecemu.exe --48 path\to\48k.rom
```

128K with two 16 KB ROMs:

```powershell
.\src\zxspecemu.exe --128 path\to\128-0.rom path\to\128-1.rom
```

128K with one combined 32 KB ROM:

```powershell
.\src\zxspecemu.exe --128 path\to\128k-combined.rom
```

Spectrum +3 with one combined 64 KB ROM:

```powershell
.\src\zxspecemu.exe --plus3 path\to\plus3.rom
```

## Keyboard

- `Shift` maps to `CAPS SHIFT`
- `Ctrl` maps to `SYMBOL SHIFT`
- Raw key presses still drive the Spectrum matrix for held keys and games
- Printable keys use the active Windows keyboard layout, so symbols such as `"` follow the host layout

## Controller

- An XInput-compatible pad is exposed as a Kempston joystick
- D-pad or left stick maps to directions
- `A`, `B`, `X`, `Y`, shoulders, or triggers map to fire

## Menu

- `File -> Open Media/Snapshot...` opens `.tap`, `.tzx`, `.dsk`, `.z80`, `.sna`, or `.szx` files
- Drag one supported tape, disk, or snapshot file onto the emulator window to
  load it through the same media handling
- Opening a `.dsk` inserts it as +3 drive `A:`, switches to the +3 model,
  and starts the ROM Loader so bootable disks run automatically
- `Disk -> Eject Disk` removes the current DSK; sector writes remain in
  memory only, and the emulator warns before discarding modified media
- `File -> Auto-load Tapes On Open` toggles whether opening a tape starts loading automatically
- With auto-load on, opening a tape inspects the tape and chooses `48 BASIC` or the `128K` tape loader automatically
- With auto-load off, opening a tape just inserts and rewinds it for manual loading
- Tape and snapshot file reading now run in the background so opening larger `.tap`, `.tzx`, `.z80`, `.sna`, or `.szx` files does not stall the main window
- Standard ROM `LOAD ""` operations fast-load automatically for `.tap` and standard-block `.tzx` files
- `Tape -> Use Fast Tape Loading` runs custom and turbo tape loaders as quickly as the host CPU allows, without generating intermediate audio or frames; it is enabled by default
- Use `F3` or `File -> Play Tape` only for custom loaders or real-time tape playback
- Press `F4` or use `File -> Stop Tape` to stop real-time playback
- `File -> Reset` resets the active machine
- `File -> Exit` closes the emulator
- `Machine -> 48K` / `Machine -> 128K` / `Machine -> +3` rebuilds the emulator for that model and remembers the choice between runs
- `Ctrl+1`, `Ctrl+2`, and `Ctrl+3` switch directly to the 48K, 128K, and +3 models
- `Sound -> Mute Sound` or `Ctrl+M` toggles all emulator audio and remembers the choice between runs
- `Tools -> Assembler...` opens a small RAM patching assembler with support for common Z80 instructions plus `ORG`, `DB`, `DW`, `DS`/`DEFS`, `INCBIN`, `INCLUDE`, and TAP export
- `Tools -> Debugger...` opens a separate debugger window with pause, run, single-step, register state, and memory/disassembly views
- `Tools -> Poke...` opens a small RAM poke tool for writing one or more byte values directly to memory

## Tape tests

Run the focused TAP/TZX decoder and snapshot-state tests with:

```powershell
.\src\test_tape.bat
.\src\test_snapshot.bat
```

## Assembler Notes

- The built-in assembler is intentionally small: it supports labels, `EQU` constants, and a practical subset of Z80 mnemonics, not a full macro assembler
- `File -> New` clears the current source after prompting to save when needed
- The current assembler file is watched for external edits and prompts to reload automatically
- `File -> Reload`, `Ctrl+R`, or `F5` reloads the current assembler file manually; dirty in-editor changes must be discarded first
- `DS count[, fill]` is an alias for `DEFS`
- `DEFS count[, fill]` emits repeated bytes into RAM and advances the assembly address; omitted `fill` defaults to `0`
- `INCBIN "file.bin"` copies raw binary data into RAM at the current assembly address
- `Export TAP...` writes the assembled output as one standard Spectrum `CODE` block in a `.tap` file
- TAP export requires one contiguous assembled output range; sources that jump around with `ORG` cannot be written as a single block
- `INCLUDE "file.asm"` expands another source file in place during assembly, so it can appear in the middle of a source file; relative paths are resolved from the current source file
- `Ctrl+B` assembles the current source
- `Ctrl+F5` assembles the current source and starts execution at its `ORG` address
- `Ctrl+F` opens Find and `Ctrl+H` opens Find and Replace; both support match case and whole-word searches, and searches wrap around the document
- `Ctrl+A`, `Ctrl+Z`, `Ctrl+X`, `Ctrl+C`, and `Ctrl+V` work like a normal text editor inside the assembler source box
- Assembler writes are limited to RAM at `0x4000`-`0xFFFF`; ROM addresses are read-only in the running machine
- Working samples are included at [hello.asm](/C:/Users/Les%20Farrell/OneDrive/Desktop/emu/src/examples/hello.asm), [include-main.asm](/C:/Users/Les%20Farrell/OneDrive/Desktop/emu/src/examples/include-main.asm), and [space-invaders.asm](/C:/Users/Les%20Farrell/OneDrive/Desktop/emu/src/examples/space-invaders.asm)

### Space Invaders sample

The included `space-invaders.asm` is a complete 48K mini-game and a larger example of the integrated assembler. Open it in `Tools -> Assembler...` and press `Ctrl+F5` to assemble it at `8000h` and start it immediately.

- Use `5` and `8` to move and `0` to fire; invader and player explosions have a dedicated beeper effect
- The game includes a three-row animated alien formation, aimed alien bombs, four destructible shield bases, collision detection, score, lives, and win/game-over screens
- Eleven custom 8x8 UDG characters provide the player, projectiles, explosion, bases, and two animation frames for each alien type
- Spectrum attributes give the alien rows and game objects distinct colours
- The 48K beeper provides firing, impact, alien movement, and bomb-drop effects
- Each occupied shield cell absorbs one player laser or alien bomb before being destroyed

## Notes

This is a deliberately simple emulator shell built around the vendored `chips` ZX Spectrum core under the zlib/libpng license. The license text is in [third_party/chips/LICENSE.txt](</C:/Users/Les Farrell/OneDrive/Desktop/emu/third_party/chips/LICENSE.txt>).
