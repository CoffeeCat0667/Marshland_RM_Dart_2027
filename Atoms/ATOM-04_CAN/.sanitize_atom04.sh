#!/usr/bin/env bash
set -u
P=/mnt/d/Project/C++/RM2027/Cocoon-Dart2027/NewVersion
M=$P/Atoms/ATOM-04_CAN
OUT=/tmp/atom04_san; mkdir -p "$OUT"
CASES="can.slot_contract can.big_endian_codec can.control_frame can.zero_torque_frame can.receive_validation can.node_state_transitions can.snapshot_semantics can.config_validation can.torque_output_gating"
SRCS="$P/Tests/Atom04CanTests.cpp $M/Src/motor_frame_codec.cpp $M/Src/can_config.cpp $M/Src/protocol_dispatcher.cpp $M/Src/can_core.cpp"

echo "=== ASan + UBSan（全部 9 个用例） ==="
if g++ -std=c++23 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread -I$P/Atoms/Public -I$M/Inc $SRCS -o "$OUT/asan" 2>&1 | head -20; then
  bad=0
  for c in $CASES; do
    if ASAN_OPTIONS=detect_leaks=1 "$OUT/asan" "$c" >/dev/null 2>&1; then echo "  OK   $c"; else echo "  FAIL $c"; ASAN_OPTIONS=detect_leaks=1 "$OUT/asan" "$c" 2>&1 | tail -12; bad=1; fi
  done
  echo "  ASAN/UBSAN 失败项: $bad"
else
  echo "  BUILD FAIL"
fi
