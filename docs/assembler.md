# Assembler

`Tools -> Assembler...` (`Ctrl+Shift+A`) opens a Z80 assembler that writes
straight into the running machine's RAM. It is meant for patches, experiments,
and small programs rather than as a full macro assembler.

## Editor

| Key | Action |
| --- | --- |
| `Ctrl+N` | New source (prompts to save changes) |
| `Ctrl+O` | Load a source file |
| `Ctrl+R` or `F5` | Reload the current file from disk |
| `Ctrl+S` / `Ctrl+Shift+S` | Save / Save As |
| `Ctrl+Shift+T` | Export the assembled code as a `.tap` file |
| `Ctrl+B` | Assemble |
| `Ctrl+F5` | Assemble and run from the `ORG` address |
| `Ctrl+F` / `Ctrl+H` | Find / Find and Replace (match case, whole word, wraps around) |
| `Ctrl+Shift+F` | Choose the editor font |
| `Ctrl+Shift+L` | Format the source |
| `Ctrl+Shift+U` | Convert the source to upper case |
| `Ctrl+Z`, `Ctrl+X`, `Ctrl+C`, `Ctrl+V`, `Ctrl+A` | Usual editing keys |

The current file is watched for changes made in another editor, and the
assembler offers to reload it. A manual reload asks before discarding unsaved
changes.

## Source syntax

- One instruction or directive per line; `;` starts a comment.
- Labels end with `:` and can be used before they are defined.
- Plain numbers are decimal. Hex is written `$8000`, `0x8000`, or `8000h`,
  binary `%1010` or `1010b`, and a single
  character `'A'` or `"A"`. (The debugger is different: it reads plain
  numbers as hex.)
- `EQU` lines and constants are written `NAME EQU value`, without a colon.

### Directives

| Directive | Meaning |
| --- | --- |
| `ORG addr` | Set the assembly address |
| `name EQU value` | Define a constant |
| `DB a, b, "text"` | Emit bytes |
| `DW a, b` | Emit little-endian words |
| `DEFS count[, fill]` / `DS count[, fill]` | Emit `count` copies of `fill` (default 0) |
| `INCBIN "file.bin"` | Copy a binary file into memory |
| `INCLUDE "file.asm"` | Assemble another source file in place; relative paths start from the current file |

### Instructions

The standard Z80 set covers the instructions most programs need:

```text
ADC  ADD  AND  BIT  CALL CCF  CP   CPL  DAA  DEC  DI   DJNZ EI   EX   EXX
HALT IN   INC  JP   JR   LD   LDIR NEG  NOP  OR   OUT  POP  PUSH RES  RET
RLA  RLCA RRA  RRCA RST  SCF  SET  SUB  XOR
```

`IX`, `IY`, and their halves (`IXH`/`IXL`/`IYH`/`IYL`, also written `XH`,
`HX`, `LX`, and so on) are accepted. The following are **not** supported yet:
`SBC`, `IM`, `RETI`, `RETN`, the `CB` rotates and shifts (`RL`, `RR`, `RLC`,
`RRC`, `SLA`, `SRA`, `SRL`), `RLD`/`RRD`, and the block instructions other
than `LDIR` (`LDI`, `LDD`, `LDDR`, `CPI`, `CPIR`, `INI`, `OUTI`, `OTIR`, ...).
Use `DB` to emit them as bytes if you need them.

### Spectrum Next (Z80N) instructions

The assembler also accepts the Z80N extensions. They assemble on every model
but only do something on the ZX Spectrum Next:

| Instruction | Notes |
| --- | --- |
| `NEXTREG reg,value` / `NEXTREG reg,A` | Write a Next register |
| `MUL D,E` | `DE = D * E` |
| `ADD HL,A` / `ADD DE,A` / `ADD BC,A` | |
| `ADD HL,nn` / `ADD DE,nn` / `ADD BC,nn` | |
| `PUSH nn` | Push an immediate value |
| `SWAPNIB`, `MIRROR A`, `TEST n` | |
| `BSLA DE,B`, `BSRA DE,B`, `BSRL DE,B`, `BSRF DE,B`, `BRLC DE,B` | Barrel shifts |
| `PIXELDN`, `PIXELAD`, `SETAE` | Screen address helpers |
| `OUTINB`, `JP (C)` | |
| `LDIX`, `LDWS`, `LDDX`, `LDIRX`, `LDPIRX`, `LDDRX` | Block copies |

Implied operands may be left out (`SWAPNIB`, `MUL`, `BSLA`), as most Next
assemblers allow.

## Where code goes

Assembled bytes are written to the current machine's memory map. Only RAM at
`4000`-`FFFF` can be written; ROM addresses are rejected. On the Next the
bytes go to whatever 8 KB pages are mapped into those slots.

`Export TAP...` writes the output as a single standard `CODE` block, so the
program must assemble to one contiguous range; sources that use several
`ORG`s cannot be exported in one block.

## Examples

Working samples are in [`src/examples`](../src/examples):

- [hello.asm](../src/examples/hello.asm) - a minimal program
- [include-main.asm](../src/examples/include-main.asm) - uses `INCLUDE`
- [equ-demo.asm](../src/examples/equ-demo.asm) - constants with `EQU`
- [space-invaders.asm](../src/examples/space-invaders.asm) - a complete 48K
  game

### Space Invaders

Open `space-invaders.asm` and press `Ctrl+F5` to assemble it at `8000h` and
start it.

- `5` and `8` move, `0` fires.
- A three-row animated alien formation, aimed bombs, four destructible
  shields, score, lives, and win/game-over screens.
- Eleven custom 8x8 UDGs, coloured attribute rows, and beeper effects for
  firing, hits, alien movement, and explosions.
