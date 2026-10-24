# Debugger

Open the debugger with `Tools -> Debugger...` or `Ctrl+Shift+D`. It works on
every model, including the ZX Spectrum Next. The window stays open while the
machine runs; breakpoints are checked on every emulated instruction and memory
or port access while it is open.

## Layout

- **Toolbar** - Run/Pause, Step, Step Over, Refresh, the address box, Go,
  Run To, Run @, and the Sync PC check box on the first row; Toggle BP,
  Toggle WP, Remove Sel, PC, SP, Page -, Page +, and Step Out on the second.
- **Command box** - type a debugger command and press `Enter` or click
  **Do**. `Esc` clears the box.
- **Disassembly** - the arrow and blue line mark the current PC; a red circle
  marks an execution breakpoint.
- **Registers** - run state, model, main and alternate registers, `I`, `R`,
  interrupt mode, flags, and paging. On the Next this also shows the eight MMU
  slots, CPU speed, ROM and paging ports, layer state, divMMC, and Multiface.
  The last breakpoint hit is shown at the bottom.
- **Breakpoints & watchpoints** - every breakpoint with its number, `+`
  (enabled) or `-` (disabled), kind, address range, options, and hit count.
- **Memory** and **Stack** - hex and ASCII dumps around the view address and
  SP.

`F1` or `Help -> Debugger Help` shows a quick reference.

## Numbers

Addresses and values are **hexadecimal by default**: `8000`, `FE`, `5C3A`.
These forms are also accepted:

| Form | Meaning |
| --- | --- |
| `$8000`, `0x8000`, `8000h` | hexadecimal |
| `%10100000` | binary |
| `#32768` | decimal |

Hit counts (`hits N`) and breakpoint numbers (`del 2`) are decimal.

## Running and stepping

| Key | Button | Action |
| --- | --- | --- |
| `F5` | Run / Pause | Resume or pause the machine |
| `F11` | Step | Execute one instruction |
| `F10` | Step Over | Run a `CALL`, `RST`, `DJNZ`, or repeating block instruction (`LDIR`, `CPIR`, `INIR`, `OTIR`, …) to completion |
| `Shift+F11` | Step Out | Run until the current subroutine returns (SP rises above its value at the start) |
| `Ctrl+Enter` | Run To | Run until execution reaches the address in the address box |
| `Shift+Enter` | Run @ | Set PC to the address in the address box and resume |

Navigation: `Enter` or **Go** moves the view to the typed address, `Ctrl+P`
and `Ctrl+S` jump to PC and SP, and `PgUp`/`PgDn` page by 256 bytes. With
**Sync PC** checked the view follows PC while the machine runs.

## Breakpoints

There are five kinds of breakpoint:

| Command | Stops when |
| --- | --- |
| `bp RANGE` (or `bx`) | an instruction starting in RANGE is about to execute |
| `br RANGE` | memory in RANGE is read |
| `bw RANGE` | memory in RANGE is written |
| `bi PORT` | a port in the range is read with `IN` |
| `bo PORT` | a port in the range is written with `OUT` |

`RANGE` is one address (`8000`) or an inclusive range (`4000-57FF`). For
port breakpoints a range that stays within `00`-`FF` matches the **low byte**
of the port, so `bo FE` catches every ULA write whatever the high byte;
use a full 16-bit value (`bi 7FFE`) to match one exact port.

Memory and port breakpoints stop after the instruction that made the access
has finished, so the registers show its result. The Registers pane shows
what happened, for example `Hit #2 Write 4865 = 00 (PC 72F4)`,
where `PC` is the instruction that made the access.

### Options

Options follow the range, in any order:

| Option | Meaning |
| --- | --- |
| `=VAL` | Only stop when the byte read or written equals VAL (not for `bp`) |
| `if COND` | Only stop when the condition is true (non-zero); see below |
| `hits N` | Count matching hits and stop on the Nth one and every one after it |
| `off` | Create the breakpoint disabled |

Examples:

