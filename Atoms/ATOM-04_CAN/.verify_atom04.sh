#!/usr/bin/env bash
# ATOM-04_CAN 增量验证：语法/警告门 + ARM 目标编译 + 逻辑测试执行
set -u
P=/mnt/d/Project/C++/RM2027/Cocoon-Dart2027/NewVersion
M=$P/Atoms/ATOM-04_CAN
INC="-I$P/Atoms/Public -I$M/Inc -I$M"
WARN="-Wall -Wextra -Wpedantic -Werror"
STD="-std=c++23"
OUT=/tmp/atom04_verify
mkdir -p "$OUT"
fail=0

echo "=== pass 1: 语法/警告门（x86 宿主，js 纯逻辑单元） ==="
for f in motor_frame_codec can_config protocol_dispatcher can_core lifecycle_adapter; do
  if g++ $STD $WARN -O0 -fsyntax-only $INC "$M/Src/$f.cpp" 2>&1; then echo "  OK   $f.cpp"
  else echo "  FAIL $f.cpp"; fail=1; fi
done

echo "=== pass 2: SocketCAN 传输（linux/can 头） ==="
if g++ $STD $WARN -O0 -fsyntax-only $INC "$M/Src/socketcan_transport.cpp" 2>&1; then echo "  OK   socketcan_transport.cpp"
else echo "  FAIL socketcan_transport.cpp"; fail=1; fi

echo "=== pass 3: ARM (aarch64) 目标编译 ==="
if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
  for f in motor_frame_codec can_config protocol_dispatcher can_core lifecycle_adapter socketcan_transport; do
    if aarch64-linux-gnu-g++ $STD $WARN -O2 -c $INC "$M/Src/$f.cpp" -o "$OUT/$f.o" 2>&1; then echo "  OK   aarch64 $f.o"
    else echo "  FAIL aarch64 $f.cpp"; fail=1; fi
  done
else
  echo "  SKIP aarch64-linux-gnu-g++ 未安装（规则要求 ARM 目标；本次仅登记，不伪装为已通过）"
fi

echo "=== pass 4: 逻辑测试执行 ==="
SRCS="$M/Tests/Atom04CanTests.cpp $M/Src/motor_frame_codec.cpp $M/Src/can_config.cpp $M/Src/protocol_dispatcher.cpp $M/Src/can_core.cpp"
if g++ $STD $WARN -O1 -pthread $INC $SRCS -o "$OUT/atom04_tests" 2>&1; then
  echo "  build OK"
  if "$OUT/atom04_tests"; then echo "  RUN OK"; else echo "  RUN FAIL"; fail=1; fi
else
  echo "  BUILD FAIL"; fail=1
fi

echo "=== pass 5: 依赖方向检查（不得出现 ATOM 私有头 / AppCoordinator） ==="
if grep -rnE '#include[[:space:]]*"(AppCoordinator|RuntimeCoordinator|StateArbitration|ATOM-0[0-9]|ATOM-1[0-9])' "$M/Inc" "$M/Src" "$P/Atoms/Public/CanTypes.hpp" "$P/Atoms/Public/CanInterfaces.hpp" 2>/dev/null; then
  echo "  FAIL 出现禁止的横向依赖"; fail=1
else
  echo "  OK   无横向 ATOM 依赖"
fi
if grep -rn 'main[[:space:]]*(' "$M/Src" 2>/dev/null; then
  echo "  FAIL 源码中出现 main()"; fail=1
else
  echo "  OK   无 main()"
fi

echo "=== RESULT: fail=$fail ==="
exit $fail
