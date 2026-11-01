#!/bin/sh
# Rebuilds the harness, runs the scenarios (optional filter) and compares
# every hash with the golden run.
root="$(cd "$(dirname "$0")/../.." && pwd)"
sh "$root/tmp/claude_perf/build.sh"
cd "$root/bin" || exit 1
../tmp/claude_perf/perf.exe "$@" > ../tmp/claude_perf/last.txt 2>&1
awk 'NR==FNR { split($0, a, "  frames"); golden[a[1]] = $NF; next }
     { split($0, a, "  frames"); name = a[1]; h = $NF; if ($0 ~ /NONDET/) h = $(NF-1);
       status = (golden[name] == h) ? "same" : "CHANGED (golden " golden[name] ")";
       printf "%s  %s\n", $0, status }' ../tmp/claude_perf/golden.txt ../tmp/claude_perf/last.txt
