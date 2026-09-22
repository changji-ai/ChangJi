# 树莓派（64 位）交叉编译工具链。
#
# 阶段 0 就要跑通这个，不是为了现在部署，是为了尽早发现哪个依赖
# 在 ARM 上编不过。等写了两万行再发现，改起来的代价完全不同。
#
# 用法：
#   cmake -S . -B build-rpi \
#         -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-rpi.cmake \
#         -DCHANGJI_STATIC_RUNTIME=ON
#   cmake --build build-rpi -j
#
# 交叉编译器从哪来：
#   Debian/Ubuntu:  sudo apt install g++-aarch64-linux-gnu
#   Windows:        用 WSL2 里的 Debian 装上面那个包，别在原生 Windows 上折腾
#
# 也可以直接在树莓派上本机编译（不用这个文件），但 Pi 5 编 Crow 那几个
# 头文件很吃内存，8GB 的机器建议先加 swap，或者 -j2 限制并行度。

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Cortex-A76 是 Pi 5 的核。Pi 4 是 A72，用 -mcpu=cortex-a72。
# 不确定目标机型就把这行注释掉，用通用的 armv8-a。
set(CMAKE_C_FLAGS_INIT   "-mcpu=cortex-a76")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a76")

# 只在 sysroot 里找库，别把宿主机 x86 的库链进去
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
