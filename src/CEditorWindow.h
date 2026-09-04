#pragma once
#include <windows.h>
#include <memory>
#include "CTextBuffer.h"
#include "CLineIndex.h"
#include "CRenderer.h"

// 主编辑器窗口：窗口生命周期、消息循环、菜单/状态栏、虚拟滚动与光标
class CEditorWindow
{
public:
    CEditorWindow();
    ~CEditorWindow();

    BOOL Create(HINSTANCE hInstance, int nCmdShow);
    int  Run();

    void Destroy();

    // 打开文件（命令行或"打开"菜单），成功返回 TRUE
    BOOL OpenFile(LPCWSTR szPath);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void OnCreate(HWND hwnd);
    void OnResize();
    void OnCommand(WORD commandId);
    void OnPaint();
    void OnScroll(WPARAM wParam, LPARAM lParam);
    void OnMouseClick(LPARAM lParam);
    void OnKeyDown(WPARAM wParam);

    void UpdateStatusBar();
    void UpdateTitle();
    void UpdateScrollBar();
    void ScrollToLine(DWORD line);
    void MoveCaret(INT dRow, INT dCol);
    void EnsureCaretVisible();

    // 解码第 row 行（按编码）为 UTF-16 文本（去除行尾换行）
    std::wstring GetLineText(DWORD row) const;
    // 组装当前可视区各行
    void BuildVisibleRows(std::vector<CRenderer::Row>& rows) const;

    static void RegisterWindowClass(HINSTANCE hInstance);

private:
    HINSTANCE m_hInstance;
    HWND      m_hwnd;
    HWND      m_hStatusBar;
    HMENU     m_hMenu;

    std::unique_ptr<CTextBuffer> m_buffer;
    CLineIndex m_lineIndex;
    std::unique_ptr<CRenderer> m_renderer;

    DWORD m_scrollLine;   // 当前首可见行
    DWORD m_caretRow;     // 光标逻辑行
    DWORD m_caretCol;     // 光标列（UTF-16 code unit）
};