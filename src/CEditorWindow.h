#pragma once
#include <windows.h>

// 主编辑器窗口：负责窗口创建/销毁、消息循环、菜单与状态栏
// 后续里程碑会在此接入渲染器、缓冲区、编辑模型等模块
class CEditorWindow
{
public:
    CEditorWindow();
    ~CEditorWindow();

    BOOL Create(HINSTANCE hInstance, int nCmdShow);
    int  Run();

    // 供 WM_CLOSE 等消息触发窗口销毁
    void Destroy();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void OnCreate(HWND hwnd);
    void OnResize();
    void OnCommand(WORD commandId);

    static void RegisterWindowClass(HINSTANCE hInstance);

private:
    HINSTANCE m_hInstance;
    HWND      m_hwnd;
    HWND      m_hStatusBar;
    HMENU     m_hMenu;
};