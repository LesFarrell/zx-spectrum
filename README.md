# ZX Spectrum Emulator

Small ZX Spectrum emulator in C for Windows. It uses the Win32 API for display and input and currently builds through `build.bat` with `zig rc` and `zig cc`.

## Current scope

- 48K model
- 128K model with ROM switching and RAM paging through port `0x7FFD`
- ULA screen rendering with border and flash attributes
- Keyboard matrix input
- 48K beeper sound through the Windows audio device
- 128K AY sound through the Windows audio device

## Not implemented

- Tape loading
- Accurate ULA contention timing
- `.sna` snapshot loading

## Build

```powershell
.\build.bat
```

## Run

With no arguments, the emulator looks for `128.rom` in the working folder first and starts in 128K mode. If that is not present, it falls back to `48.rom`.

```powershell
.\build\zxspecemu.exe
```

48K:

```powershell
.\build\zxspecemu.exe --48 path\to\48k.rom
```

128K with two 16 KB ROMs:

```powershell
.\build\zxspecemu.exe --128 path\to\128-0.rom path\to\128-1.rom
```

128K with one combined 32 KB ROM:

```powershell
.\build\zxspecemu.exe --128 path\to\128k-combined.rom
```

## Keyboard

- `Shift` maps to `CAPS SHIFT`
- `Ctrl` maps to `SYMBOL SHIFT`
- Raw key presses still drive the Spectrum matrix for held keys and games
- Printable keys use the active Windows keyboard layout, so symbols such as `"` follow the host layout
- `Input -> Paste Text` or `Ctrl+V` queues clipboard text into the emulator one Spectrum key tap at a time

## Menu

- `File -> Open .z80 Snapshot...` loads a ZX Spectrum `.z80` snapshot into the current emulator session
- `File -> Reset` resets the active machine
- `File -> Exit` closes the emulator
- `Machine -> 48K` / `Machine -> 128K` rebuilds the emulator for that model and remembers the choice between runs
- `Input -> Paste Text` types clipboard text through the Spectrum keyboard matrix
- `Tools -> Assembler...` opens a small RAM patching assembler with support for common Z80 instructions plus `ORG`, `DB`, `DW`, and `INCLUDE`
- `Tools -> Debugger...` opens a separate debugger window with pause, run, single-step, register state, and memory/disassembly views

## Assembler Notes

- The built-in assembler is intentionally small: it supports labels and a practical subset of Z80 mnemonics, not a full macro assembler
- `File -> New` clears the current source after prompting to save when needed
- `INCLUDE "file.asm"` expands another source file in place during assembly, so it can appear in the middle of a source file; relative paths are resolved from the current source file
- `Ctrl+B` assembles the current source
- `Ctrl+A`, `Ctrl+Z`, `Ctrl+X`, `Ctrl+C`, and `Ctrl+V` work like a normal text editor inside the assembler source box
- Assembler writes are limited to RAM at `0x4000`-`0xFFFF`; ROM addresses are read-only in the running machine
- Working samples are included at [hello.asm](/C:/Users/Les%20Farrell/OneDrive/Desktop/emu/src/examples/hello.asm) and [include-main.asm](/C:/Users/Les%20Farrell/OneDrive/Desktop/emu/src/examples/include-main.asm)

## Text Entry Notes

- The Spectrum ROM still decides whether a key press means a BASIC keyword, a letter, or a symbol
- Queued text is most useful for symbols and line fragments once the ROM editor is already in the mode you expect
- BASIC keywords still follow normal Spectrum editing rules, so the first token on a line is not treated like a modern text field

## Notes

This is a deliberately simple emulator shell built around the vendored `chips` ZX Spectrum core under the zlib/libpng license. The license text is in [third_party/chips/LICENSE.txt](</C:/Users/Les Farrell/OneDrive/Desktop/emu/third_party/chips/LICENSE.txt>).
