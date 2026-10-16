# Repository Guidelines

## Project Structure & Module Organization

This is a C11 Windows ZX Spectrum emulator. Core sources live in `src/`: `main.c` owns the Win32 application, `spectrum.c` wraps the emulation core, and `tape.c`, `disk.c`, `microdrive.c`, `zip.c`, and `szx_inflate.c` handle media formats. UI components such as `debugger.c`, `assembler_ui.c`, `assembler_core.c`, `poke.c`, and `crt.c` are included by `main.c` and share its static helpers; do not compile them as separate translation units. Resources are defined in `src/resources.rc` and `src/assets/`. ROMs and assembler/media samples are under `src/roms/` and `src/examples/`. Vendored dependencies live in `third_party/`; change them only when the emulator core requires it.

## Build, Test, and Development Commands

Run commands from the repository root in PowerShell:

```powershell
.\src\build.bat
.\bin\zx-spectrum.exe
git diff --check
```

`build.bat` compiles resources with `zig rc`, builds with `zig cc -std=c11 -O2 -Wall -Wextra`, and writes the self-contained output to `bin/` at the repository root: `zx-spectrum.exe`, `Scintilla.dll`, and a copy of the ROMs in `bin/roms/`. The executable expects ROMs in `bin/roms/` unless ROM paths are supplied on the command line. `bin/` is generated output and is not committed. There is currently no committed automated test suite; a warning-free build is the minimum validation.

## Coding Style & Naming Conventions

Use four-space indentation, C11 constructs, explicit-width integer types, and the brace style of the surrounding file. Functions and variables use `snake_case`; application helpers use the `app_` prefix, Spectrum wrapper APIs use `spectrum_`, and compile-time constants use uppercase prefixes such as `APP_` or `TAPE_`. Keep public declarations in the matching `.h` file. Prefer small, module-local `static` helpers and preserve ownership/error-buffer conventions already used by media loaders. No formatter is configured, so review diffs for accidental whitespace churn.

## Testing Guidelines

Build after every functional change. Manually exercise affected models and media types, including failure paths and file round trips. For UI or rendering changes, verify window resizing, fullscreen, and integer scaling. Temporary test harnesses must be removed before commit; keep permanent tests deterministic and name them `*_test.c`.

## Commit & Pull Request Guidelines

History uses short, plain-language summaries such as `bug fixes` and `screen scaling added`. Prefer a clearer imperative subject, for example `Fix TAP program header encoding`. Keep commits focused. Pull requests should describe behavior changes, list build/manual-test results, link relevant issues, and include screenshots for visible UI or rendering changes. Do not commit generated `.o`, `.pdb`, `.exe`, or sample output files.
