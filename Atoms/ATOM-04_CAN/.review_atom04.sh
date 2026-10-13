#!/usr/bin/env bash
set -u
P=/mnt/d/Project/C++/RM2027/Cocoon-Dart2027/NewVersion
M=$P/Atoms/ATOM-04_CAN
INC="-I$P/Atoms/Public -I$M/Inc -I$M"
OUT=/tmp/atom04_review; mkdir -p "$OUT"
STRICT="-std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wuseless-cast -Wdouble-promotion -Wformat=2 -Wnull-dereference -Wunused-const-variable=2 -Wold-style-cast -Wcast-qual -Wswitch-enum"

echo "=== A. 更严格告警（任何输出即为缺陷） ==="
warn_found=0
for f in motor_frame_codec can_config protocol_dispatcher can_core lifecycle_adapter socketcan_transport; do
  out=$(g++ $STRICT -O0 -fsyntax-only $INC "$M/Src/$f.cpp" 2>&1)
  if [ -n "$out" ]; then echo "--- $f.cpp"; echo "$out" | head -20; warn_found=1; else echo "  OK   $f.cpp"; fi
done
echo "  严格告警缺陷: $warn_found"

echo "=== B. ASan + UBSan 全量测试（含 CanCore 线程） ==="
SRCS="$M/Tests/Atom04CanTests.cpp $M/Src/motor_frame_codec.cpp $M/Src/can_config.cpp $M/Src/protocol_dispatcher.cpp $M/Src/can_core.cpp"
if g++ -std=c++23 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread $INC $SRCS -o "$OUT/asan" 2>&1 | head -20; then
  if ASAN_OPTIONS=detect_leaks=1 "$OUT/asan"; then echo "  ASAN/UBSAN OK"; else echo "  ASAN/UBSAN FAIL"; fi
fi

echo "=== C. TSan（线程竞争检测，若可用） ==="
if g++ -std=c++23 -O1 -g -fsanitize=thread -pthread $INC $SRCS -o "$OUT/tsan" 2>&1 | head -10; then
  if "$OUT/tsan" 2>&1 | tail -25; then echo "  TSAN RUN DONE"; fi
fi