```text
bp 8000                         stop at 8000
bp 8003 if B=3                  stop at 8003 only when B is 3
bp 6000-7FFF                    stop anywhere in 6000-7FFF
bp 9C40 hits 10                 stop the tenth time 9C40 runs
bw 4000-57FF                    stop on any write to the screen bitmap
bw 5C3A =FF                     stop when FF is written to 5C3A
br 5C00 if VALUE>3              stop when a byte above 3 is read from 5C00
bo FE =04                       stop when the border is set to green
bi FE if (VALUE & 1F) != 1F     stop when a key is pressed during a keyboard read
bo 243B if (NR(07)&3)=3         Next: stop on a NextReg select while at 28 MHz
```

### Managing breakpoints

| Command | Action |
| --- | --- |
| `del N` / `del all` | Delete breakpoint N, or all of them |
| `en N` / `en all` | Enable |
| `dis N` / `dis all` | Disable |
| `reset N` / `reset all` | Reset the hit count to zero |

In the list, double-click an entry to enable or disable it, and press
`Delete` or click **Remove Sel** to remove the selected entry. `F9` (Toggle BP)
and `Ctrl+W` (Toggle WP) add or remove a plain execution breakpoint or write
watchpoint at the address in the address box.

Breakpoints are saved in the settings file (`%APPDATA%\zxspecemu.ini`,
section `[debugger_points]`) whenever they change and are restored the next
time the emulator starts. Hit counts start from zero each session.

## Conditions

A condition is an expression that is true when it is non-zero. It can use:

- **Registers**: `A`, `F`, `B`, `C`, `D`, `E`, `H`, `L`, `I`, `R`, `IXH`,
  `IXL`, `IYH`, `IYL`, `AF`, `BC`, `DE`, `HL`, `IX`, `IY`, `SP`, `PC`, and the
  alternate set `AF'`, `BC'`, `DE'`, `HL'`
- **Flags**: `ZF`, `CF`, `SF`, `HF`, `PF`, `NF` (each 0 or 1)
- **Access values**: `ADDR` is the address or port that was accessed and
  `VALUE` is the byte read or written (for execution breakpoints `ADDR` is the
  instruction address)
- **Functions**: `PEEK(addr)` reads a byte, `DPEEK(addr)` reads a
  little-endian word, and `NR(reg)` reads a Spectrum Next register
- **Operators**, C precedence: `!` `~` unary `-`; `*` `/` `%`; `+` `-`;
  `<<` `>>`; `<` `<=` `>` `>=`; `==` `!=` (a single `=` also means equals);
  `&`; `^`; `|`; `&&`; `||`; and parentheses

Bare numbers in conditions are hexadecimal like everywhere else, so
`HL>4000` compares with 4000h. A name that is not a register, such as `DE00`,
is read as a hex number. A condition that does not parse is rejected when the breakpoint is added. If
one stops parsing later (for example after editing the settings file) the
breakpoint stops every time so you can fix it.

## Changing registers and memory

These commands take effect immediately; pause first if you want the change to
apply at a known point:

```text
HL=4000               set a 16-bit register
A=PEEK(5C08)+1        any condition expression works as the value
PC=8000               move execution; the disassembly follows
SP=FF00
poke 8000 3E 01 C9    write bytes starting at 8000
```

Byte registers (`A`, `F`, `B`, `C`, `D`, `E`, `H`, `L`, `I`, `R`) take the
low 8 bits of the result; word registers (`AF`, `BC`, `DE`, `HL`, `IX`, `IY`,
`SP`, `PC`, `AF'`, `BC'`, `DE'`, `HL'`) take the low 16 bits. `poke` writes
through the current memory map, so ROM is not changed.

## Spectrum Next

On the Next the disassembler decodes the Z80N instructions (`NEXTREG`,
`MUL D,E`, `PUSH nn`, `ADD HL,A`, `LDIRX`, `PIXELAD`, `OUTINB`, and the rest),
and the memory views, `PEEK`, and `poke` go through the Next MMU, so they see
the 8K pages currently mapped into each slot. Use `NR(n)` in conditions to test
Next registers, for example `bp 8000 if NR(50)=1C`.
