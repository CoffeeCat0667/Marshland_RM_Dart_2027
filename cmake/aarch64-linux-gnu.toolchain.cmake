# aarch64-linux-gnu.toolchain.cmake
#
# CMake toolchain file for cross-compiling Marshland for the Ubuntu ARM target
# host from WSL (x86_64) on this development machine.
#
# Why CMAKE_SYSTEM_NAME matters:
#   Setting CMAKE_SYSTEM_NAME switches CMake into cross-compiling mode. Without
#   it, pointing CMAKE_CXX_COMPILER at aarch64-linux-gnu-g++ is still treated as
#   a NATIVE build: CMAKE_SYSTEM_PROCESSOR stays x86_64, find_package/find_library
#   resolve x86 libraries, and linking fails. The architecture guard in the root
#   CMakeLists.txt rejects that case on purpose (AGENTS.md forbids x86 builds).
#
# Usage (CLion): Settings -> Build, Execution, Deployment -> CMake -> profile ->
# CMake options:
#   -DCMAKE_TOOLCHAIN_FILE=/mnt/d/Project/C++/RM2027/Cocoon-Dart2027/NewVersion/cmake/aarch64-linux-gnu.toolchain.cmake
# Use the WSL absolute path: CMake resolves a relative toolchain path against the
# build directory, not the source directory.
#
# Usage (command line inside WSL):
#   cmake -S "/mnt/d/Project/C++/RM2027/Cocoon-Dart2027/NewVersion" \
#         -B build-arm \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.toolchain.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Cross toolchain installed in WSL as Ubuntu packages
# (gcc-aarch64-linux-gnu / g++-aarch64-linux-gnu, version 13.3.0).
set(CMAKE_C_COMPILER   /usr/bin/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/aarch64-linux-gnu-g++)

# Debian/Ubuntu cross packages keep the target sysroot here; the compiler driver
# already searches it, this makes find_* behave consistently as well.
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)

# Look for target headers/libraries only inside the sysroot, but run build-time
# programs (protoc-like helpers, if ever added) on the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
