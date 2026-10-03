#!/usr/bin/env bash
# fw.sh — build (and optionally flash) one benchmark firmware.
#
#   tools/fw.sh <esp32db|flashdb|sqlite|all> [flash <port>] [monitor]
#
# Each engine builds in its own directory (build/bench-<engine>) from
# sdkconfig.defaults + bench/sdkconfig.bench + bench/sdkconfig.<engine>, so the
# boards differ only in the engine. Your normal `idf.py build` (tracked
# sdkconfig) is untouched.
#
# Examples:
#   tools/fw.sh all                              # build all three
#   tools/fw.sh flashdb flash /dev/ttyUSB1       # build + flash one board
#   tools/fw.sh esp32db flash /dev/ttyUSB0 monitor

# Sourced, the `exit`s and `set -e` below would close the caller's terminal.
if (return 0 2>/dev/null); then
    echo "run tools/fw.sh directly, don't source it (it loads the IDF env itself)" >&2
    return 1
fi
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINES=(esp32db flashdb sqlite)

usage() { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
[[ $# -ge 1 ]] || usage

if [[ -z "${IDF_PATH:-}" ]]; then
    IDF_EXPORT="${IDF_EXPORT:-$HOME/Work/esp-idf/export.sh}"
    # shellcheck disable=SC1090
    . "$IDF_EXPORT" >/dev/null
fi

engine="$1"; shift
if [[ "$engine" == all ]]; then
    targets=("${ENGINES[@]}")
else
    [[ " ${ENGINES[*]} " == *" $engine "* ]] || usage
    targets=("$engine")
fi

actions=(build)
port_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        flash) [[ $# -ge 2 ]] || usage; port_args=(-p "$2"); actions+=(flash); shift 2 ;;
        monitor) actions+=(monitor); shift ;;
        *) usage ;;
    esac
done
if [[ ${#targets[@]} -gt 1 && ${#port_args[@]} -gt 0 ]]; then
    echo "flash one engine at a time (each board gets its own port)" >&2
    exit 1
fi

for e in "${targets[@]}"; do
    if [[ "$e" != esp32db ]]; then
        "$ROOT/tools/fetch_third_party.sh" >/dev/null
    fi
    bdir="$ROOT/build/bench-$e"
    mkdir -p "$bdir"
    echo "== $e -> $bdir"
    (cd "$ROOT" && idf.py -B "$bdir" \
        -D SDKCONFIG="$bdir/sdkconfig" \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;bench/sdkconfig.bench;bench/sdkconfig.$e" \
        "${port_args[@]}" "${actions[@]}")
done
