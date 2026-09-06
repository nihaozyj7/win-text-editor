#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <unordered_map>
#include "CTextBuffer.h"
#include "PieceTable.h"
#include "CLineIndex.h"
#include "CRenderer.h"
#include "Highlighter.h"

// 编辑器内容区内边距（文本区与窗口边缘的距离，像素）
struct EditorPadding
{
    int left;
    int top;
    int right;
    int bottom;
};

// 主题模式
enum ThemeMode { ThemeFollowSystem = 0, ThemeLight = 1, ThemeDark = 2 };

// 主编辑器窗口：窗口生命周期、消息循环、菜单/状态栏、虚拟滚动、
// 光标/选区、键盘编辑(含 IME)、撤销/重做与文件保存
class CEditorWindow
{
public:
    CEditorWindow();
    ~CEditorWindow();

    BOOL Create(HINSTANCE hInstance, int nCmdShow);
    int  Run();

    void Destroy();

    BOOL OpenFile(LPCWSTR szPath);
    BOOL SaveFile(LPCWSTR szPath);   // 保存到指定路径（编码 = 打开时检测或用户设定）

    // 该文件已被任一实例窗口打开时，激活（恢复+置前）那个窗口并返回 true（含本窗口）。
    // 供启动入口与"打开"对话框去重使用
    static bool ActivateExistingForFile(LPCWSTR szPath);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void OnCreate(HWND hwnd);
    void OnResize();
    void OnCommand(WORD commandId);
    void OnPaint();
    void OnMouseClick(WPARAM wParam, LPARAM lParam, UINT clickCount);
    void OnMouseDrag(LPARAM lParam);
    void OnKeyDown(WPARAM wParam);
    void OnChar(wchar_t ch);
    void OnTimer();

    void UpdateStatusBar();
    void UpdateStatusBarParts();
    void UpdateTitle();
    void SyncDirtyFlag();            // 按撤销栈历史位置同步 m_dirty（变化时刷新标题/状态栏）
    bool SaveDocument();             // 保存当前文档（无路径时先弹另存为对话框）；失败/取消返回 false
    void UpdateScrollBar();
    void ScrollToLine(DWORD line);

    // ---- 自定义滚动条（D2D 绘制，替代原生 SCROLLBAR 控件）----
    float SbarWidth() const;                 // 滚动条厚度（像素，随 DPI 缩放）
    bool  VScrollVisible() const;            // 内容高度 ≥ 视口 1.5 倍才显示
    bool  InRect(const D2D1_RECT_F& r, const POINT& pt) const;
    bool  OnScrollbarDown(int x, int y);     // 命中滚动条按下；处理拖拽/翻页，返回是否命中
    void  OnScrollbarMove(int x, int y);     // 拖拽滑块 + 悬停着色
    void  OnScrollbarUp();                   // 结束拖拽
    void  SetHScrollPos(float px);           // 设置水平滚动位置（含钳制/刷新）
    bool  m_mouseTracking = false;           // WM_MOUSELEAVE 跟踪标记（悬停态清除）
    int   m_barDrag = 0;                     // 0 无 / 1 垂直滑块 / 2 水平滑块 / 3 垂直翻页 / 4 水平翻页
    float m_barDragOfs = 0.0f;               // 按下点相对滑块顶/左缘的偏移
    CRenderer::ScrollbarDraw m_vBar;         // 垂直条几何（窗口层计算，渲染层绘制）
    CRenderer::ScrollbarDraw m_hBar;
    int   m_vMax = 0;                        // 垂直可滚动行数上限（拖拽换算用）
    int   m_vPage = 1;                       // 视口可容纳行数
    float m_hMax = 0.0f;                     // 水平可滚动像素上限
    float m_hPage = 1.0f;
    void MoveCaret(INT dRow, INT dCol, bool extendSelection);
    void MoveCaretTo(int row, int col, bool extendSelection);
    void EnsureCaretVisible(bool typingMode);
    void ScrollCaretToComfortHeight();   // 把光标行滚到视口约 2/3 高度处
    DWORD ComfortScrollLine(float lineH, float viewportH, float ratio, bool* exact);
    DWORD ComfortScrollLineFar(float lineH, float viewportH);   // 远距离快速估算
    void InvalidateEditor();          // 仅失效文本编辑区（状态栏不重绘 → 消除闪烁）

    // ---- 布局几何 ----
    RECT  EditorRect() const;         // 编辑区客户区（已扣除状态栏）
    float ViewportHeight() const;
    float TextOriginX() const;        // 文本绘制原点 x = 左内边距 + 行号栏宽
    float TextAreaWidth() const;      // 文本排版可用宽度
    int   MaxScrollLine() const;      // 垂直滚动上限（含文末约 2/3 视口高度的留白）

