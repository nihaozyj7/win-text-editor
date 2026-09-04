cmake_minimum_required(VERSION 3.24)
project(TextEditor CXX)

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
)

add_executable(editor WIN32 ${SRC_LIST})

target_compile_definitions(editor PRIVATE UNICODE _UNICODE)

# -municode 使 CRT 使用 wWinMain 作为入口（编译与链接均需）；源文件/窄字符串均为 UTF-8
target_compile_options(editor PRIVATE
    -finput-charset=UTF-8
    -fexec-charset=UTF-8
)
target_link_options(editor PRIVATE -municode)

target_link_libraries(editor PRIVATE
    d2d1 dwrite dwmapi advapi32 shlwapi comctl32
)

target_compile_options(editor PRIVATE
    $<$<CONFIG:Release>:-O2 -ffunction-sections -fdata-sections>
)
target_link_options(editor PRIVATE
    $<$<CONFIG:Release>:-s -Wl,--gc-sections>
)