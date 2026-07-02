# ZX Spectrum Emulator

Small ZX Spectrum emulator in C for Windows. It builds with GCC via CMake and uses only the Win32 API for display and input.

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
- Snapshot formats (`.z80`, `.sna`)

## Build

```powershell
cmake -S . -B build
cmake --build build
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

## Text Entry Notes

- The Spectrum ROM still decides whether a key press means a BASIC keyword, a letter, or a symbol
- Queued text is most useful for symbols and line fragments once the ROM editor is already in the mode you expect
- BASIC keywords still follow normal Spectrum editing rules, so the first token on a line is not treated like a modern text field

## Notes

This is a deliberately simple emulator shell built around the vendored `chips` ZX Spectrum core under the zlib/libpng license. The license text is in [third_party/chips/LICENSE.txt](</C:/Users/Les Farrell/OneDrive/Desktop/emu/third_party/chips/LICENSE.txt>).
