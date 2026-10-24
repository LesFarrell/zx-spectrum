# ZX Spectrum Emulator

Small ZX Spectrum emulator in C for Windows. It uses the Win32 API for display, input, and audio, and currently builds with `zig rc` and `zig cc`.

It emulates the 48K, 128K, +2, +2A, and +3 Sinclair models and the ZX
Spectrum Next, and includes a debugger, an assembler, and a poke tool.

## Documentation

- [Spectrum Next](docs/spectrum-next.md) - setting up NextZXOS, running NEX
  and SNX files, and what the Next model emulates
- [Debugger](docs/debugger.md) - breakpoints, watchpoints, conditions,
  stepping, and the command box
- [Assembler](docs/assembler.md) - editor keys, syntax, directives, Z80N
  instructions, and the sample programs

## Current scope

- 48K model
- 128K model with ROM switching and RAM paging through port `0x7FFD`
- Spectrum +2 model with 128K-compatible paging and optional dedicated ROM
- Spectrum +2A model with four ROM banks, `0x7FFD`/`0x1FFD` paging, and
  all-RAM modes without the +3 floppy controller
- ZX Interface 1 on 48K/128K/+2 machines, with shadow-ROM paging and two
  read/write Microdrives using standard `.mdr` cartridge images
- Spectrum +3 model with four ROM banks, `0x7FFD`/`0x1FFD` paging,
  all-RAM modes, AY audio, and uPD765 floppy-controller emulation
- ULA screen rendering with border and flash attributes
- ULAplus 64-colour palette mode on ports `0xBF3B`/`0xFF3B`, including
  scanline palette changes, colour-register readback, and the optional Timex
  screen 1, hi-colour, and hi-resolution video modes through port `0xFF`
- Native CPU and frame timing: 3.5 MHz with 69,888 T-states per 48K frame,
  and 3.546894 MHz with 70,908 T-states per 128K-family frame
- Memory-side ULA contention timing for 48K and 128K RAM accesses
- Keyboard matrix input
- Selectable Kempston, Sinclair 1, Sinclair 2, and Cursor/Protek/AGF joystick
  interfaces for keyboard and XInput-compatible controller input, plus the
  active-low Fuller joystick interface on port `0x7F`
- Configurable keyboard joystick presets: cursor keys, WASD, or QAOP
- Optional Kempston mouse using the host mouse over the emulator display
- 48K beeper sound through the Windows audio device
- 128K AY sound through the Windows audio device
- Optional Fuller Audio Box AY on 48K machines, SpecDrum DAC on port `0xDF`,
  and Covox DAC on port `0xFB`
- Multiface 1, Multiface 128, and Multiface 3 ROM/RAM paging with software
  page ports, NMI button, lockout state, and 8 KB or 16 KB RAM modes
- ZX Spectrum Next with NextZXOS booting from an SD card image, Z80N CPU at
  3.5-28 MHz, 2 MB RAM and MMU, Layer 2, sprites, tilemap, Copper, zxnDMA,
  three AY chips, DACs, divMMC, and `.nex`/`.snx` loading; see
  [docs/spectrum-next.md](docs/spectrum-next.md)
- `.tap` tape loading
- `.tzx` v1.20 tape loading, including direct recording, RLE/Z-RLE CSW,
  generalized data, loop/jump/call flow, and interactive select blocks
- `.pzx` v1.x tape loading with pulse, data, pause, browse, extension, and
  conditional stop blocks, including fast loading for standard data blocks
- Standard and extended `.dsk` disk images for the Spectrum +3, including
  sector reads, catalogs, bootable disks, sector writes, and saving changes
  back to the source image
- `.z80` snapshot loading for 48K, 128K, +2, +2A, and +3 snapshots
- `.z80` snapshot saving for 48K, 128K, +2, +2A, and +3 machines
- `.sna` snapshot loading for 48K and 128K snapshots
- `.sna` snapshot saving for 48K, 128K, and +2 machines
- `.szx` ZX-State snapshot loading for 48K, 128K, +2, +2A, and +3 machines,
  including zlib-compressed RAM pages, paging, AY, ULAplus `PLTT`, Fuller AY,
  SpecDrum, Covox, and Multiface state
- Standard, ULAplus, Timex hi-colour, and Timex hi-resolution `.scr` screen
  loading and saving for the currently displayed RAM bank
- `.zip` archive loading for every supported tape, disk, Microdrive, screen,
  and snapshot format, with an entry picker when an archive contains several
  supported files
- Pause, selectable 1x/2x/4x/8x speed, borderless fullscreen, and centered
  integer display scaling
- A two-second in-memory rewind history sampled every 25 emulated frames
  (approximately 500 ms) on the Sinclair models
