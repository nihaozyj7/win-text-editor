#include "CEditorWindow.h"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <sstream>
#include <iomanip>

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

    // 编码名称（状态栏显示用）
    const wchar_t* EncodingName(Encoding enc)
    {
        switch (enc)
        {
        case Encoding::Utf8:    return L"UTF-8";
        case Encoding::Utf16LE: return L"UTF-16 LE";
        case Encoding::Utf16BE: return L"UTF-16 BE";
        case Encoding::Ansi:    return L"ANSI/GBK";
        }
        return L"未知";
    }

    // 人类可读的文件大小
    std::wstring FormatSize(LONGLONG bytes)
    {
        static const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB" };
        double value = static_cast<double>(bytes);
        int unit = 0;
        while (value >= 1024.0 && unit < 3)
        {
            value /= 1024.0;
            ++unit;
        }
        std::wstringstream ss;
        ss << std::fixed << std::setprecision(2) << value << L" " << units[unit];
        return ss.str();
    }
}

CEditorWindow::CEditorWindow()
    : m_hInstance(nullptr)
    , m_hwnd(nullptr)
    , m_hStatusBar(nullptr)
    , m_hMenu(nullptr)
    , m_renderer(std::make_unique<CRenderer>())
    , m_scrollLine(0)
    , m_caretRow(0)
    , m_caretCol(0)
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
        WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 600,
        nullptr, hMenubar, hInstance, this);

    if (!m_hwnd)
        return FALSE;

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

    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_VSCROLL:
        OnScroll(wParam, lParam);
        return 0;

    case WM_MOUSEWHEEL:
    {
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int steps = -delta / WHEEL_DELTA;   // 向下滚动为负
        if (m_buffer)
        {
            long newLine = static_cast<long>(m_scrollLine) + steps * 3;
            if (newLine < 0) newLine = 0;
            DWORD maxLine = m_lineIndex.GetLineCount() > 1
                ? static_cast<DWORD>(m_lineIndex.GetLineCount() - 1)
                : 0;
            if (static_cast<DWORD>(newLine) > maxLine) newLine = maxLine;
            ScrollToLine(static_cast<DWORD>(newLine));
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
        SetFocus(m_hwnd);
        OnMouseClick(lParam);
        return 0;

    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;

    case WM_ERASEBKGND:
        return 1;   // D2D 全量绘制，避免背景闪烁

    case WM_CLOSE:
        Destroy();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CEditorWindow::OnCommand(WORD commandId)
{
    switch (commandId)
    {
    case kOpenId:
    {
        wchar_t path[MAX_PATH * 4] = { 0 };
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hwnd;
        ofn.lpstrFilter = L"所有文件(*.*)\0*.*\0文本文件(*.txt)\0*.txt\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH * 4;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn))
            OpenFile(path);
        break;
    }
    case kExitId:
        Destroy();
        break;
    case kStatusBarId:
    {
        bool checked = !m_hStatusBar;
        CheckMenuItem(m_hMenu, kStatusBarId, checked ? MF_CHECKED : MF_UNCHECKED);
        if (checked)
        {
            m_hStatusBar = CreateStatusWindowW(WS_CHILD | WS_VISIBLE, L"就绪", m_hwnd, kStatusBarId);
            int parts[3] = { 200, 400, -1 };
            SendMessageW(m_hStatusBar, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));
        }
        else
        {
            DestroyWindow(m_hStatusBar);
            m_hStatusBar = nullptr;
        }
        OnResize();
        UpdateStatusBar();
        break;
    }
    default:
        break;
    }
}

BOOL CEditorWindow::OpenFile(LPCWSTR szPath)
{
    m_buffer = std::make_unique<CTextBuffer>();
    if (!m_buffer->OpenFile(szPath))
    {
        m_buffer.reset();
        wchar_t msg[1024];
        wsprintfW(msg, L"无法打开文件:\n%s", szPath);
        MessageBoxW(m_hwnd, msg, L"错误", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    // 构建行索引（编码感知；MMF 全量只读扫描）
    m_lineIndex.Build(m_buffer->GetBasePtr(), static_cast<uint64_t>(m_buffer->GetSize()),
                      m_buffer->GetEncoding());

    m_scrollLine = 0;
    m_caretRow = 0;
    m_caretCol = 0;

    UpdateTitle();
    UpdateStatusBar();
    UpdateScrollBar();
    InvalidateRect(m_hwnd, nullptr, TRUE);
    return TRUE;
}

void CEditorWindow::UpdateTitle()
{
    if (m_buffer && m_buffer->GetPath()[0])
    {
        std::wstring title = m_buffer->GetPath();
        title += L" - 文本编辑器";
        SetWindowTextW(m_hwnd, title.c_str());
    }
    else
    {
        SetWindowTextW(m_hwnd, kWindowTitle);
    }
}

void CEditorWindow::UpdateStatusBar()
{
    if (!m_hStatusBar)
        return;

    if (!m_buffer)
    {
        SendMessageW(m_hStatusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"就绪"));
        SendMessageW(m_hStatusBar, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(L""));
        SendMessageW(m_hStatusBar, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(L""));
        return;
    }

    std::wstring info;
    {
        std::wstringstream ss;
        ss << L"行 " << (m_caretRow + 1) << L", 列 " << (m_caretCol + 1)
           << L" | 总行 " << m_lineIndex.GetLineCount();
        info = ss.str();
    }
    std::wstring sizeText = FormatSize(m_buffer->GetSize());
    std::wstring encodingText = EncodingName(m_buffer->GetEncoding());

    SendMessageW(m_hStatusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(info.c_str()));
    SendMessageW(m_hStatusBar, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(encodingText.c_str()));
    SendMessageW(m_hStatusBar, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(sizeText.c_str()));
}

void CEditorWindow::OnResize()
{
    if (m_hStatusBar)
        SendMessageW(m_hStatusBar, WM_SIZE, 0, 0);

    if (m_renderer)
        m_renderer->Resize();

    UpdateScrollBar();
    InvalidateRect(m_hwnd, nullptr, TRUE);
}

void CEditorWindow::OnCreate(HWND hwnd)
{
    m_hwnd = hwnd;

    m_hStatusBar = CreateStatusWindowW(WS_CHILD | WS_VISIBLE, L"就绪", hwnd, kStatusBarId);
    if (m_hStatusBar)
    {
        int parts[3] = { 240, 500, -1 };
        SendMessageW(m_hStatusBar, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));
    }

    if (m_renderer)
        m_renderer->Init(hwnd);
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

// ---------- 渲染 / 滚动 / 光标 ----------

std::wstring CEditorWindow::GetLineText(DWORD row) const
{
    if (!m_buffer)
        return {};

    uint64_t start = m_lineIndex.GetLineStart(row);
    uint64_t len = m_lineIndex.GetLineLength(row);

    const BYTE* base = m_buffer->GetBasePtr();
    if (!base)
        return {};

    // 解码（分块拷贝，避免超长行一次性分配太大）
    std::wstring text = DecodeToWide(base + start, static_cast<DWORD>(len), m_buffer->GetEncoding());

    // 去除行尾换行符
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r'))
        text.pop_back();

    // 第一行可能有 BOM 字符（UTF-8 BOM/UTF-16 BOM），剥离
    if (row == 0 && !text.empty() && text[0] == 0xFEFF)
        text.erase(text.begin());

    return text;
}

void CEditorWindow::BuildVisibleRows(std::vector<CRenderer::Row>& rows) const
{
    if (!m_buffer)
        return;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (m_hStatusBar && IsWindowVisible(m_hStatusBar))
    {
        RECT sb;
        GetWindowRect(m_hStatusBar, &sb);
        rc.bottom -= (sb.bottom - sb.top);
    }

    float lineH = m_renderer ? m_renderer->GetLineHeight() : 20.0f;
    int visibleCount = static_cast<int>((rc.bottom - rc.top) / lineH) + 1;
    DWORD totalLines = static_cast<DWORD>(m_lineIndex.GetLineCount());

    rows.clear();
    rows.reserve(visibleCount);

    for (int i = 0; i < visibleCount; ++i)
    {
        DWORD row = m_scrollLine + i;
        if (row >= totalLines)
            break;
        CRenderer::Row r;
        r.row = row;
        r.text = GetLineText(row);
        rows.push_back(std::move(r));
    }
}

void CEditorWindow::OnPaint()
{
    PAINTSTRUCT ps;
    BeginPaint(m_hwnd, &ps);

    if (!m_renderer)
    {
        EndPaint(m_hwnd, &ps);
        return;
    }

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    int clientHeight = rc.bottom - rc.top;
    if (m_hStatusBar && IsWindowVisible(m_hStatusBar))
    {
        RECT sb;
        GetWindowRect(m_hStatusBar, &sb);
        clientHeight -= (sb.bottom - sb.top);
    }
    if (clientHeight < 0)
        clientHeight = 0;

    std::vector<CRenderer::Row> rows;
    BuildVisibleRows(rows);

    int lineHeight = static_cast<int>(m_renderer->GetLineHeight());
    m_renderer->Render(rows, lineHeight, rc.right - rc.left, m_caretRow, m_caretCol);

    EndPaint(m_hwnd, &ps);
}

void CEditorWindow::UpdateScrollBar()
{
    if (!m_hwnd || !m_buffer)
    {
        ShowScrollBar(m_hwnd, SB_VERT, FALSE);
        return;
    }

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (m_hStatusBar && IsWindowVisible(m_hStatusBar))
    {
        RECT sb;
        GetWindowRect(m_hStatusBar, &sb);
        rc.bottom -= (sb.bottom - sb.top);
    }

    float lineH = m_renderer ? m_renderer->GetLineHeight() : 20.0f;
    int pageLines = static_cast<int>((rc.bottom - rc.top) / lineH);
    DWORD totalLines = static_cast<DWORD>(m_lineIndex.GetLineCount());

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    si.nMin = 0;
    si.nMax = totalLines > 0 ? static_cast<int>(totalLines - 1) : 0;
    si.nPage = pageLines > 0 ? static_cast<UINT>(pageLines) : 1;
    si.nPos = static_cast<int>(m_scrollLine);
    SetScrollInfo(m_hwnd, SB_VERT, &si, TRUE);
}

void CEditorWindow::ScrollToLine(DWORD line)
{
    if (!m_buffer)
        return;
    DWORD totalLines = static_cast<DWORD>(m_lineIndex.GetLineCount());
    if (totalLines == 0)
        line = 0;
    if (line >= totalLines)
        line = totalLines > 0 ? totalLines - 1 : 0;

    if (line != m_scrollLine)
    {
        m_scrollLine = line;
        UpdateScrollBar();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void CEditorWindow::OnScroll(WPARAM wParam, LPARAM)
{
    if (!m_buffer)
        return;

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    GetScrollInfo(m_hwnd, SB_VERT, &si);

    int pos = si.nPos;
    switch (LOWORD(wParam))
    {
    case SB_TOP:           pos = si.nMin; break;
    case SB_BOTTOM:        pos = si.nMax; break;
    case SB_LINEUP:        pos -= 1; break;
    case SB_LINEDOWN:      pos += 1; break;
    case SB_PAGEUP:        pos -= static_cast<int>(si.nPage); break;
    case SB_PAGEDOWN:      pos += static_cast<int>(si.nPage); break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION:
        pos = si.nTrackPos; break;
    default: return;
    }

    ScrollToLine(static_cast<DWORD>(pos));
}

void CEditorWindow::OnMouseClick(LPARAM lParam)
{
    if (!m_buffer)
        return;

    int x = GET_X_LPARAM(lParam);
    int y = GET_Y_LPARAM(lParam);

    RECT sb;
    if (m_hStatusBar && IsWindowVisible(m_hStatusBar))
    {
        GetWindowRect(m_hStatusBar, &sb);
        int sbTop;
        POINT p{ x, y };
        ClientToScreen(m_hwnd, &p);
        if (p.y >= sb.top)
            return;   // 点击在状态栏上
    }

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    int clientWidth = rc.right - rc.left;

    std::vector<CRenderer::Row> rows;
    BuildVisibleRows(rows);

    int lineHeight = static_cast<int>(m_renderer->GetLineHeight());
    DWORD row = 0, col = 0;
    if (m_renderer->HitTestPoint(rows, lineHeight, clientWidth,
                                 static_cast<float>(x), static_cast<float>(y),
                                 &row, &col))
    {
        m_caretRow = row;
        m_caretCol = col;
        UpdateStatusBar();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void CEditorWindow::MoveCaret(INT dRow, INT dCol)
{
    if (!m_buffer)
        return;

    DWORD totalLines = static_cast<DWORD>(m_lineIndex.GetLineCount());
    if (totalLines == 0)
    {
        m_caretRow = 0;
        m_caretCol = 0;
        return;
    }

    if (dRow != 0)
    {
        INT newRow = static_cast<INT>(m_caretRow) + dRow;
        if (newRow < 0) newRow = 0;
        if (static_cast<DWORD>(newRow) >= totalLines) newRow = totalLines - 1;
        m_caretRow = static_cast<DWORD>(newRow);

        // 换行后裁剪列到行尾
        std::wstring line = GetLineText(m_caretRow);
        if (m_caretCol > line.size())
            m_caretCol = static_cast<DWORD>(line.size());
    }
    else if (dCol != 0)
    {
        std::wstring line = GetLineText(m_caretRow);
        INT newCol = static_cast<INT>(m_caretCol) + dCol;
        if (newCol < 0) newCol = 0;
        if (static_cast<DWORD>(newCol) > line.size()) newCol = static_cast<INT>(line.size());
        m_caretCol = static_cast<DWORD>(newCol);
    }

    EnsureCaretVisible();
    UpdateStatusBar();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CEditorWindow::EnsureCaretVisible()
{
    if (!m_buffer)
        return;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (m_hStatusBar && IsWindowVisible(m_hStatusBar))
    {
        RECT sb;
        GetWindowRect(m_hStatusBar, &sb);
        rc.bottom -= (sb.bottom - sb.top);
    }
    float lineH = m_renderer ? m_renderer->GetLineHeight() : 20.0f;
    int visibleCount = static_cast<int>((rc.bottom - rc.top) / lineH);

    if (m_caretRow < m_scrollLine)
        ScrollToLine(m_caretRow);
    else if (m_caretRow >= m_scrollLine + visibleCount)
        ScrollToLine(m_caretRow - visibleCount + 1);
}

void CEditorWindow::OnKeyDown(WPARAM wParam)
{
    switch (wParam)
    {
    case VK_LEFT:  MoveCaret(0, -1); break;
    case VK_RIGHT: MoveCaret(0, 1); break;
    case VK_UP:    MoveCaret(-1, 0); break;
    case VK_DOWN:  MoveCaret(1, 0); break;
    case VK_HOME:  m_caretCol = 0; EnsureCaretVisible(); UpdateStatusBar(); InvalidateRect(m_hwnd, nullptr, FALSE); break;
    case VK_END:
    {
        std::wstring line = GetLineText(m_caretRow);
        m_caretCol = static_cast<DWORD>(line.size());
        EnsureCaretVisible();
        UpdateStatusBar();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        break;
    }
    default:
        break;
    }
}