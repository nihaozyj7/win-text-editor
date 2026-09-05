cmake_minimum_required(VERSION 3.24)
project(TextEditor CXX)

# 默认 Release：Debug（-g 调试信息、无优化未 strip）体积大且运行慢数倍；
# 下面的 -O2/-s/--gc-sections 瘦身选项只在 Release 生效
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译器 / 构建工具（MinGW-W64 位于非默认路径，需显式指定）
set(CMAKE_C_COMPILER   "C:/Users/Easecat/Application/mingw64/bin/gcc.exe")
set(CMAKE_CXX_COMPILER "C:/Users/Easecat/Application/mingw64/bin/g++.exe")
set(CMAKE_MAKE_PROGRAM "C:/Users/Easecat/Application/mingw64/bin/ninja.exe")

set(SRC_LIST
    src/main.cpp
    src/CEditorWindow.cpp
    src/CTextBuffer.cpp
    src/Encoding.cpp
    src/CLineIndex.cpp
    src/CRenderer.cpp
    src/PieceTable.cpp
)

# 应用程序清单（公共控件 v6，深色菜单栏需要）：
# GCC 16 的 specs 会自动链接工具链内的 default-manifest.o
# （x86_64-w64-mingw32/lib/default-manifest.o）。
# 该文件已被替换为 resources/ 清单编译的版本（带 comctl6 依赖）。
# 重新生成（必须在源码目录下用相对路径调用，windres 对非 ASCII 路径会失败）：
#   cd <源码目录> && windres -O coff resources/editor.rc build/default-manifest.o
#   cp build/default-manifest.o C:/Users/Easecat/Application/mingw64/x86_64-w64-mingw32/lib/default-manifest.o
# 恢复工具链原始默认清单：用仅含 UAC/supportedOS 的最小清单 XML 重新编译即可

add_executable(editor WIN32 ${SRC_LIST})

target_compile_definitions(editor PRIVATE UNICODE _UNICODE)

# -municode 使 CRT 使用 wWinMain 作为入口（编译与链接均需）；源文件/窄字符串均为 UTF-8
target_compile_options(editor PRIVATE
    -finput-charset=UTF-8
    -fexec-charset=UTF-8
)
target_link_options(editor PRIVATE -municode)

target_link_libraries(editor PRIVATE
    d2d1 dwrite dwmapi uxtheme advapi32 shlwapi comctl32 comdlg32 imm32 shell32
)

target_compile_options(editor PRIVATE
    $<$<CONFIG:Release>:-O2 -ffunction-sections -fdata-sections>
)
target_link_options(editor PRIVATE
    $<$<CONFIG:Release>:-s -Wl,--gc-sections>
)

# 核心单元测试（控制台程序，不依赖窗口/D2D）
add_executable(core_tests
    tests/core_tests.cpp
    src/PieceTable.cpp
    src/Encoding.cpp
    src/CLineIndex.cpp
)
target_compile_definitions(core_tests PRIVATE UNICODE _UNICODE)
target_compile_options(core_tests PRIVATE
    -finput-charset=UTF-8
    -fexec-charset=UTF-8
)