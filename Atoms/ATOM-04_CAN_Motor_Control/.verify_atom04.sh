#!/usr/bin/env bash
# ATOM-04_CAN_Motor_Control 验证：严格告警 + aarch64 目标编译 + 结构边界
#
# 当前模块形态（2026-10-16 闭环控制版）：
#   can_motor_control.cpp —— 两个对外入口 + 独立接收线程 + 20ms PID 驱动
#   motor_frame_codec.cpp —— 控制帧大端序构建 + C620 反馈帧解析
#   pid.cpp               —— 位置式 PID（抗积分饱和、微分作用于测量值）
#
# 已移除的检查：具名用例执行。测试源码位于上层 Tests/，且现有测试针对的是已被
# 删除的旧公开面（CanCore/CanRuntimeConfig），无法编译；详见 PLAN.md §7.2。
set -u
P=/mnt/d/Project/C++/RM2027/Cocoon-Dart2027/NewVersion
M=$P/Atoms/ATOM-04_CAN_Motor_Control
INC="-I$P/Atoms/Public -I$M/Inc -I$M"
STD="-std=c++23"
WARN="-Wall -Wextra -Wpedantic -Werror"
STRICT="-Wshadow -Wconversion -Wsign-conversion -Wuseless-cast -Wdouble-promotion -Wformat=2 -Wnull-dereference -Wold-style-cast -Wcast-qual -Wswitch-enum -Wunused-const-variable=2"
OUT=/tmp/atom04_verify
mkdir -p "$OUT"
fail=0
pending=0

# 参与编译的翻译单元（其余 .cpp 必须只剩注释，等待物理删除）
SRC_LIST="motor_frame_codec pid can_motor_control"
KEEP_CPP="can_motor_control.cpp motor_frame_codec.cpp pid.cpp"
KEEP_HPP="can_motor_control.hpp motor_frame_codec.hpp pid.hpp"

echo "=== pass 1: 基础告警门 ==="
for f in $SRC_LIST; do
  if g++ $STD $WARN -O0 -fsyntax-only $INC "$M/Src/$f.cpp" 2>&1; then echo "  OK   $f.cpp"
  else echo "  FAIL $f.cpp"; fail=1; fi
done

echo "=== pass 2: 严格告警（任何输出即缺陷） ==="
for f in $SRC_LIST; do
  out=$(g++ $STD $WARN $STRICT -O0 -fsyntax-only $INC "$M/Src/$f.cpp" 2>&1)
  if [ -n "$out" ]; then echo "  FAIL $f.cpp"; echo "$out" | head -12; fail=1
  else echo "  OK   $f.cpp"; fi
done

echo "=== pass 3: aarch64 目标编译（编译证据，不是执行证据） ==="
if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
  for f in $SRC_LIST; do
    if aarch64-linux-gnu-g++ $STD $WARN -O2 -c $INC "$M/Src/$f.cpp" -o "$OUT/$f.o" 2>&1; then
      echo "  OK   aarch64 $f.o"
    else echo "  FAIL aarch64 $f.cpp"; fail=1; fi
  done
else
  echo "  SKIP 无 aarch64-linux-gnu-g++"
fi

echo "=== pass 4: 结构边界与对外面约束 ==="
if grep -rn 'main[[:space:]]*(' "$M/Src" "$M/Inc" 2>/dev/null; then
  echo "  FAIL 模块内出现 main()"; fail=1
else echo "  OK   模块内无 main()"; fi

if grep -rnE '#include[[:space:]]*"(AppCoordinator|RuntimeCoordinator|StateArbitration|ATOM-0[0-9]|ATOM-1[0-9])' \
     "$M/Inc" "$M/Src" 2>/dev/null; then
  echo "  FAIL 出现禁止的横向依赖"; fail=1
else echo "  OK   无横向 ATOM 依赖"; fi

if grep -q 'ATOM_MODULE_TARGET atom_04_can_motor_control' "$M/CMakeLists.txt"; then
  echo "  OK   CMake 目标名 = atom_04_can_motor_control"
else echo "  FAIL CMake 目标名不符"; fail=1; fi

if grep -q 'Marshland::PublicHeaders' "$M/CMakeLists.txt"; then
  echo "  OK   CMake 已链接 Marshland::PublicHeaders"
else echo "  FAIL CMake 未链接公共头"; fail=1; fi

# 对外契约必须声明在共享公共头，而不是模块目录内（防止其它 ATOM 误判为私有）
PUBHDR="$P/Atoms/Public/Motor.hpp"
HDR="$M/Inc/atom04_can/can_motor_control.hpp"

if [ -f "$PUBHDR" ] && grep -q 'start(' "$PUBHDR" && grep -q 'setRpm(' "$PUBHDR"; then
  echo "  OK   两个对外入口声明在 Atoms/Public/Motor.hpp"
else echo "  FAIL 对外入口未声明在公共契约头"; fail=1; fi

if grep -q 'struct MotorPidParams' "$PUBHDR" && grep -q 'struct MotorState' "$PUBHDR"; then
  echo "  OK   对外结构体声明在 Atoms/Public/Motor.hpp"
else echo "  FAIL 对外结构体未声明在公共契约头"; fail=1; fi

if grep -q 'class IMotorControl' "$PUBHDR"; then
  echo "  OK   公共端口 IMotorControl 存在"
else echo "  FAIL 公共端口 IMotorControl 缺失"; fail=1; fi

if grep -q 'Motor.hpp' "$P/Atoms/Public/Headers/CMakeLists.txt"; then
  echo "  OK   Motor.hpp 已登记进 Marshland::PublicHeaders"
else echo "  FAIL Motor.hpp 未登记进公共头目标"; fail=1; fi

# 单一真相：模块头不得重复声明公共契约类型
if grep -qE 'struct (MotorPidParams|MotorState)' "$HDR"; then
  echo "  FAIL 模块头重复声明了公共契约类型（应只 include Motor.hpp）"; fail=1
else echo "  OK   模块头未重复声明公共契约类型"; fi

# 实现类必须真的实现公共端口
if grep -q 'public motor::IMotorControl' "$HDR"; then
  echo "  OK   CanMotorControl 实现 marshland::motor::IMotorControl"
else echo "  FAIL CanMotorControl 未实现公共端口"; fail=1; fi

# 线程安全：跨线程共享量必须由 atomic 或 mutex 覆盖
if grep -q 'std::mutex' "$HDR" && grep -q 'std::atomic' "$HDR"; then
  echo "  OK   跨线程共享量有 atomic/mutex 保护"
else echo "  FAIL 缺少线程安全原语"; fail=1; fi

# 已移除能力不得回归 / 无游离文件残留
for f in "$M"/Src/*.cpp; do
  base=$(basename "$f")
  case " $KEEP_CPP " in *" $base "*) continue ;; esac
  if grep -qE '^[[:space:]]*[^[:space:]/]' "$f"; then
    echo "  FAIL $base 仍含代码（应只剩注释）"; fail=1
  else
    echo "  WARN $base 待物理删除（见 PLAN.md §7）"; pending=$((pending + 1))
  fi
done
for f in "$M"/Inc/atom04_can/*.hpp; do
  base=$(basename "$f")
  case " $KEEP_HPP " in *" $base "*) continue ;; esac
  if grep -qE '^[[:space:]]*[^[:space:]/]' "$f"; then
    echo "  FAIL $base 仍含代码（应只剩注释）"; fail=1
  else
    echo "  WARN $base 待物理删除（见 PLAN.md §7）"; pending=$((pending + 1))
  fi
done

echo "=== RESULT: fail=$fail pending_delete=$pending ==="
exit $fail
