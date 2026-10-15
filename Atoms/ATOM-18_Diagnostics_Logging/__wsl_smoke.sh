#!/bin/bash
set -e
ATOM="/mnt/d/Project/C++/RM2027/Cocoon-Dart2027/NewVersion/Atoms/ATOM-18_Diagnostics_Logging"
ROOT="/mnt/d/Project/C++/RM2027/Cocoon-Dart2027/NewVersion"
rm -rf /tmp/atom18_logs
aarch64-linux-gnu-g++ -std=c++23 -Wall -Wextra -Wpedantic \
  -I "$ATOM/Inc" -I "$ROOT/Atoms/Public" -I "$ROOT/ThirdParty" \
  "$ATOM/__smoke.cpp" \
  /tmp/marshland_cfg/Atoms/ATOM-18_Diagnostics_Logging/libatom_18_diagnostics_logging.a \
  -pthread -o /tmp/atom18_smoke
QEMU=$(command -v qemu-aarch64 || command -v qemu-aarch64-static || true)
if [ -z "$QEMU" ]; then echo "QEMU_NOT_FOUND"; exit 2; fi
"$QEMU" -L /usr/aarch64-linux-gnu /tmp/atom18_smoke
echo "--- log files ---"
ls -la /tmp/atom18_logs/
echo "--- newest file head ---"
head -3 "$(ls -t /tmp/atom18_logs/*.jsonl | head -1)"
echo "--- oldest file head ---"
head -2 "$(ls -tr /tmp/atom18_logs/*.jsonl | head -1)"
