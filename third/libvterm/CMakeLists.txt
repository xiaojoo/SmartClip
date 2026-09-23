# libvterm 0.3.3（2023-08-04 快照）—— 内置编译（vendored）
#
# 这是什么：一个纯 C99 的 VT220/xterm 终端仿真内核（解析转义序列 + 维护字符网格），
# Neovim 的内嵌终端就是用它。底部终端面板（src/TerminalEngine.cpp）只负责
# "把字节喂进来、把网格画出去"，转义序列那一整套全在这儿。
#
# 为什么内置而不是找现成包：
#   1) 官方只发 Makefile + libtool，没有 CMakeLists，接进本工程必须自己写一份；
#   2) 官方 tarball 里 unicode 表（fullwidth.inc）和编码表（encoding/*.inc）已经
#      预生成了，所以**不需要**它 Makefile 里那个 bin2array 生成步骤 —— 直接编
#      src/*.c 就够，这也是内置比外部依赖省事的原因；
#   3) 版本要钉死：换一版终端行为就可能变（宽字符、备用屏、查询应答），
#      内置能保证每台机器编出来一样。
#
# 为什么不用 qtermwidget：它只有 Unix PTY（README 写的兼容范围是 BSD/Linux/OS X），
# Windows 上编不过；Qt Creator 自带的那套是 GPL-3.0 且绑死 Core/ProjectExplorer，搬不动。
#
# MSVC 编译要点（实测）：
#   * 根 CMakeLists 的 project() 只声明了 CXX，这里必须 enable_language(C)，
#     否则报 "no CMAKE_C_COMPILER could be found"。
#   * parser.c 里的 strncpy 会刷 C4996（"consider using strncpy_s"）——
#     那是它的内部实现，改不得，只能关这个警告，不要用 /wd 之外的全局抑制。
#   * 表文件是 .inc（不是 .c），必须一起列进目标，Qt Creator 的文件视图才看得到；
#     它们由 .c 里的 #include 拉进来，不会重复编译。
cmake_minimum_required(VERSION 3.21)

enable_language(C)

file(GLOB LIBVTERM_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
file(GLOB LIBVTERM_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")
file(GLOB LIBVTERM_TABLES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/encoding/*.inc")

add_library(libvterm STATIC ${LIBVTERM_SOURCES} ${LIBVTERM_HEADERS} ${LIBVTERM_TABLES})

# 只暴露 include/：src/vterm_internal.h 是它自己的私有头，本工程不该碰
target_include_directories(libvterm PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_definitions(libvterm PRIVATE _CRT_SECURE_NO_WARNINGS)

set_target_properties(libvterm PROPERTIES FOLDER third)
