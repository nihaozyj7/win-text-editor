#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <unordered_map>
#include "CTextBuffer.h"
#include "PieceTable.h"
#include "CLineIndex.h"
#include "CRenderer.h"

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

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void OnCreate(HWND hwnd);
    void OnResize();
    void OnCommand(WORD commandId);
    void OnPaint();
    void OnScroll(WPARAM wParam, LPARAM lParam);
    void OnHScroll(WPARAM wParam, LPARAM lParam);
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
        UINT         visualLines;
        std::wstring text;
    };
    void BumpVisualEpoch();
    const VisualRow& VisualRowOf(DWORD row) const;
    UINT RowVisualCount(DWORD row) const;

private:
    HINSTANCE m_hInstance;
    HWND      m_hwnd;
    HWND      m_hStatusBar;
    HWND      m_hVScroll;              // 垂直滚动条（编辑区子控件，不占状态栏行）
    HWND      m_hHScroll;              // 水平滚动条（编辑区子控件）
    HMENU     m_hMenu;

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

    // ---- 状态栏 owner-draw ----
    std::wstring m_statusText[4];
    HBRUSH       m_statusBgBrush;
    COLORREF     m_statusFgColor;
};
