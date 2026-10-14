#!/usr/bin/env bash
# ATOM-04_CAN 增量验证：严格告警 + aarch64 交叉编译 + 具名用例执行 + 边界检查
set -u
P=/mnt/d/Project/C++/RM2027/Cocoon-Dart2027/NewVersion
M=$P/Atoms/ATOM-04_CAN
INC="-I$P/Atoms/Public -I$M/Inc -I$M"
STD="-std=c++23"
WARN="-Wall -Wextra -Wpedantic -Werror"
STRICT="-Wshadow -Wconversion -Wsign-conversion -Wuseless-cast -Wdouble-promotion -Wformat=2 -Wnull-dereference -Wold-style-cast -Wcast-qual -Wswitch-enum -Wunused-const-variable=2"
OUT=/tmp/atom04_verify; mkdir -p "$OUT"
fail=0

SRC_LIST="motor_frame_codec can_config protocol_dispatcher can_core lifecycle_adapter socketcan_transport"

echo "=== pass 1: 基础告警门 ==="
for f in $SRC_LIST; do
  if g++ $STD $WARN -O0 -fsyntax-only $INC "$M/Src/$f.cpp" 2>&1; then echo "  OK   $f.cpp"
  else echo "  FAIL $f.cpp"; fail=1; fi
done

echo "=== pass 2: 严格告警（任何输出即缺陷） ==="
for f in $SRC_LIST; do
  out=$(g++ $STD $WARN $STRICT -O0 -fsyntax-only $INC "$M/Src/$f.cpp" 2>&1)
  if [ -n "$out" ]; then echo "  FAIL $f.cpp"; echo "$out" | head -12; fail=1; else echo "  OK   $f.cpp"; fi
done

echo "=== pass 3: aarch64 目标编译 ==="
if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
  for f in $SRC_LIST; do
    if aarch64-linux-gnu-g++ $STD $WARN -O2 -c $INC "$M/Src/$f.cpp" -o "$OUT/$f.o" 2>&1; then echo "  OK   aarch64 $f.o"
    else echo "  FAIL aarch64 $f.cpp"; fail=1; fi
  done
else
  echo "  SKIP 无 aarch64-linux-gnu-g++"
fi

echo "=== pass 4: 具名用例执行（tests live in repo Tests/） ==="
TESTS="$P/Tests/Atom04CanTests.cpp"
SRCS="$TESTS $M/Src/motor_frame_codec.cpp $M/Src/can_config.cpp $M/Src/protocol_dispatcher.cpp $M/Src/can_core.cpp"
if g++ $STD $WARN -O1 -pthread -I$P/Atoms/Public -I$M/Inc $SRCS -o "$OUT/atom04_tests" 2>&1; then
  CASES="can.slot_contract can.big_endian_codec can.control_frame can.zero_torque_frame can.receive_validation can.node_state_transitions can.snapshot_semantics can.config_validation can.torque_output_gating"
  for c in $CASES; do
    if "$OUT/atom04_tests" "$c" >/dev/null 2>&1; then echo "  OK   $c"
    else echo "  FAIL $c"; "$OUT/atom04_tests" "$c" 2>&1 | head -3; fail=1; fi
  done
else
  echo "  BUILD FAIL"; fail=1
fi

echo "=== pass 5: 结构边界 ==="
if grep -rn 'main[[:space:]]*(' "$M/Src" "$M/Inc" 2>/dev/null; then echo "  FAIL 模块内出现 main()"; fail=1
else echo "  OK   模块内无 main()"; fi

if [ -d "$M/Tests" ]; then echo "  FAIL 模块内仍存在 Tests/ 目录"; fail=1
else echo "  OK   模块内无 Tests/ 目录"; fi

if grep -rnE '#include[[:space:]]*"(AppCoordinator|RuntimeCoordinator|StateArbitration|ATOM-0[0-9]|ATOM-1[0-9])' "$M/Inc" "$M/Src" "$P/Atoms/Public/CanTypes.hpp" "$P/Atoms/Public/CanInterfaces.hpp" 2>/dev/null; then
  echo "  FAIL 出现禁止的横向依赖"; fail=1
else echo "  OK   无横向 ATOM 依赖"; fi

if grep -q 'target_link_libraries' "$M/CMakeLists.txt" && grep -q 'Marshland::PublicHeaders' "$M/CMakeLists.txt"; then
  echo "  OK   CMake 已链接 Marshland::PublicHeaders"
else echo "  FAIL CMake 未链接 Marshland::PublicHeaders"; fail=1; fi

echo "=== RESULT: fail=$fail ==="
exit $fail
