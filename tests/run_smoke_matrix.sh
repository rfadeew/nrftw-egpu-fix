#!/usr/bin/env bash
# Release check: smoke test for Measure 0/1 x VidmemHeaps 0/1, the "break" negative
# test, and a run without egpu_fix.ini (code defaults). Needs build/version.dll and
# build/smoke.exe (build.bat, tests/build_smoke.bat) and an NVIDIA GPU. Close the game first.
# Usage (Git Bash, from anywhere): tests/run_smoke_matrix.sh [out_dir]
set -u
cd "$(dirname "$0")/.."
OUT="${1:-build/smoke_logs}"
mkdir -p "$OUT"
INI=build/egpu_fix.ini
FAIL=0

make_ini() { # $1 = Measure, $2 = VidmemHeaps
    sed -e "s/^Measure=.*/Measure=$1/" -e "s/^VidmemHeaps=.*/VidmemHeaps=$2/" tests/egpu_fix.dev.ini > "$INI"
}

run() { # $1 = name, $2 = expected (pass|fail), rest = smoke args
    local name="$1" want="$2"; shift 2
    build/smoke.exe "$@" > "$OUT/$name.out" 2>&1
    local rc=$?
    cp build/egpu_proxy.log "$OUT/$name.log" 2>/dev/null
    local got=pass; [ $rc -ne 0 ] && got=fail
    local verdict=OK; [ "$got" != "$want" ] && { verdict=WRONG; FAIL=1; }
    printf '%-14s exit %-3s expected %-4s -> %-5s | %s | %s\n' "$name" "$rc" "$want" "$verdict" \
        "$(grep -m1 -o 'stage 2c ON[^:]*\|stage 2c off[^,]*\|Vidmem: OFF ([^)]*)' "$OUT/$name.log")" \
        "$(grep -c -- '-> VRAM' "$OUT/$name.log") heaps -> VRAM, $(grep -c 'SYSMEM' "$OUT/$name.log") SYSMEM"
}

for m in 0 1; do
    for v in 0 1; do
        make_ini $m $v
        run "m${m}_v${v}" pass 60 90 60
    done
    make_ini $m 1
    run "m${m}_v1_break" fail 30 30 30 break
done

rm -f "$INI"
run "no_ini" pass 60 90 60
grep -m1 'config:' "$OUT/no_ini.log" | sed 's/^.*config:/no_ini config:/'

# The log must not contain local paths either (users attach it publicly).
if grep -l -i -E '\\Users\\|SteamLibrary' "$OUT"/*.log; then echo "LOCAL PATH IN LOG"; FAIL=1; fi

echo; [ $FAIL -eq 0 ] && echo "MATRIX PASSED" || echo "MATRIX FAILED"
exit $FAIL
