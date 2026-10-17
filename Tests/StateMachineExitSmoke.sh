#!/usr/bin/env bash
set -euo pipefail

binary=${1:-cmake-build-ubuntu-arm/atom02-check/entry_smoke}
runner=${QEMU_AARCH64:-qemu-aarch64}
sysroot=${ARM_SYSROOT:-/usr/aarch64-linux-gnu}
log=$(mktemp)
pid=
cleanup() {
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -f -- "$log"
}
trap cleanup EXIT

for signal in INT TERM; do
    "$runner" -L "$sysroot" "$binary" >"$log" 2>&1 &
    pid=$!
    ready=false
    for ((i=0; i<250; ++i)); do
        if grep -qF '[Main] Press Ctrl+C to exit' "$log"; then
            ready=true
            break
        fi
        sleep 0.02
    done
    if [[ "$ready" != true ]]; then
        cat "$log"
        echo "FAIL entry did not start" >&2
        exit 1
    fi
    kill -s "$signal" "$pid"
    stopped=false
    for ((i=0; i<250; ++i)); do
        if ! kill -0 "$pid" 2>/dev/null; then
            stopped=true
            break
        fi
        sleep 0.02
    done
    if [[ "$stopped" != true ]]; then
        cat "$log"
        echo "FAIL entry did not stop after SIG$signal" >&2
        exit 1
    fi
    wait "$pid"
    pid=
    grep -qF '[Main] Received signal' "$log"
    grep -qF '[Runtime] All threads stopped' "$log"
    grep -qF '[Main] Exit' "$log"
    echo "PASS entry SIG$signal shutdown and thread joins"
done
