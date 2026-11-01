# Scintilla

This directory vendors the 64-bit Windows Scintilla 5.6.4 runtime and the C API
headers used by the assembler editor.

- Upstream: https://www.scintilla.org/
- Vendored runtime: `bin/Scintilla.dll`
- Redistribution destination: `redist/Scintilla.dll`
- Headers: `include/Scintilla.h` and `include/Sci_Position.h`
- License: `LICENSE.txt`

`src/build.bat` copies the runtime into `redist` beside `zx-spectrum.exe` after
a successful build.
