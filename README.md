# TextEditor — Windows 记事本风格大文件文本编辑器

一个用 **C++20 + Win32 + Direct2D/DirectWrite** 从零实现的记事本风格文本编辑器，目标是**流畅打开与编辑超大文本文件**（已用 100MB 样本验证：秒开、滚动/编辑不卡顿）。

不做通用 UI 框架，不依赖第三方库——内存映射、PieceTable、稀疏行索引、D2D 渲染全部手写，适合作为 Windows 原生开发与大文件编辑器设计的学习参考。

## 功能特性

**大文件性能**
- 内存映射文件（MMF）只读加载，打开文件不做全量拷贝
- PieceTable 编辑模型：编辑写入追加缓冲，原始缓冲保持只读
- 稀疏行索引（每 1000 行一个关键帧），编辑按增量同步，不做全量重扫
- 虚拟滚动：只排版可视区域内的行，百万行文件滚动依旧流畅

**编辑**
- 撤销 / 重做（命令栈上限 1000 步）
- 复制 / 粘贴（CF_UNICODETEXT 剪贴板）
- 中文 IME 输入（`WM_IME_CHAR`）
- 鼠标点击 / 双击选词 / 拖拽选区，选区高亮

**渲染与外观**
- Direct2D 渲染，DirectWrite 字体回退，彩色 Emoji
- 深色 / 浅色 / 跟随系统主题（含深色标题栏、菜单栏）
- 行号栏、自动换行、可调内边距与行高（1.0~2.0 倍）
- 字号调节（`Ctrl+=` / `Ctrl+-` / `Ctrl+0`），主字体 Consolas / 次要字体微软雅黑

**编码**
- 自动检测：BOM → UTF-8 校验 → 回退 ANSI/GBK，支持 UTF-16 双向转换
- 内部统一使用"逻辑字节偏移"（原文件编码下的字节偏移），保存时保留原编码

## 环境要求

- Windows 10 1809+（深色标题栏需要）
- CMake 3.24+
- MinGW-w64（GCC 16+，含 windres）

> 当前 `CMakeLists.txt` 中编译器/Ninja 路径写死为 `C:/Users/Easecat/Application/mingw64/bin/`，
> 其他环境请按需修改这三行 `CMAKE_C_COMPILER` / `CMAKE_CXX_COMPILER` / `CMAKE_MAKE_PROGRAM`。

## 构建与运行

```bash
# 全新配置（已配置好 build/ 可跳过）
cmake -S . -B build -G Ninja

# 构建（默认 Release：-O2 + strip + --gc-sections，单文件 exe，无 DLL 依赖）
cmake --build build

# 运行（命令行第一参数为要打开的文件，可选）
build/editor.exe [文件路径]

# 运行核心单元测试（纯数据层，无窗口/D2D 依赖）
cmake --build build --target core_tests
build/core_tests.exe
```

## 快捷键

| 快捷键 | 功能 |
| --- | --- |
| `Ctrl+N` / `Ctrl+O` | 新建 / 打开 |
| `Ctrl+S` / `Ctrl+Shift+S` | 保存 / 另存为 |
| `Ctrl+Z` / `Ctrl+Y` | 撤销 / 重做 |
| `Ctrl+C` / `Ctrl+V` | 复制 / 粘贴 |
| `Ctrl+=` / `Ctrl+-` / `Ctrl+0` | 增大 / 减小 / 重置字号 |
| `Alt` | 菜单栏隐藏时临时呼出 |

## 架构设计

数据自下而上单向流动，各层不跨层直连：

```
CTextBuffer      内存映射文件（只读）+ 编码检测
     │
PieceTable       编辑模型：只读原始缓冲 + 追加缓冲的片段链表
     │           内建撤销/重做命令栈
CLineIndex       稀疏行索引（每 1000 行一个关键帧）
     │           数据源抽象为 ByteReader 回调（可指向 MMF 或 PieceTable）
CRenderer        Direct2D 渲染：虚拟滚动、字体回退、Emoji
     │
CEditorWindow    Win32 窗口/菜单/状态栏/键盘/IME/滚动条，组装以上各层
```

另有独立的 `Encoding` 模块负责编码检测与 UTF-16 转换。

### 性能设计要点

- **打开**：MMF 只读映射，零拷贝；行索引按需分块扫描
- **编辑**：Insert/Erase 后通过 `CLineIndex::NotifyEditRange` 增量同步行索引，
  行结构变化走关键帧增删/平移（O(帧数)），常规编辑禁止全量重建
- **滚动**：只对视口内的行做 DirectWrite 排版，配块缓存复用排版结果
- **UTF-16 换行扫描**按 code unit（2 字节）进行，避免把 `0x0A` 误判为换行

## 目录结构

```
├── src/
│   ├── main.cpp            入口：COM(STA) 初始化、消息循环
│   ├── CTextBuffer.*       MMF 只读映射 + 编码检测
│   ├── PieceTable.*        PieceTable 编辑模型 + 撤销/重做
│   ├── CLineIndex.*        稀疏行索引
│   ├── CRenderer.*         Direct2D 渲染
│   ├── CEditorWindow.*     窗口/菜单/输入/滚动/IME
│   └── Encoding.*          编码检测与转换
├── tests/core_tests.cpp    数据层单元测试（PieceTable / CLineIndex / Encoding）
├── resources/              应用图标 + 应用程序清单（comctl32 v6）
└── CMakeLists.txt
```

## 开发进度

- [x] M1 工程骨架（窗口/菜单/状态栏）
- [x] M2 MMF 只读加载 + 编码检测
- [x] M3 PieceTable 编辑模型 + 撤销/重做
- [x] M4 行索引 + 虚拟滚动 + D2D 渲染
- [x] M5 编辑/IME/撤销 + 字体回退 + Emoji + 选区
- [x] M6 渲染细节（选区高亮、防闪烁、主题）