    // ---- 设置项应用 ----
    void ApplyRendererOptions();      // 字体/行高/换行/行号 → 渲染器
    void ApplyThemeToWindow();        // 主题 → 渲染器 + 标题栏/菜单/状态栏
    bool IsDarkTheme() const;         // 解析"跟随系统"
    void SetDarkTitleBar(bool dark);
    void ApplyMenuTheme(bool dark);   // uxtheme 深色菜单/滚动条（未公开 API，失败无害）
    void OnDrawStatusBarPart(DRAWITEMSTRUCT* dis);  // 状态栏 owner-draw（主题着色）
    void SyncMenuChecks();
    void PickFont(bool primary);      // ChooseFont 选择主/次要字体
    void UpdateFontMenuLabels();      // 字体菜单项显示当前字体名
    void ApplyMenuBarState();         // 按设置挂/摘菜单栏（隐藏时 Alt 临时呼出由键盘分支处理）
    void EndTempMenuBar();            // 收回 Alt 临时呼出的菜单栏
    void ChangeFontSize(float delta); // 字号增减（delta=0 重置为 14）
    void LoadSettings();              // 启动时从 %APPDATA%\TextEditor\settings.ini 读取
    void SaveSettings();              // 任一设置变化时写回
    void ApplyDpiScale();             // 按窗口所在显示器 DPI 换算渲染尺寸/内边距/状态栏字体
    int  Scale(int v) const;          // 96-DPI 基准值 → 当前 DPI 像素
    void UpdateStatusFont();          // 状态栏字体随 DPI 缩放
    HFONT m_statusFont = nullptr;     // 状态栏使用的缩放字体

    // ---- 多实例去重 ----
    bool  OnFileActivateCopyData(LPARAM lParam);  // WM_COPYDATA：查询的文件已在本窗口打开则置前自己

    // ---- 命中测试 / IME ----
    bool  HitTestClient(int x, int y, DWORD* pRow, DWORD* pCol); // false = 最后一行之下的空白
    POINT GetCaretClientPoint();      // 光标的客户区坐标（IME 组合窗口定位用）
    void  UpdateImeCompositionWindow();
    bool  IsImeOpen() const;
    bool  VisualTopOfCaretRow(float lineH, float* yTop); // 从滚动行累计视觉高度，false=过远

    // ---- 文本/编辑 ----
    void RebuildDocument();           // 文件(重新)打开后：PieceTable 复位 + 行索引重建
    std::wstring GetLineText(DWORD row) const;
    uint64_t     DocByteSize() const;
    unsigned char DocByteAt(uint64_t ofs) const;
    uint64_t     DocRead(uint64_t ofs, unsigned char* dst, uint64_t maxLen) const;
    // 逻辑字节偏移 → 行号（通过行索引）
    uint64_t     DocOffsetToRow(uint64_t ofs) const;

    // 行内文本（UTF-16）→ 该行内部字节偏移；用于 (row,col)→字节坐标换算
    uint64_t     ColToByteInLine(DWORD row, DWORD col) const;
    // (row, col) → 文档逻辑字节偏移
    uint64_t     PosToByte(DWORD row, DWORD col) const;
    // 文档逻辑字节偏移 → (row, col)
    void         ByteToPos(uint64_t ofs, DWORD* pRow, DWORD* pCol) const;

    void         InsertTextAtCaret(const std::wstring& text);
    void         DeleteRange(DWORD startRow, DWORD startCol, DWORD endRow, DWORD endCol);
    void         Backspace();
    void         Undo();
    void         Redo();

    // ---- 剪贴板 ----
    void         CopySelection();       // 选区 → 剪贴板（跨行用 \r\n 连接）
    void         PasteFromClipboard();  // 剪贴板 → 光标处（CF_UNICODETEXT）
    bool         SetClipboardText(const std::wstring& text);

    // ---- 选区 ----
    void         BeginSelectionAnchor();      // 在光标处设锚点
    void         SelectWordAt(DWORD row, DWORD col);
    void         SelectLineAt(DWORD row);
    void         NormalizeSelection();         // 交换 anchor/caret 使 start<=end
    CRenderer::Selection GetRenderSelection() const;
    bool         HasSelection() const;

    void BuildVisibleRows(std::vector<CRenderer::Row>& rows) const;
    static void RegisterWindowClass(HINSTANCE hInstance);

    // ---- 可见行缓存：行文本 + 自动换行视觉行数，按"数据代"失效 ----
    // （编辑/设置/尺寸变化时代数 +1；滚动/移动光标不失效，避免每键重复解码与建 layout）
    struct VisualRow
    {
        UINT               visualLines;
        std::wstring       text;
        // 语法着色 token 缓存：token 只依赖行文本 + 行首词法状态，
        // 编辑/设置变化通过整代失效（BumpVisualEpoch）覆盖
        std::vector<Token> tokens;
        uint32_t           hlStateIn = 0;
        bool               highlighted = false;
    };
    void BumpVisualEpoch();
    const VisualRow& VisualRowOf(DWORD row) const;
    UINT RowVisualCount(DWORD row) const;

