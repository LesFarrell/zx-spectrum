# Scintilla

This directory vendors the 64-bit Windows Scintilla 5.6.4 runtime and the C API
headers used by the assembler editor.

- Upstream: https://www.scintilla.org/
- Runtime: `bin/Scintilla.dll`
- Headers: `include/Scintilla.h` and `include/Sci_Position.h`
- License: `LICENSE.txt`

`src/build.bat` copies the runtime beside `zx-spectrum.exe` after a successful
build.
