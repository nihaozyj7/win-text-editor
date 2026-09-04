#include "CEditorWindow.h"
#include <commctrl.h>

namespace
{
    constexpr wchar_t kWindowClassName[] = L"TextEditorMainWindow";
    constexpr wchar_t kWindowTitle[] = L"无标题 - 文本编辑器";

    constexpr int kNewId       = 101;
    constexpr int kOpenId      = 102;
    constexpr int kSaveId      = 103;
    constexpr int kSaveAsId    = 104;
    constexpr int kExitId      = 105;
    constexpr int kWordWrapId  = 201;
    constexpr int kFontId      = 202;
    constexpr int kStatusBarId = 301;

    constexpr int kStatusBarHeight = 22;
}

CEditorWindow::CEditorWindow()
    : m_hInstance(nullptr)
    , m_hwnd(nullptr)
    , m_hStatusBar(nullptr)
    , m_hMenu(nullptr)
{
}

CEditorWindow::~CEditorWindow()
{
}

void CEditorWindow::RegisterWindowClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = CEditorWindow::WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;

    RegisterClassExW(&wc);
}

BOOL CEditorWindow::Create(HINSTANCE hInstance, int nCmdShow)
{
    m_hInstance = hInstance;
    RegisterWindowClass(hInstance);

    // 菜单栏
    HMENU hMenubar = CreateMenu();

    HMENU hFile = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, kNewId,    L"新建(&N)\tCtrl+N");
    AppendMenuW(hFile, MF_STRING, kOpenId,   L"打开(&O)\tCtrl+O");
    AppendMenuW(hFile, MF_STRING, kSaveId,   L"保存(&S)\tCtrl+S");
    AppendMenuW(hFile, MF_STRING, kSaveAsId, L"另存为(&A)\tCtrl+Shift+S");
    AppendMenuW(hFile, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hFile, MF_STRING, kExitId,   L"退出(&X)\tAlt+F4");
    AppendMenuW(hMenubar, MF_POPUP, reinterpret_cast<UINT_PTR>(hFile), L"文件(&F)");

    HMENU hFormat = CreatePopupMenu();
    AppendMenuW(hFormat, MF_STRING, kWordWrapId, L"自动换行(&W)");
    AppendMenuW(hFormat, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hFormat, MF_STRING, kFontId, L"字体(&F)...");
    AppendMenuW(hMenubar, MF_POPUP, reinterpret_cast<UINT_PTR>(hFormat), L"格式(&O)");

    HMENU hView = CreatePopupMenu();
    AppendMenuW(hView, MF_STRING | MF_CHECKED, kStatusBarId, L"状态栏(&S)");
    AppendMenuW(hMenubar, MF_POPUP, reinterpret_cast<UINT_PTR>(hView), L"查看(&V)");

    m_hMenu = hMenubar;

    m_hwnd = CreateWindowExW(
        0, kWindowClassName, kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 600,
        nullptr, hMenubar, hInstance, this);

    if (!m_hwnd)
        return FALSE;

    // 把 this 存进 GWLP_USERDATA（CreateWindowEx 后补设备用）
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);
    return TRUE;
}

LRESULT CALLBACK CEditorWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    CEditorWindow* self = reinterpret_cast<CEditorWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<CEditorWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    if (self)
        return self->HandleMessage(hwnd, msg, wParam, lParam);

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CEditorWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        OnCreate(hwnd);
        return 0;

    case WM_COMMAND:
        OnCommand(LOWORD(wParam));
        return 0;

    case WM_SIZE:
        OnResize();
        return 0;

    case WM_CLOSE:
        Destroy();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

void CEditorWindow::OnCommand(WORD commandId)
{
    switch (commandId)
    {
    case kExitId:
        Destroy();
        break;
    case kStatusBarId:
    {
        // 切换状态栏勾选状态并显示/隐藏
        bool checked = !m_hStatusBar;
        CheckMenuItem(m_hMenu, kStatusBarId, checked ? MF_CHECKED : MF_UNCHECKED);
        ShowWindow(m_hStatusBar, checked ? SW_SHOW : SW_HIDE);
        if (!checked)
        {
            DestroyWindow(m_hStatusBar);
            m_hStatusBar = nullptr;
        }
        OnResize();
        break;
    }
    default:
        break;
    }
}

void CEditorWindow::OnResize()
{
    if (m_hStatusBar)
        SendMessageW(m_hStatusBar, WM_SIZE, 0, 0);
}

void CEditorWindow::OnCreate(HWND hwnd)
{
    m_hwnd = hwnd;

    m_hStatusBar = CreateStatusWindowW(WS_CHILD | WS_VISIBLE, L"就绪", hwnd, kStatusBarId);
    if (m_hStatusBar)
    {
        int parts[3] = { 200, 400, -1 };
        SendMessageW(m_hStatusBar, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));
    }
}

void CEditorWindow::Destroy()
{
    DestroyWindow(m_hwnd);
}

int CEditorWindow::Run()
{
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}