- A debugger with execution, memory read/write, and port breakpoints,
  conditions, hit counts, step over/out, and breakpoints saved between runs
- A Z80 and Z80N assembler that writes straight into emulated RAM
- Eight remembered recent-media paths

## Not implemented

- 128K SNA snapshots captured with the TR-DOS ROM paged in
- Floating-bus reads and selectable early/late ULA timing variants
- Pixel-accurate mid-scanline border and screen changes (the current renderer
  is scanline-granular)
- SZX machine variants outside the five currently emulated Sinclair models;
  unrelated peripheral blocks are skipped while core state is restored
- Creating or formatting DSK tracks
- Saving +2A/+3 snapshots as `.sna`; use the supported `.z80` writer instead
- Interface 1 RS-232 and Sinclair Network connections, and raw/unformatted
  Microdrive media (standard formatted `.mdr` images are supported)

See [docs/spectrum-next.md](docs/spectrum-next.md#known-limitations) for the
Next model's limitations.

## Build

```powershell
.\src\build.bat
```

The build writes the self-contained output to `.\bin`: `zx-spectrum.exe`,
`Scintilla.dll`, and a copy of the ROMs in `.\bin\roms`.

## Run

With no arguments, the emulator looks for `plus3.rom`, `plus2a.rom`,
`plus2.rom`, `128.rom`, and `48.rom` in `.\bin\roms`. It enables Interface 1 on
48K/128K/+2 machines when `Interface1-v2.rom` is present there. When dedicated +2
or +2A ROMs are absent, the 128K or +3 ROM set is used as a compatible
fallback. It restores the last selected model when its ROM is available, or the ZX
Spectrum Next if that was in use and NextZXOS is installed.

The ZX Spectrum Next needs the NextZXOS emulator package unpacked into
`.\bin\nextzxos`; see [docs/spectrum-next.md](docs/spectrum-next.md#setting-up-nextzxos).

Optional Multiface ROMs are discovered in the same folder as
`multiface1.rom`, `multiface128.rom`, and `multiface3.rom`. The matching image
must be exactly 8 KB; enable it from the Machine menu before pressing its NMI
button.

```powershell
.\bin\zx-spectrum.exe
```

48K:

```powershell
.\bin\zx-spectrum.exe --48 path\to\48k.rom
```

128K with two 16 KB ROMs:

```powershell
.\bin\zx-spectrum.exe --128 path\to\128-0.rom path\to\128-1.rom
```

128K with one combined 32 KB ROM:

```powershell
.\bin\zx-spectrum.exe --128 path\to\128k-combined.rom
```

Spectrum +2 with one combined 32 KB ROM:

```powershell
.\bin\zx-spectrum.exe --plus2 path\to\plus2.rom
```

Spectrum +2A with one combined 64 KB ROM:

```powershell
.\bin\zx-spectrum.exe --plus2a path\to\plus2a.rom
```

Spectrum +3 with one combined 64 KB ROM:

```powershell
.\bin\zx-spectrum.exe --plus3 path\to\plus3.rom
```

## Keyboard

- `Shift` maps to `CAPS SHIFT`
- `Ctrl` maps to `SYMBOL SHIFT`
- Raw key presses still drive the Spectrum matrix for held keys and games
- Printable keys use the active Windows keyboard layout, so symbols such as `"` follow the host layout
- `Input -> Keyboard Joystick` selects no keyboard joystick, cursor keys plus
  Space, WASD plus Space, or QAOP plus M
- `Input -> Joystick Interface` selects Kempston, Sinclair 1, Sinclair 2,
  Cursor/Protek/AGF, or Fuller; both keyboard presets and controllers use it
- `Input -> Kempston Mouse` exposes host movement and three buttons through
  ports `FBDF`, `FFDF`, and `FADF`; input selections are remembered

## Controller

- An XInput-compatible pad is exposed through the selected joystick interface
- D-pad or left stick maps to directions
- `A`, `B`, `X`, `Y`, shoulders, or triggers map to fire

## Menu

- `File -> Open Media/Snapshot...` opens `.nex`, `.snx`, `.zip`, `.tap`, `.tzx`, `.pzx`, `.mdr`, `.dsk`, `.scr`, `.z80`, `.sna`, or `.szx` files
- Opening a `.nex` or `.snx` file switches to the ZX Spectrum Next and starts it
- `File -> Save Snapshot...` writes every supported model as `.z80`, or writes
  48K/128K/+2 machines as `.sna` when that extension is selected
- `File -> Save Screen...` writes the visible Spectrum screen as a 6912-byte `.scr`
- `File -> Recent Media` remembers the eight most recently opened media files
- Drag one supported media, screen, archive, or snapshot file onto the emulator
  window to load it through the same media handling
- ZIP archives support stored and deflated entries. Disk and Microdrive images
  opened from an archive are mounted read-only so the archive cannot be
  overwritten accidentally. Multi-disk, encrypted, ZIP64-sized, and
  compression methods other than stored/Deflate are rejected with a clear
  error.
- Opening a `.dsk` inserts it as +3 drive `A:`, switches to the +3 model,
  and starts the ROM Loader so bootable disks run automatically
- `Disk -> Save Disk` writes sector changes back to the inserted DSK
- `Disk -> Eject Disk` offers to save a modified disk before removing it
- `Microdrive -> Drive 1` and `Drive 2` insert/eject `.mdr` cartridges
  and controls their write-protect state. `New Empty Cartridge...` creates a
  formatted 254-sector image whose cartridge label comes from the filename;
  changed cartridges can be saved explicitly or on eject/exit
- Opening or dropping an `.mdr` inserts it into the first free Microdrive. If
  +2A or +3 is active, the emulator switches to an Interface 1-compatible model.
- `Tape -> Auto-load Tapes On Open` toggles whether opening a tape starts loading automatically
- With auto-load on, opening a tape inspects the tape and chooses `48 BASIC` or the `128K` tape loader automatically
- With auto-load off, opening a tape just inserts and rewinds it for manual loading
- Tape and snapshot files are read in the background so opening larger `.tap`,
  `.tzx`, `.pzx`, `.z80`, `.sna`, or `.szx` files does not stall the main window
- Standard ROM `LOAD ""` operations fast-load automatically for `.tap`,
  standard-block `.tzx`, and standard-data `.pzx` files
- `Tape -> Use Fast Tape Loading` runs custom and turbo tape loaders as quickly as the host CPU allows, without generating intermediate audio or frames; it is enabled by default
- Use `F3` or `Tape -> Play Tape` only for custom loaders or real-time tape playback
- Press `F4` or use `Tape -> Stop Tape` to stop real-time playback
- `File -> Reset` resets the active machine
- `File -> Save Program To Tape...` writes the current BASIC program as a
  standard two-block `.tap` image
- `File -> Save RAM To Tape...` writes the visible `0x4000`-`0xFFFF` RAM range
  as a CODE block in a `.tap` image
- `File -> Exit` closes the emulator
- `Machine` switches among 48K, 128K, +3, +2, +2A, and ZX Spectrum Next. The
  machine in use when the emulator closes is started again next time; a model
  flag, ROM path, or media file on the command line takes precedence
- A `.tap` file opened from Explorer or the command line starts on the 128K
  (when `128.rom` is available) so the 128K Tape Loader runs it; this does not
  change the saved model. A model flag such as `--48` overrides it
- `Ctrl+1` through `Ctrl+6` select 48K, 128K, +3, +2, +2A, and the Next
- `Machine -> Speed` selects 1x, 2x, 4x, or 8x and remembers the selection
- `F2` pauses and `F6` cycles through the speed choices
- `F8` or `Machine -> Rewind 2 Seconds` rewinds two emulated seconds, or to
  the oldest available half-second checkpoint when less history exists
- `F11` toggles fullscreen; `Escape` leaves fullscreen
- `View -> Integer Scaling` keeps the image at a centered whole-number scale
- `View -> Scanlines` applies a lightweight alternating-line display filter;
  the setting is remembered between runs
- `Sound -> Mute Sound` or `Ctrl+M` toggles all emulator audio and remembers the choice between runs
- The Sound menu also enables Fuller AY, SpecDrum, and Covox; the Machine menu
  attaches the matching Multiface and `Alt+F5` presses its NMI button
- `Tools -> Assembler...` (`Ctrl+Shift+A`) opens a small RAM patching
  assembler; see [docs/assembler.md](docs/assembler.md)
- `Tools -> Debugger...` (`Ctrl+Shift+D`) opens the debugger on any model,
  including the Next; see [docs/debugger.md](docs/debugger.md)
- `Tools -> Poke...` (`Ctrl+Shift+P`) opens a small RAM poke tool for writing
  one or more byte values directly to memory

## Settings

Settings are stored in `%APPDATA%\zxspecemu.ini`: the selected machine, speed,
mute, input choices, tape options, recent media, and debugger breakpoints.
Delete the file to return to the defaults.

## Notes

This is a deliberately simple emulator shell built around the vendored `chips`
ZX Spectrum core under the zlib/libpng license. The license text is in
[third_party/chips/LICENSE.txt](third_party/chips/LICENSE.txt).