    // ---- 滚动活跃检测与高亮延迟补算 ----
    void NoteScrollActivity();          // 滚动路径调用，刷新活跃时间戳
    bool ScrollActive() const;          // 最近是否在快速滚动（期间跳过词法分析）
    void ScheduleHighlightRefresh() const; // 启动补算定时器（已启动则忽略）
    void OnHighlightRefreshTimer();     // 定时器：停稳后补一次高亮重绘

    // ---- 语法高亮：行词法状态缓存 ----
    // m_hlStates[i] = 第 i 行结束时的词法状态（m_hlStatesValid 条有效）。
    // 编辑只从改动行起失效（前缀状态不变），避免大文件每次按键全量重扫
    uint32_t StateAfterLine(DWORD row, size_t maxCatchUp = SIZE_MAX,
                            bool* throttled = nullptr) const;
    uint32_t StateBeforeLine(DWORD row, bool* throttled = nullptr) const;
    void InvalidateHighlightFrom(DWORD line);    // 自 line 起状态失效（编辑钩子）
    void ClearHighlightCache();                  // 撤销/重做/换文件等全量失效

private:
    HINSTANCE m_hInstance;
    HWND      m_hwnd;
    HWND      m_hStatusBar;
    HMENU     m_hMenu;
    float     m_dpiScale = 1.0f;       // 当前显示器 DPI / 96（Per-Monitor V2）

    std::unique_ptr<CTextBuffer> m_buffer;   // 原文件 MMF（只读原始字节）
    PieceTable m_piece;                      // 编辑模型（原文 + 追加）
    CLineIndex m_lineIndex;
    std::unique_ptr<CRenderer> m_renderer;

    // 编码：打开时检测；另存时可由用户改（v1 保持检测值）
    Encoding m_encoding;
    DWORD    m_bomBytes;   // 文件头 BOM 字节数（0/2/3），编辑偏移换算用
    std::wstring m_filePath;
    bool m_dirty;
    size_t m_savedUndoDepth;   // 保存/打开时的撤销历史位置，比较即可判断"回到已保存状态"

    DWORD m_scrollLine;      // 首可见逻辑行（可超出总行数 → 文末留白区）
    DWORD m_caretRow;
    DWORD m_caretCol;

    // 选区（anchor + caret 双向）
    bool  m_selAnchorValid;
    DWORD m_selAnchorRow, m_selAnchorCol;
    DWORD m_selCaretRow, m_selCaretCol;   // 与 m_caret* 同步

    // 光标闪烁
    bool  m_caretVisible;
    bool  m_hasFocus;

    // ---- 显示设置 ----
    bool          m_showStatusBar;       // 状态栏（默认开，可持久化）
    bool          m_hideMenuBar;         // 隐藏菜单栏（默认关；隐藏后 Alt 临时呼出）
    bool          m_menuBarTempShown;    // 菜单栏处于 Alt 临时呼出状态
    bool          m_altOtherKey;         // Alt 按下期间又按了别的键（非孤立 Alt）
    bool          m_showLineNumbers;     // 行号（默认关）
    bool          m_wordWrap;            // 自动换行（默认开）
    float         m_lineHeightFactor;    // 行高 = 字号 × factor
    float         m_fontSize;            // 字号（磅）
    EditorPadding m_pad;                 // 内容区内边距
    int           m_themeMode;           // ThemeMode
    std::wstring  m_fontPrimary;         // 主字体（缺字形回退次要字体）
    std::wstring  m_fontSecondary;       // 次要字体（再缺字形回退系统默认）
    float         m_hScrollPos;          // 水平滚动位置（像素，仅关换行时用）
    float         m_maxLineWidth;        // 已见到的最宽行（像素，水平滚动范围）

    // ---- 可见行缓存（mutable：只读路径 BuildVisibleRows/命中测试中更新）----
    mutable std::unordered_map<DWORD, VisualRow> m_visualCache;
    mutable uint64_t m_visualEpochSeen;  // 缓存对应的数据代
    uint64_t         m_visualEpoch;      // 当前数据代（编辑/设置/尺寸变化 +1）

    // ---- 语法高亮（mutable：只读绘制路径 BuildVisibleRows 中惰性扩展）----
    Lang m_lang = Lang::None;                       // 按扩展名检测；None 不高亮
    mutable std::vector<uint32_t> m_hlStates;       // 行末词法状态缓存
    mutable size_t m_hlStatesValid = 0;             // 有效条数
    mutable CLineIndex::LineCursor m_hlCursor;      // 补算顺序游标（O(n) 逐行推进）
    mutable size_t m_hlCursorRow = SIZE_MAX;        // 游标当前指向的行（与有效条数同步）
    mutable ULONGLONG m_lastScrollTick = 0;         // 最近一次滚动的时刻（毫秒计数）
    mutable bool m_hlRefreshTimerOn = false;        // 高亮补算定时器是否在跑

    // ---- 状态栏 owner-draw ----
    std::wstring m_statusText[4];
    HBRUSH       m_statusBgBrush;
    COLORREF     m_statusFgColor;
};
