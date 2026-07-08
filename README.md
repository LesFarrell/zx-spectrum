# ZX Spectrum Emulator

Small ZX Spectrum emulator in C for Windows. It uses the Win32 API for display, input, and audio, and currently builds with `zig rc` and `zig cc`.

## Current scope

- 48K model
- 128K model with ROM switching and RAM paging through port `0x7FFD`
- ULA screen rendering with border and flash attributes
- Memory-side ULA contention timing for 48K and 128K RAM accesses
- Keyboard matrix input
- Kempston joystick input from an XInput-compatible controller
- 48K beeper sound through the Windows audio device
- 128K AY sound through the Windows audio device
- `.tap` tape loading
- `.tzx` tape loading for standard-speed and common pulse/data blocks
- `.z80` snapshot loading for 48K and 128K snapshots

## Not implemented

- Advanced `.tzx` block types such as direct recording, generalized data, and call/jump control flow
- Full ULA I/O contention timing
- `.sna` snapshot loading
- Non-48K/128K `.z80` snapshot models

## Build

```powershell
.\src\build.bat
```

## Run

With no arguments, the emulator looks for `128.rom` in `.\src` first and starts in `128K` mode. If that is not present, it falls back to `48.rom`.

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

- `File -> Open Tape/Snapshot...` opens `.tap`, `.tzx`, or `.z80` files
- `File -> Auto-load Tapes On Open` toggles whether opening a tape starts loading automatically
- With auto-load on, opening a tape inspects the tape and chooses `48 BASIC` or the `128K` tape loader automatically
- With auto-load off, opening a tape just inserts and rewinds it for manual loading
- Tape and snapshot file reading now run in the background so opening larger `.tap`, `.tzx`, or `.z80` files does not stall the main window
- Standard ROM `LOAD ""` operations fast-load automatically for `.tap` and standard-block `.tzx` files
- Use `F3` or `File -> Play Tape` only for custom loaders or real-time tape playback
- Press `F4` or use `File -> Stop Tape` to stop real-time playback
- `File -> Reset` resets the active machine
- `File -> Exit` closes the emulator
- `Machine -> 48K` / `Machine -> 128K` rebuilds the emulator for that model and remembers the choice between runs
- `Tools -> Assembler...` opens a small RAM patching assembler with support for common Z80 instructions plus `ORG`, `DB`, `DW`, `DS`/`DEFS`, `INCBIN`, `INCLUDE`, and TAP export
- `Tools -> Debugger...` opens a separate debugger window with pause, run, single-step, register state, and memory/disassembly views

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
- `Ctrl+A`, `Ctrl+Z`, `Ctrl+X`, `Ctrl+C`, and `Ctrl+V` work like a normal text editor inside the assembler source box
- Assembler writes are limited to RAM at `0x4000`-`0xFFFF`; ROM addresses are read-only in the running machine
- Working samples are included at [hello.asm](/C:/Users/Les%20Farrell/OneDrive/Desktop/emu/src/examples/hello.asm), [include-main.asm](/C:/Users/Les%20Farrell/OneDrive/Desktop/emu/src/examples/include-main.asm), and [space-invaders.asm](/C:/Users/Les%20Farrell/OneDrive/Desktop/emu/src/examples/space-invaders.asm)

## Notes

This is a deliberately simple emulator shell built around the vendored `chips` ZX Spectrum core under the zlib/libpng license. The license text is in [third_party/chips/LICENSE.txt](</C:/Users/Les Farrell/OneDrive/Desktop/emu/third_party/chips/LICENSE.txt>).
