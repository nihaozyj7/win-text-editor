#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include "CTextBuffer.h"
#include "PieceTable.h"
#include "CLineIndex.h"
#include "CRenderer.h"

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
    void OnMouseClick(WPARAM wParam, LPARAM lParam, UINT clickCount);
    void OnMouseDrag(LPARAM lParam);
    void OnKeyDown(WPARAM wParam);
    void OnChar(wchar_t ch);
    void OnTimer();

    void UpdateStatusBar();
    void UpdateTitle();
    void UpdateScrollBar();
    void ScrollToLine(DWORD line);
    void MoveCaret(INT dRow, INT dCol, bool extendSelection);
    void MoveCaretTo(int row, int col, bool extendSelection);
    void EnsureCaretVisible();
    void InvalidateEditor();          // 仅失效文本编辑区（状态栏不重绘 → 消除闪烁）

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

private:
    HINSTANCE m_hInstance;
    HWND      m_hwnd;
    HWND      m_hStatusBar;
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

    DWORD m_scrollLine;      // 首可见逻辑行
    DWORD m_caretRow;
    DWORD m_caretCol;

    // 选区（anchor + caret 双向）
    bool  m_selAnchorValid;
    DWORD m_selAnchorRow, m_selAnchorCol;
    DWORD m_selCaretRow, m_selCaretCol;   // 与 m_caret* 同步

    // 光标闪烁
    bool  m_caretVisible;
    bool  m_hasFocus;
};