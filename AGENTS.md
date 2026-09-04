# AGENTS.md

Windows 记事本风格文本编辑器：Win32 + Direct2D/DirectWrite，C++20，仅 Windows 平台。目标是流畅打开/编辑超大文件（已用 100MB 样本验证）。

## 构建与测试

- MinGW-w64 与 Ninja 安装在**非默认路径** `C:/Users/Easecat/Application/mingw64/bin/`，已硬编码在 `CMakeLists.txt`。
- `build/` 已配置好，日常构建直接：
  - `cmake --build build`（构建全部）
  - `cmake --build build --target core_tests`（仅单测）
- 全新配置：`cmake -S . -B build -G Ninja`
- 运行单测：`build/core_tests.exe`（纯数据层，控制台程序，无窗口/D2D 依赖）
- 运行主程序：`build/editor.exe [文件路径]`（GUI 程序，命令行第一参数为要打开的文件）
- 大样本文件 `big_*.txt`、`test_*.txt` 已被 gitignore，不要提交。

## 架构分层（数据流向，勿跨层直连）

1. `CTextBuffer` — 内存映射文件（MMF **只读**）+ 编码检测。禁止写入映射视图。
2. `PieceTable` — 编辑模型：只读原始缓冲 + 追加缓冲的片段链表；内建撤销/重做命令栈（上限 1000）。
3. `CLineIndex` — 稀疏行索引（每 1000 行一个关键帧）。数据源抽象为 `ByteReader` 回调，可指向 MMF 或 PieceTable；换行扫描按编码感知。
4. `CRenderer` — D2D 渲染（虚拟滚动、字体回退、Emoji 彩色）。
5. `CEditorWindow` — Win32 窗口/菜单/状态栏/键盘/IME/滚动条，组装以上各层。
6. `Encoding` — 编码检测（BOM → UTF-8 校验 → 回退 ANSI/GBK）与 UTF-16 双向转换。

核心单测只覆盖 2/3/6 层（PieceTable、CLineIndex、Encoding）；改这些层必须跑 `core_tests.exe`。

## 关键约束与已知坑（多为已修复 bug，勿回退）

- **坐标体系**：所有偏移均为“逻辑字节偏移”（原文件编码下的字节），不是 UTF-16 字符位置。PosToByte/ByteToPos 换算需注意 BOM 占用的字节（见 fix 0738be7）。
- **编辑同步**：每次 Insert/Erase 后必须调用 `CLineIndex::NotifyEdit(ofs, deltaBytes, deltaLines)`，否则行索引失步（曾出 bug）；关键帧漂移过大时它内部会全量重建。
- **UTF-16 换行扫描**按 code unit（2 字节）进行，字节 0x0A 不是换行；新增扫描逻辑务必走 `ScanState::ReadUnit`。
- **编码/入口**：源码为 UTF-8，编译带 `-finput-charset=UTF-8 -fexec-charset=UTF-8`，`UNICODE/_UNICODE` 已定义，入口是 `wWinMain`（`-municode`）；新增字符串用宽字符。
- **UI 细节**：主窗口带 `WS_CLIPCHILDREN` 防闪烁；IME 走 `WM_IME_CHAR` 分支；这些处理在 `CEditorWindow.cpp`，改消息处理前先看现有分支。
- 渲染初始化 COM 为 STA（DWrite/D2D 要求），见 `main.cpp`。

## 约定

- 代码注释与提交信息用中文；提交格式 `feat:` / `fix:` / `refactor:` + 中文描述。
- 项目按里程碑推进（M1~M6 已完成：工程骨架 → MMF 只读 → PieceTable → 行索引/虚拟滚动 → 编辑/IME/撤销 → 渲染细节）。
