#!/usr/bin/env bash
# Every check that runs without hardware, in one go:
#   tools/check.sh            simulator build + host unit tests (ctest)
#   tools/check.sh --firmware also builds the ESP-IDF firmware
# Prints one line per stage and a final PASS or FAIL; full output goes to
# sim/build/check.log so a failure can be read without rerunning.
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
mkdir -p sim/build
log="sim/build/check.log"
: > "$log"
fail=0

stage() { # stage NAME COMMAND...
    local name="$1"; shift
    printf '%-24s' "$name"
    if "$@" >> "$log" 2>&1; then
        echo "ok"
    else
        echo "FAILED (see $log)"
        fail=1
    fi
}

stage "sim configure" cmake -S sim -B sim/build
stage "sim build" cmake --build sim/build -j 8
if [[ $fail -eq 0 ]]; then
    printf '%-24s' "unit tests"
    if ctest --test-dir sim/build --output-on-failure >> "$log" 2>&1; then
        echo "ok ($(grep -Eo 'out of [0-9]+' "$log" | tail -1 | grep -Eo '[0-9]+') tests)"
    else
        echo "FAILED"; grep -E "^ *[0-9]+/[0-9]+ Test" "$log" | grep -v Passed
        fail=1
    fi
fi

if [[ "${1:-}" == "--firmware" ]]; then
    # shellcheck disable=SC1091
    . "${IDF_PATH:-$HOME/esp/esp-idf-v5.5}/export.sh" >/dev/null 2>&1
    stage "firmware build" idf.py build
fi

if [[ $fail -eq 0 ]]; then echo "PASS"; else echo "FAIL"; fi
exit $fail
