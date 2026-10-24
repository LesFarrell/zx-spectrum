# ZX Spectrum Next

The emulator includes a ZX Spectrum Next model that boots NextZXOS from an SD
card image. It is a separate machine from the Sinclair models: its 2 MB of
RAM, MMU, Next registers, and SD card do not affect the classic machines or
their snapshots.

## Setting up NextZXOS

The Next model needs the files from the official NextZXOS emulator package.
Unpack the package into a `nextzxos` folder beside `zx-spectrum.exe`, either
directly or in a `sn-emulator-*` sub-folder:

```text
bin\
  zx-spectrum.exe
  nextzxos\
    tbblue_loader.rom
    sn-emulator-24.11\
      enNextZX.rom
      enNxtmmc.rom
      cspect-next-1gb.img
```

| File | Purpose |
| --- | --- |
| `enNextZX.rom` | NextZXOS ROM |
| `enNxtmmc.rom` | divMMC ROM |
| `tbblue_loader.rom` | 8 KB FPGA boot loader (GPL-3.0, see `tbblue_loader-NOTICE.md`) |
| `cspect-next-*.img` | SD card image; a `cspect-next-*-fixed.img` is used in preference when present |

Release numbers are not built into the emulator, so a newer package can be
dropped in without rebuilding. The SD image is opened read/write so NextZXOS
can save files; if it cannot be opened for writing it is used read-only.

`src\build.bat` does not copy these files. They stay in `bin\nextzxos` between
builds.

## Using the Next

- `Machine -> ZX Spectrum Next` or `Ctrl+6` boots NextZXOS. The title bar shows
  `ZX Spectrum Emulator (Next)`.
- Opening or dropping a `.nex` file switches to the Next and starts the
  program. NEX files can also be opened from inside a `.zip`.
- Opening a `.snx` snapshot starts it through NextZXOS, which keeps the file
  open on the SD card so the game can load more data. The same file must
  therefore be present on the SD image: files kept under a `nex-corpus\games`
  folder map to `/games/...` on the card. SNX files cannot be run from a ZIP.
- Programs can also be started from the NextZXOS Browser or command line, as
  on real hardware.
- `Ctrl+1` to `Ctrl+5` return to the Sinclair models. Opening a tape, disk,
  snapshot, or other Sinclair file while the Next is running also switches
  back to the last Sinclair model first.
- If the Next was running when the emulator closed, it boots the Next again
  on the next plain launch. Starting with a model flag, ROM path, or media file
  on the command line overrides this.
- Double-clicking a `.nex` or `.snx` file that is associated with
  `zx-spectrum.exe` starts it on the Next.

The keyboard is mapped as on the other models (`Shift` is CAPS SHIFT and
`Ctrl` is SYMBOL SHIFT). Keyboard joystick presets and XInput controllers
appear as a Kempston joystick on port `1F`. Pause, the 1x-8x speed settings,
mute, fullscreen, integer scaling, and scanlines all work on the Next.

These commands are classic-only and are greyed out on the Next: Save Snapshot,
Save Screen, Save Program/RAM To Tape, Play/Stop Tape, Save/Eject Disk,
Rewind, the external Multiface menu entries, and Poke. The Next has its own
Multiface, which NextZXOS uses itself.

## What is emulated

- **CPU**: Z80N with all Next extended instructions, at 3.5, 7, 14, or 28 MHz
  (NextReg `07`)
- **Memory**: 2 MB RAM in 8 KB pages through the eight-slot MMU, legacy
  `7FFD`/`1FFD`/`DFFD` paging, ROM selection, and Layer 2 memory mapping
- **Video**: ULA screen with the Next palettes and ULANext, Timex hi-colour
  and hi-res modes, LoRes, Layer 2 at 256x192, 320x256, and 640x256, the
  tilemap in 40- and 80-column modes, 128 hardware sprites, per-layer clip
  windows and scrolling, layer priority, and the Copper
- **Sound**: three AY chips (TurboSound) with per-chip stereo panning, the
  four 8-bit DACs, and the beeper, mixed in stereo
- **DMA**: the zxnDMA, including burst and continuous modes and the
  prescaler
- **Storage**: divMMC with automatic mapping and SD card access to the
  NextZXOS image
- **Other**: Multiface NMI (NextZXOS uses it to finish launching snapshots),
  stackless NMI, the real-time clock, and line interrupts

## Debugging Next programs

The debugger (`Ctrl+Shift+D`) works on the Next. It disassembles Z80N
instructions, shows the MMU slots, CPU speed, and layer state, and can break on
Next register values with `NR(n)` in a condition. See [debugger.md](debugger.md).

The built-in assembler accepts the Z80N instructions too; see
[assembler.md](assembler.md).

## Known limitations

- SNX snapshots only run when the matching file exists on the SD image (see
  above).
- Next snapshots cannot be saved, and rewind is not available on the Next.
- The FPGA bitstream itself is not emulated; the boot loader ROM stands in for
  it.
