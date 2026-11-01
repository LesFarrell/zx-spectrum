#!/bin/sh
# Builds the harness with build.bat's flags plus -g. The Next RTC is compiled
# against a fixed clock so Next runs are repeatable.
cd "$(dirname "$0")/../../src" || exit 1
zig cc -std=c11 -O2 -g -c -D_time64=claude_fixed_time -I. next_rtc.c -o ../tmp/claude_perf/next_rtc_fixed.o || exit 1
zig cc -std=c11 -O2 -g $EXTRA -I. -I../third_party/chips -I../third_party/scintilla/include ../tmp/claude_perf/perf_test.c ../tmp/claude_perf/next_rtc_fixed.o file_util.c spectrum.c next_machine.c next_dma.c next_copper.c next_sprite.c next_sd.c szx_inflate.c zip.c disk.c microdrive.c tape.c -lgdi32 -lcomdlg32 -lcomctl32 -lshell32 -luser32 -lwinmm -lole32 -lshlwapi -ldbghelp -o ../tmp/claude_perf/perf.exe 2>&1 | grep -E "error|warning: (unused|implicit)" | grep -v third_party
