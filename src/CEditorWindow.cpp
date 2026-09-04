// 主编辑器窗口：消息循环、菜单/状态栏、虚拟滚动、光标/选区、
// 键盘编辑(含 IME)、撤销/重做与文件保存
#include "CEditorWindow.h"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <imm.h>

namespace
{
    constexpr wchar_t kWindowClassName[] = L"TextEditorMainWindow";
    constexpr wchar_t kWindowTitle[] = L"无标题 - 文本编辑器";

    constexpr int kNewId       = 101;
    constexpr int kOpenId      = 102;
    constexpr int kSaveId      = 103;
    constexpr int kSaveAsId    = 104;
    constexpr int kExitId      = 105;
    constexpr int kUndoId      = 111;
    constexpr int kRedoId      = 112;
    constexpr int kStatusBarId = 301;

    constexpr UINT_PTR kCaretTimer = 1;
    constexpr UINT      kCaretBlinkMs = 530;

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

    bool IsHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
    bool IsLowSurrogate(wchar_t c)  { return c >= 0xDC00 && c <= 0xDFFF; }

    // 统计 UTF-16 文本中的换行次数（\r\n 视为一次）
    int CountLineBreaks(const std::wstring& s)
    {
        int n = 0;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == L'\r')
            {
                ++n;
                if (i + 1 < s.size() && s[i + 1] == L'\n')
                    ++i;
            }
            else if (s[i] == L'\n')
            {
                ++n;
            }
        }
        return n;
    }

    // 回退一个 code point 的起始列
    DWORD PrevCodePointCol(const std::wstring& line, DWORD col)
    {
        if (col == 0)
            return 0;
        size_t i = col;
        --i;
        while (i > 0 && IsLowSurrogate(line[i]))
            --i;
        return static_cast<DWORD>(i);
    }

    // 前进一个 code point（跳过代理对）
    DWORD NextCodePointCol(const std::wstring& line, DWORD col)
    {
        if (col >= line.size())
            return static_cast<DWORD>(line.size());
        size_t n = (IsHighSurrogate(line[col]) && col + 1 < line.size() &&
                    IsLowSurrogate(line[col + 1])) ? 2 : 1;
        return col + static_cast<DWORD>(n);
    }

    // 判断 code point 是否属于"词字符"（用于双击选词）
    bool IsWordChar(wchar_t c)
    {
        if (IsHighSurrogate(c) || IsLowSurrogate(c))
            return false;
        return (iswalnum(c) != 0) || c == L'_';
    }
}

CEditorWindow::CEditorWindow()
    : m_hInstance(nullptr)
    , m_hwnd(nullptr)
    , m_hStatusBar(nullptr)
    , m_hMenu(nullptr)
    , m_renderer(std::make_unique<CRenderer>())
    , m_encoding(Encoding::Utf8)
    , m_bomBytes(0)
    , m_dirty(false)
    , m_scrollLine(0)
    , m_caretRow(0)
    , m_caretCol(0)
    , m_selAnchorValid(false)
    , m_selAnchorRow(0)
    , m_selAnchorCol(0)
    , m_selCaretRow(0)
    , m_selCaretCol(0)
    , m_caretVisible(true)
    , m_hasFocus(false)
{
}

CEditorWindow::~CEditorWindow()
{
}

void CEditorWindow::RegisterWindowClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
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

    HMENU hMenubar = CreateMenu();

    HMENU hFile = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING, kNewId,    L"新建(&N)\tCtrl+N");
    AppendMenuW(hFile, MF_STRING, kOpenId,   L"打开(&O)\tCtrl+O");
    AppendMenuW(hFile, MF_STRING, kSaveId,   L"保存(&S)\tCtrl+S");
    AppendMenuW(hFile, MF_STRING, kSaveAsId, L"另存为(&A)\tCtrl+Shift+S");
    AppendMenuW(hFile, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hFile, MF_STRING, kExitId,   L"退出(&X)\tAlt+F4");
    AppendMenuW(hMenubar, MF_POPUP, reinterpret_cast<UINT_PTR>(hFile), L"文件(&F)");

    HMENU hEdit = CreatePopupMenu();
    AppendMenuW(hEdit, MF_STRING, kUndoId, L"撤销(&U)\tCtrl+Z");
    AppendMenuW(hEdit, MF_STRING, kRedoId, L"重做(&R)\tCtrl+Y");
    AppendMenuW(hMenubar, MF_POPUP, reinterpret_cast<UINT_PTR>(hEdit), L"编辑(&E)");

    HMENU hView = CreatePopupMenu();
    AppendMenuW(hView, MF_STRING | MF_CHECKED, kStatusBarId, L"状态栏(&S)");
    AppendMenuW(hMenubar, MF_POPUP, reinterpret_cast<UINT_PTR>(hView), L"查看(&V)");

    m_hMenu = hMenubar;

    m_hwnd = CreateWindowExW(
        0, kWindowClassName, kWindowTitle,
        WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN,
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

    case WM_SETFOCUS:
        m_hasFocus = true;
        m_caretVisible = true;
        SetTimer(m_hwnd, kCaretTimer, kCaretBlinkMs, nullptr);
        InvalidateEditor();
        return 0;

    case WM_KILLFOCUS:
        m_hasFocus = false;
        m_caretVisible = false;
        KillTimer(m_hwnd, kCaretTimer);
        InvalidateEditor();
        return 0;

    case WM_TIMER:
        OnTimer();
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
        int steps = -delta / WHEEL_DELTA;
        long newLine = static_cast<long>(m_scrollLine) + steps * 3;
        if (newLine < 0)
            newLine = 0;
        DWORD total = static_cast<DWORD>(m_lineIndex.GetLineCount());
        if (total > 0 && static_cast<DWORD>(newLine) >= total)
            newLine = total - 1;
        ScrollToLine(static_cast<DWORD>(newLine));
        return 0;
    }

    case WM_LBUTTONDOWN:
        SetFocus(m_hwnd);
        SetCapture(m_hwnd);
        OnMouseClick(wParam, lParam, 1);
        return 0;

    case WM_LBUTTONDBLCLK:
        SetFocus(m_hwnd);
        OnMouseClick(wParam, lParam, 2);
        return 0;

    case WM_LBUTTONUP:
        if (GetCapture() == m_hwnd)
            ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
        if (wParam & MK_LBUTTON)
            OnMouseDrag(lParam);
        return 0;

    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;

    case WM_CHAR:
        OnChar(static_cast<wchar_t>(wParam));
        return 0;

    case WM_IME_CHAR:
        // 中文 IME 提交的文字经 WM_IME_CHAR 送达（宽字符 wParam）
        OnChar(static_cast<wchar_t>(wParam));
        return 0;

    case WM_ERASEBKGND:
        return 1;   // D2D 全量绘制

    case WM_CLOSE:
        Destroy();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------- 命令（菜单） ----------------

void CEditorWindow::OnCommand(WORD commandId)
{
    switch (commandId)
    {
    case kNewId:
        OpenFile(L"");
        break;

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
    case kSaveId:
        if (m_filePath.empty())
        {
            // 无路径 → 走另存为
            wchar_t path[MAX_PATH * 4] = { 0 };
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = m_hwnd;
            ofn.lpstrFilter = L"所有文件(*.*)\0*.*\0文本文件(*.txt)\0*.txt\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH * 4;
            ofn.Flags = OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&ofn))
                SaveFile(path);
        }
        else
        {
            SaveFile(m_filePath.c_str());
        }
        break;

    case kSaveAsId:
    {
        wchar_t path[MAX_PATH * 4] = { 0 };
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hwnd;
        ofn.lpstrFilter = L"所有文件(*.*)\0*.*\0文本文件(*.txt)\0*.txt\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH * 4;
        ofn.Flags = OFN_PATHMUSTEXIST;
        if (GetSaveFileNameW(&ofn))
            SaveFile(path);
        break;
    }
    case kExitId:
        Destroy();
        break;

    case kUndoId:
        Undo();
        break;

    case kRedoId:
        Redo();
        break;

    case kStatusBarId:
    {
        bool checked = !m_hStatusBar;
        CheckMenuItem(m_hMenu, kStatusBarId, checked ? MF_CHECKED : MF_UNCHECKED);
        if (checked)
        {
            m_hStatusBar = CreateStatusWindowW(WS_CHILD | WS_VISIBLE, L"就绪", m_hwnd, kStatusBarId);
            int parts[3] = { 260, 520, -1 };
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

// ---------------- 文件打开 / 保存 ----------------

void CEditorWindow::RebuildDocument()
{
    // 原文件只读字节交 PieceTable 作 Original 片段；编辑沉淀为 Added 片段
    if (m_buffer)
    {
        const BYTE* base = m_buffer->GetBasePtr();
        uint64_t size = static_cast<uint64_t>(m_buffer->GetSize());
        m_encoding = m_buffer->GetEncoding();
        m_bomBytes = 0;

        // 探测 BOM 字节数（GetLineText 会剥离 U+FEFF 显示字符；偏移换算需补回）
        if (size >= 3 && base[0] == 0xEF && base[1] == 0xBB && base[2] == 0xBF)
            m_bomBytes = 3;
        else if (size >= 2 && ((base[0] == 0xFF && base[1] == 0xFE) ||
                               (base[0] == 0xFE && base[1] == 0xFF)))
            m_bomBytes = 2;

        m_piece.SetOriginal(base, size);
        m_lineIndex.Build(
            [this](uint64_t ofs, unsigned char* dst, uint64_t maxLen) -> uint64_t {
                return DocRead(ofs, dst, maxLen);
            },
            m_piece.Size(), m_encoding);
    }
    else
    {
        // 空文档（新建）：视为 1 个空行
        m_encoding = Encoding::Utf8;
        m_bomBytes = 0;
        m_piece.SetOriginal(nullptr, 0);
        m_lineIndex.Build(
            [this](uint64_t ofs, unsigned char* dst, uint64_t maxLen) -> uint64_t {
                return DocRead(ofs, dst, maxLen);
            },
            0, m_encoding);
    }
}

BOOL CEditorWindow::OpenFile(LPCWSTR szPath)
{
    // 新建空白文档
    if (!szPath || !szPath[0])
    {
        m_buffer.reset();
        m_filePath.clear();
        m_dirty = false;
        RebuildDocument();
        m_scrollLine = 0;
        m_caretRow = 0;
        m_caretCol = 0;
        m_selAnchorValid = false;
        UpdateTitle();
        UpdateStatusBar();
        UpdateScrollBar();
        InvalidateEditor();
        return TRUE;
    }

    auto buf = std::make_unique<CTextBuffer>();
    if (!buf->OpenFile(szPath))
    {
        wchar_t msg[1024];
        wsprintfW(msg, L"无法打开文件:\n%s", szPath);
        MessageBoxW(m_hwnd, msg, L"错误", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    m_buffer = std::move(buf);
    m_filePath = szPath;
    m_dirty = false;
    RebuildDocument();

    m_scrollLine = 0;
    m_caretRow = 0;
    m_caretCol = 0;
    m_selAnchorValid = false;

    UpdateTitle();
    UpdateStatusBar();
    UpdateScrollBar();
    InvalidateEditor();
    return TRUE;
}

BOOL CEditorWindow::SaveFile(LPCWSTR szPath)
{
    if (!szPath || !szPath[0])
        return FALSE;

    // 内容 = PieceTable 逻辑字节（原文 + 追加，仍为原编码字节流）
    std::vector<unsigned char> content(static_cast<size_t>(m_piece.Size()));
    if (!content.empty())
        m_piece.CopyOut(content.data(), content.size());

    // 写临时文件后原子替换
    std::wstring tmpPath = szPath;
    tmpPath += L".tmp";

    HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        MessageBoxW(m_hwnd, L"无法写入文件（权限或占用）", L"错误", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    DWORD written = 0;
    BOOL ok = content.empty() ||
        WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    if (ok)
        ok = FlushFileBuffers(hFile);
    CloseHandle(hFile);

    if (!ok || written != content.size())
    {
        DeleteFileW(tmpPath.c_str());
        MessageBoxW(m_hwnd, L"写入文件失败", L"错误", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    if (!MoveFileExW(tmpPath.c_str(), szPath,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(tmpPath.c_str());
        MessageBoxW(m_hwnd, L"替换文件失败", L"错误", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    m_dirty = false;
    m_filePath = szPath;
    UpdateTitle();
    UpdateStatusBar();
    return TRUE;
}

// ---------------- 标题 / 状态栏 ----------------

void CEditorWindow::UpdateTitle()
{
    std::wstring title = m_filePath.empty()
        ? kWindowTitle
        : (m_filePath + L" - 文本编辑器");
    if (m_dirty)
        title.insert(0, L"* ");
    SetWindowTextW(m_hwnd, title.c_str());
}

void CEditorWindow::UpdateStatusBar()
{
    if (!m_hStatusBar)
        return;

    if (!m_lineIndex.IsValid())
    {
        SendMessageW(m_hStatusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"就绪"));
        SendMessageW(m_hStatusBar, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(L""));
        SendMessageW(m_hStatusBar, SB_SETTEXTW, 2, reinterpret_cast<LPARAM>(L""));
        return;
    }

    std::wstringstream pos;
    pos << L"行 " << (m_caretRow + 1) << L", 列 " << (m_caretCol + 1)
        << L" | 总行 " << m_lineIndex.GetLineCount()
        << (m_dirty ? L" | 已修改" : L"");

    std::wstring encodingText = EncodingName(m_encoding);
    std::wstring sizeText = FormatSize(static_cast<LONGLONG>(m_piece.Size()));

    SendMessageW(m_hStatusBar, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(pos.str().c_str()));
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
    InvalidateEditor();
}

void CEditorWindow::OnCreate(HWND hwnd)
{
    m_hwnd = hwnd;

    m_hStatusBar = CreateStatusWindowW(WS_CHILD | WS_VISIBLE, L"就绪", hwnd, kStatusBarId);
    if (m_hStatusBar)
    {
        int parts[3] = { 260, 520, -1 };
        SendMessageW(m_hStatusBar, SB_SETPARTS, 3, reinterpret_cast<LPARAM>(parts));
    }

    if (m_renderer)
        m_renderer->Init(hwnd);

    RebuildDocument();
    UpdateStatusBar();
    UpdateScrollBar();
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

// ---------------- 渲染 / 滚动 / 光标 ----------------

void CEditorWindow::InvalidateEditor()
{
    if (!m_hwnd)
        return;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (m_hStatusBar && IsWindowVisible(m_hStatusBar))
    {
        RECT sb;
        GetWindowRect(m_hStatusBar, &sb);
        POINT p{ 0, 0 };
        ScreenToClient(m_hwnd, &p);
        rc.bottom = sb.top - p.y;
    }
    if (rc.bottom > rc.top)
        InvalidateRect(m_hwnd, &rc, FALSE);
}

// 该行文本（不含行尾换行；第 0 行剔除 BOM 字符 U+FEFF）
std::wstring CEditorWindow::GetLineText(DWORD row) const
{
    if (!m_lineIndex.IsValid())
        return {};

    uint64_t start = m_lineIndex.GetLineStart(row);
    uint64_t len = m_lineIndex.GetLineLength(row);
    if (len > 1024 * 1024)
        len = 1024 * 1024;   // 防御性上限：单行超大时截断（罕见）

    std::wstring text;
    if (len > 0)
    {
        std::vector<unsigned char> bytes(len);
        DocRead(start, bytes.data(), len);
        text = DecodeToWide(bytes.data(), static_cast<DWORD>(len), m_encoding);
    }

    // 去掉行尾换行
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r'))
        text.pop_back();

    // 首行若为 BOM 文件，剥离解码出的 BOM 字符（显示用；字节偏移换算另行补回）
    if (row == 0 && m_bomBytes > 0 && !text.empty() && text[0] == 0xFEFF)
        text.erase(text.begin());

    return text;
}

void CEditorWindow::BuildVisibleRows(std::vector<CRenderer::Row>& rows) const
{
    rows.clear();
    if (!m_lineIndex.IsValid())
        return;

    DWORD totalLines = static_cast<DWORD>(m_lineIndex.GetLineCount());
    if (totalLines == 0)
        return;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (m_hStatusBar && IsWindowVisible(m_hStatusBar))
    {
        RECT sb;
        GetWindowRect(m_hStatusBar, &sb);
        POINT p{ 0, 0 };
        ScreenToClient(m_hwnd, &p);
        rc.bottom = sb.top - p.y;
    }

    float lineH = m_renderer ? m_renderer->GetLineHeight() : 20.0f;
    int visibleCount = static_cast<int>((rc.bottom - rc.top) / lineH) + 2;
    if (visibleCount < 1)
        visibleCount = 1;

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
    if (m_hStatusBar && IsWindowVisible(m_hStatusBar))
    {
        RECT sb;
        GetWindowRect(m_hStatusBar, &sb);
        POINT p{ 0, 0 };
        ScreenToClient(m_hwnd, &p);
        rc.bottom = sb.top - p.y;
    }

    std::vector<CRenderer::Row> rows;
    BuildVisibleRows(rows);

    int lineHeight = static_cast<int>(m_renderer->GetLineHeight());
    m_renderer->Render(rows, lineHeight, rc.right - rc.left,
                       m_caretRow, m_caretCol, m_caretVisible && m_hasFocus,
                       GetRenderSelection());

    EndPaint(m_hwnd, &ps);
}

void CEditorWindow::UpdateScrollBar()
{
    if (!m_hwnd || !m_lineIndex.IsValid())
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
        POINT p{ 0, 0 };
        ScreenToClient(m_hwnd, &p);
        rc.bottom = sb.top - p.y;
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
    if (!m_lineIndex.IsValid())
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
        InvalidateEditor();
    }
}

void CEditorWindow::OnScroll(WPARAM wParam, LPARAM)
{
    if (!m_lineIndex.IsValid())
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

void CEditorWindow::MoveCaretTo(int row, int col, bool extendSelection)
{
    if (!m_lineIndex.IsValid())
        return;
    DWORD totalLines = static_cast<DWORD>(m_lineIndex.GetLineCount());
    if (totalLines == 0)
        return;

    if (row < 0) row = 0;
    if (row >= static_cast<int>(totalLines)) row = static_cast<int>(totalLines) - 1;
    if (col < 0) col = 0;

    // 非 shift 移动 → 丢弃旧锚点
    if (!extendSelection)
    {
        m_selAnchorValid = false;
        m_selAnchorRow = m_caretRow;
        m_selAnchorCol = m_caretCol;
    }
    else if (!m_selAnchorValid)
    {
        m_selAnchorValid = true;
        m_selAnchorRow = m_caretRow;
        m_selAnchorCol = m_caretCol;
    }

    m_caretRow = static_cast<DWORD>(row);
    m_caretCol = static_cast<DWORD>(col);
    m_selCaretRow = m_caretRow;
    m_selCaretCol = m_caretCol;

    std::wstring line = GetLineText(m_caretRow);
    if (m_caretCol > line.size())
        m_caretCol = static_cast<DWORD>(line.size());
    m_selCaretCol = m_caretCol;

    EnsureCaretVisible();
    UpdateStatusBar();
    InvalidateEditor();
}

void CEditorWindow::MoveCaret(INT dRow, INT dCol, bool extendSelection)
{
    if (!m_lineIndex.IsValid())
        return;

    if (dRow == 0 && dCol == 0)
        return;

    DWORD totalLines = static_cast<DWORD>(m_lineIndex.GetLineCount());
    if (totalLines == 0)
        return;

    int newRow = static_cast<int>(m_caretRow) + dRow;
    int newCol = static_cast<int>(m_caretCol);

    if (dRow != 0)
    {
        // 纵向移动：列沿用旧列（向上/下尽量保持），到行尾则截断
        if (newRow < 0) newRow = 0;
        if (newRow >= static_cast<int>(totalLines)) newRow = static_cast<int>(totalLines) - 1;
        m_caretRow = static_cast<DWORD>(newRow);
        std::wstring line = GetLineText(m_caretRow);
        if (newCol > static_cast<int>(line.size()))
            newCol = static_cast<int>(line.size());
    }
    else
    {
        // 横向移动：code point 级（跳过代理对）
        std::wstring line = GetLineText(m_caretRow);
        if (dCol > 0)
            newCol = static_cast<int>(NextCodePointCol(line, m_caretCol));
        else
            newCol = static_cast<int>(PrevCodePointCol(line, m_caretCol));
    }

    MoveCaretTo(newRow, newCol, extendSelection);
}

void CEditorWindow::EnsureCaretVisible()
{
    if (!m_lineIndex.IsValid())
        return;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (m_hStatusBar && IsWindowVisible(m_hStatusBar))
    {
        RECT sb;
        GetWindowRect(m_hStatusBar, &sb);
        POINT p{ 0, 0 };
        ScreenToClient(m_hwnd, &p);
        rc.bottom = sb.top - p.y;
    }
    float lineH = m_renderer ? m_renderer->GetLineHeight() : 20.0f;
    int visibleCount = static_cast<int>((rc.bottom - rc.top) / lineH);

    if (m_caretRow < m_scrollLine)
        ScrollToLine(m_caretRow);
    else if (m_caretRow >= m_scrollLine + visibleCount)
        ScrollToLine(m_caretRow - visibleCount + 1);
}

void CEditorWindow::OnTimer()
{
    if (!m_hasFocus)
        return;
    m_caretVisible = !m_caretVisible;
    InvalidateEditor();
}

// ---------------- 选区 ----------------

bool CEditorWindow::HasSelection() const
{
    return m_selAnchorValid &&
           (m_selAnchorRow != m_selCaretRow || m_selAnchorCol != m_selCaretCol);
}

void CEditorWindow::NormalizeSelection()
{
    if (!m_selAnchorValid)
        return;
    if (m_selAnchorRow > m_selCaretRow ||
        (m_selAnchorRow == m_selCaretRow && m_selAnchorCol > m_selCaretCol))
    {
        std::swap(m_selAnchorRow, m_selCaretRow);
        std::swap(m_selAnchorCol, m_selCaretCol);
    }
}

CRenderer::Selection CEditorWindow::GetRenderSelection() const
{
    CRenderer::Selection sel{};
    sel.active = HasSelection();
    if (!sel.active)
        return sel;

    DWORD sRow = m_selAnchorRow, sCol = m_selAnchorCol;
    DWORD eRow = m_selCaretRow, eCol = m_selCaretCol;
    if (sRow > eRow || (sRow == eRow && sCol > eCol))
    {
        std::swap(sRow, eRow);
        std::swap(sCol, eCol);
    }
    sel.startRow = sRow;
    sel.startCol = sCol;
    sel.endRow = eRow;
    sel.endCol = eCol;
    return sel;
}

void CEditorWindow::BeginSelectionAnchor()
{
    m_selAnchorValid = true;
    m_selAnchorRow = m_caretRow;
    m_selAnchorCol = m_caretCol;
}

void CEditorWindow::SelectWordAt(DWORD row, DWORD col)
{
    std::wstring line = GetLineText(row);
    if (line.empty())
        return;

    DWORD start = col, end = col;
    while (start > 0 && IsWordChar(line[start - 1]))
        --start;
    while (end < line.size() && IsWordChar(line[end]))
        ++end;

    if (start == end)
        return;   // 空白处双击 → 只移动光标

    m_selAnchorValid = true;
    m_selAnchorRow = row;
    m_selAnchorCol = start;
    m_selCaretRow = row;
    m_selCaretCol = end;
    m_caretRow = row;
    m_caretCol = end;
    UpdateStatusBar();
    InvalidateEditor();
}

void CEditorWindow::SelectLineAt(DWORD row)
{
    m_selAnchorValid = true;
    m_selAnchorRow = row;
    m_selAnchorCol = 0;
    m_selCaretRow = row;
    m_selCaretCol = static_cast<DWORD>(GetLineText(row).size());
    m_caretRow = row;
    m_caretCol = m_selCaretCol;
    UpdateStatusBar();
    InvalidateEditor();
}

// ---------------- 文本坐标换算 ----------------

uint64_t CEditorWindow::DocByteSize() const
{
    return m_piece.Size();
}

unsigned char CEditorWindow::DocByteAt(uint64_t ofs) const
{
    return m_piece.At(ofs);
}

uint64_t CEditorWindow::DocRead(uint64_t ofs, unsigned char* dst, uint64_t maxLen) const
{
    return m_piece.ReadRange(ofs, dst, maxLen);
}

uint64_t CEditorWindow::DocOffsetToRow(uint64_t ofs) const
{
    return m_lineIndex.ByteOffsetToRow(ofs);
}

// (row, col) → 文档逻辑字节偏移
// col 是"不含行尾换行"的显示文本列；首行还有 BOM（m_bomBytes）位于行首字节
uint64_t CEditorWindow::PosToByte(DWORD row, DWORD col) const
{
    if (!m_lineIndex.IsValid())
        return 0;
    DWORD total = static_cast<DWORD>(m_lineIndex.GetLineCount());
    if (row >= total)
        row = total > 0 ? total - 1 : 0;

    std::wstring line = GetLineText(row);
    if (col > line.size())
        col = static_cast<DWORD>(line.size());

    std::string prefix = EncodeFromWide(line.substr(0, col), m_encoding);

    // 该行"显示内容"在文件中起点：行首 + 首行 BOM 字节
    uint64_t lineStart = m_lineIndex.GetLineStart(row);
    uint64_t contentStart = lineStart + (row == 0 ? m_bomBytes : 0);
    return contentStart + prefix.size();
}

// 文档逻辑字节偏移 → (row, col)，col 为该行文本的 code unit 列
void CEditorWindow::ByteToPos(uint64_t ofs, DWORD* pRow, DWORD* pCol) const
{
    if (!pRow || !pCol)
        return;
    *pRow = 0;
    *pCol = 0;
    if (!m_lineIndex.IsValid())
        return;

    uint64_t row64 = m_lineIndex.ByteOffsetToRow(ofs);
    DWORD row = static_cast<DWORD>(row64);
    *pRow = row;

    std::wstring line = GetLineText(row);
    uint64_t lineStart = m_lineIndex.GetLineStart(row);
    uint64_t contentStart = lineStart + (row == 0 ? m_bomBytes : 0);

    if (ofs < contentStart)
    {
        *pCol = 0;
        return;
    }

    // 行内内容字节偏移
    uint64_t rel = ofs - contentStart;

    // 从行首起逐 code point 累计编码字节，直到 >= rel
    std::string prefix;
    DWORD col = 0;
    while (col < line.size())
    {
        size_t n = (IsHighSurrogate(line[col]) && col + 1 < line.size() &&
                    IsLowSurrogate(line[col + 1])) ? 2 : 1;
        std::string enc = EncodeFromWide(line.substr(col, n), m_encoding);
        if (prefix.size() + enc.size() > rel)
            break;
        prefix += enc;
        col += static_cast<DWORD>(n);
    }
    *pCol = col;
}

// ---------------- 编辑操作 ----------------

void CEditorWindow::InsertTextAtCaret(const std::wstring& text)
{
    if (text.empty())
        return;
    if (!m_lineIndex.IsValid())
        return;

    // 有选区则先删除
    if (HasSelection())
    {
        CRenderer::Selection sel = GetRenderSelection();
        DeleteRange(sel.startRow, sel.startCol, sel.endRow, sel.endCol);
    }

    uint64_t ofs = PosToByte(m_caretRow, m_caretCol);
    std::string bytes = EncodeFromWide(text, m_encoding);
    if (bytes.empty())
        return;

    m_piece.Insert(ofs, reinterpret_cast<const unsigned char*>(bytes.data()),
                   static_cast<uint32_t>(bytes.size()));
    m_dirty = true;

    // 行结构更新：统计新增行数；纯插入不跨行则快速平移关键帧
    int breaks = CountLineBreaks(text);
    int64_t lineDelta = breaks;
    int64_t byteDelta = static_cast<int64_t>(bytes.size());
    if (lineDelta > 0)
    {
        // 插入包含换行 → 编辑点可能位于行中间，后续行号/偏移变化较大：整帧重建更稳
        m_lineIndex.Build(
            [this](uint64_t o, unsigned char* d, uint64_t m) -> uint64_t {
                return DocRead(o, d, m);
            },
            m_piece.Size(), m_encoding);
    }
    else
    {
        m_lineIndex.NotifyEdit(ofs, byteDelta, 0);
    }

    // 光标移动到插入文本之后
    m_selAnchorValid = false;
    if (breaks == 0)
    {
        m_caretCol = static_cast<DWORD>(m_caretCol + text.size());
    }
    else
    {
        // 移动到最后一个换行之后的那一段尾部
        DWORD row = m_caretRow;
        DWORD col = m_caretCol;
        for (size_t i = 0; i < text.size();)
        {
            size_t n = (IsHighSurrogate(text[i]) && i + 1 < text.size() &&
                        IsLowSurrogate(text[i + 1])) ? 2 : 1;
            if (text[i] == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n')
            {
                ++row;
                col = 0;
                i += 2;
                continue;
            }
            if (text[i] == L'\n' || text[i] == L'\r')
            {
                ++row;
                col = 0;
                i += 1;
                continue;
            }
            col += static_cast<DWORD>(n);
            i += n;
        }
        m_caretRow = row;
        m_caretCol = col;
    }
    m_selCaretRow = m_caretRow;
    m_selCaretCol = m_caretCol;

    UpdateStatusBar();
    EnsureCaretVisible();
    InvalidateEditor();
}

void CEditorWindow::DeleteRange(DWORD startRow, DWORD startCol, DWORD endRow, DWORD endCol)
{
    if (!m_lineIndex.IsValid())
        return;
    if (startRow > endRow || (startRow == endRow && startCol > endCol))
    {
        std::swap(startRow, endRow);
        std::swap(startCol, endCol);
    }

    uint64_t startByte = PosToByte(startRow, startCol);
    uint64_t endByte = PosToByte(endRow, endCol);
    if (endByte <= startByte)
    {
        m_selAnchorValid = false;
        return;
    }
    uint64_t len = endByte - startByte;

    m_piece.Erase(startByte, static_cast<uint32_t>(len));
    m_dirty = true;

    // 删除跨了多少行换行
    uint64_t rowCount = static_cast<uint64_t>(endRow) - startRow;

    // 无论是否跨行，统一重建关键帧以保一致（删除更复杂，直接重建）
    if (rowCount > 0)
    {
        m_lineIndex.Build(
            [this](uint64_t o, unsigned char* d, uint64_t m) -> uint64_t {
                return DocRead(o, d, m);
            },
            m_piece.Size(), m_encoding);
    }
    else
    {
        m_lineIndex.NotifyEdit(startByte, -static_cast<int64_t>(len), 0);
    }

    // 光标回到删除起点
    m_selAnchorValid = false;
    m_caretRow = startRow;
    m_caretCol = startCol;
    m_selCaretRow = startRow;
    m_selCaretCol = startCol;

    // 若删除发生在文件末尾附近，行数收缩后校准
    DWORD total = static_cast<DWORD>(m_lineIndex.GetLineCount());
    if (total == 0)
    {
        m_caretRow = 0;
        m_caretCol = 0;
    }
    else if (m_caretRow >= total)
    {
        m_caretRow = total - 1;
        m_caretCol = static_cast<DWORD>(GetLineText(m_caretRow).size());
    }
    else
    {
        std::wstring line = GetLineText(m_caretRow);
        if (m_caretCol > line.size())
            m_caretCol = static_cast<DWORD>(line.size());
    }
    m_selCaretRow = m_caretRow;
    m_selCaretCol = m_caretCol;

    UpdateStatusBar();
    EnsureCaretVisible();
    InvalidateEditor();
}

void CEditorWindow::Backspace()
{
    if (!m_lineIndex.IsValid())
        return;

    if (HasSelection())
    {
        CRenderer::Selection sel = GetRenderSelection();
        DeleteRange(sel.startRow, sel.startCol, sel.endRow, sel.endCol);
        return;
    }

    std::wstring line = GetLineText(m_caretRow);
    if (m_caretCol > 0)
    {
        DWORD prev = PrevCodePointCol(line, m_caretCol);
        DeleteRange(m_caretRow, prev, m_caretRow, m_caretCol);
        m_caretCol = prev;
        m_selCaretCol = prev;
        return;
    }

    // 行首：与上一行合并（删除换行）
    if (m_caretRow > 0)
    {
        DWORD prevRow = m_caretRow - 1;
        DWORD prevLen = static_cast<DWORD>(GetLineText(prevRow).size());
        DeleteRange(prevRow, prevLen, m_caretRow, 0);
        m_caretRow = prevRow;
        m_caretCol = prevLen;
        m_selCaretRow = prevRow;
        m_selCaretCol = prevLen;
    }
}

void CEditorWindow::Undo()
{
    if (m_piece.Undo())
    {
        m_dirty = true;
        // 重建行索引 + 光标置于改动前位置（用命令内偏移）
        uint64_t ofs = m_piece.UndoOffset();
        m_lineIndex.Build(
            [this](uint64_t o, unsigned char* d, uint64_t m) -> uint64_t {
                return DocRead(o, d, m);
            },
            m_piece.Size(), m_encoding);
        ByteToPos(ofs, &m_caretRow, &m_caretCol);
        m_selAnchorValid = false;
        m_selCaretRow = m_caretRow;
        m_selCaretCol = m_caretCol;
        UpdateStatusBar();
        EnsureCaretVisible();
        InvalidateEditor();
    }
}

void CEditorWindow::Redo()
{
    if (m_piece.Redo())
    {
        m_dirty = true;
        uint64_t ofs = m_piece.RedoOffset();
        m_lineIndex.Build(
            [this](uint64_t o, unsigned char* d, uint64_t m) -> uint64_t {
                return DocRead(o, d, m);
            },
            m_piece.Size(), m_encoding);
        ByteToPos(ofs, &m_caretRow, &m_caretCol);
        m_selAnchorValid = false;
        m_selCaretRow = m_caretRow;
        m_selCaretCol = m_caretCol;
        UpdateStatusBar();
        EnsureCaretVisible();
        InvalidateEditor();
    }
}

// ---------------- 鼠标 / 键盘 / IME ----------------

void CEditorWindow::OnMouseClick(WPARAM wParam, LPARAM lParam, UINT clickCount)
{
    if (!m_lineIndex.IsValid())
        return;

    int x = GET_X_LPARAM(lParam);
    int y = GET_Y_LPARAM(lParam);

    RECT sb;
    if (m_hStatusBar && IsWindowVisible(m_hStatusBar))
    {
        GetWindowRect(m_hStatusBar, &sb);
        POINT p{ x, y };
        ClientToScreen(m_hwnd, &p);
        if (p.y >= sb.top)
            return;   // 状态栏
    }

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    int clientWidth = rc.right - rc.left;

    std::vector<CRenderer::Row> rows;
    BuildVisibleRows(rows);
    if (rows.empty())
        return;

    int lineHeight = static_cast<int>(m_renderer->GetLineHeight());
    DWORD row = 0, col = 0;
    if (!m_renderer->HitTestPoint(rows, lineHeight, clientWidth,
                                  static_cast<float>(x), static_cast<float>(y),
                                  &row, &col))
        return;

    bool shift = (wParam & MK_SHIFT) != 0;

    if (clickCount >= 3)
    {
        // 三击：选整行
        SelectLineAt(row);
        return;
    }
    if (clickCount == 2)
    {
        SelectWordAt(row, col);
        return;
    }

    if (shift)
    {
        // Shift+点击：扩展选区
        if (!m_selAnchorValid)
        {
            m_selAnchorValid = true;
            m_selAnchorRow = m_caretRow;
            m_selAnchorCol = m_caretCol;
        }
        m_caretRow = row;
        m_caretCol = col;
        m_selCaretRow = row;
        m_selCaretCol = col;
    }
    else
    {
        // 单击：设光标 + 锚点
        m_caretRow = row;
        m_caretCol = col;
        m_selAnchorValid = true;
        m_selAnchorRow = row;
        m_selAnchorCol = col;
        m_selCaretRow = row;
        m_selCaretCol = col;
    }

    EnsureCaretVisible();
    UpdateStatusBar();
    InvalidateEditor();
}

void CEditorWindow::OnMouseDrag(LPARAM lParam)
{
    if (!m_lineIndex.IsValid())
        return;

    int x = GET_X_LPARAM(lParam);
    int y = GET_Y_LPARAM(lParam);

    RECT sb;
    if (m_hStatusBar && IsWindowVisible(m_hStatusBar))
    {
        GetWindowRect(m_hStatusBar, &sb);
        POINT p{ x, y };
        ClientToScreen(m_hwnd, &p);
        if (p.y >= sb.top)
            return;
    }

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    int clientWidth = rc.right - rc.left;

    std::vector<CRenderer::Row> rows;
    BuildVisibleRows(rows);
    if (rows.empty())
        return;

    int lineHeight = static_cast<int>(m_renderer->GetLineHeight());
    DWORD row = 0, col = 0;
    if (!m_renderer->HitTestPoint(rows, lineHeight, clientWidth,
                                  static_cast<float>(x), static_cast<float>(y),
                                  &row, &col))
        return;

    if (!m_selAnchorValid)
    {
        m_selAnchorValid = true;
        m_selAnchorRow = m_caretRow;
        m_selAnchorCol = m_caretCol;
    }
    m_caretRow = row;
    m_caretCol = col;
    m_selCaretRow = row;
    m_selCaretCol = col;

    // 拖到视口边缘自动滚动
    int scrollMargin = 8;
    RECT cr;
    GetClientRect(m_hwnd, &cr);
    if (y < scrollMargin && m_scrollLine > 0)
        ScrollToLine(m_scrollLine - 1);
    else if (y > cr.bottom - scrollMargin)
        ScrollToLine(m_scrollLine + 1);

    UpdateStatusBar();
    InvalidateEditor();
}

void CEditorWindow::OnKeyDown(WPARAM wParam)
{
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    if (ctrl)
    {
        switch (wParam)
        {
        case 'S': case 's': OnCommand(kSaveId); return;
        case 'Z': case 'z': Undo(); return;
        case 'Y': case 'y': Redo(); return;
        case 'N': case 'n': OnCommand(kNewId); return;
        case 'O': case 'o': OnCommand(kOpenId); return;
        case 'A': case 'a':
            if (m_lineIndex.IsValid())
            {
                DWORD total = static_cast<DWORD>(m_lineIndex.GetLineCount());
                if (total > 0)
                {
                    m_selAnchorValid = true;
                    m_selAnchorRow = 0;
                    m_selAnchorCol = 0;
                    m_selCaretRow = total - 1;
                    m_selCaretCol = static_cast<DWORD>(GetLineText(total - 1).size());
                    m_caretRow = total - 1;
                    m_caretCol = m_selCaretCol;
                    UpdateStatusBar();
                    InvalidateEditor();
                }
            }
            return;
        default:
            break;
        }
    }

    switch (wParam)
    {
    case VK_LEFT:   MoveCaret(0, -1, shift); break;
    case VK_RIGHT:  MoveCaret(0, 1, shift); break;
    case VK_UP:     MoveCaret(-1, 0, shift); break;
    case VK_DOWN:   MoveCaret(1, 0, shift); break;
    case VK_HOME:   MoveCaretTo(static_cast<int>(m_caretRow), 0, shift); break;
    case VK_END:
        MoveCaretTo(static_cast<int>(m_caretRow),
                    static_cast<int>(GetLineText(m_caretRow).size()), shift);
        break;
    case VK_BACK:
        Backspace();
        break;
    case VK_DELETE:
    {
        if (!m_lineIndex.IsValid())
            break;
        if (HasSelection())
        {
            CRenderer::Selection sel = GetRenderSelection();
            DeleteRange(sel.startRow, sel.startCol, sel.endRow, sel.endCol);
        }
        else
        {
            // 删除光标右侧一个 code point
            std::wstring line = GetLineText(m_caretRow);
            DWORD col = m_caretCol;
            if (col < line.size())
            {
                DWORD next = NextCodePointCol(line, col);
                DeleteRange(m_caretRow, col, m_caretRow, next);
            }
            else if (m_caretRow + 1 < m_lineIndex.GetLineCount())
            {
                DeleteRange(m_caretRow, col, m_caretRow + 1, 0);
            }
        }
        break;
    }
    default:
        break;
    }
}

void CEditorWindow::OnChar(wchar_t ch)
{
    if (!m_lineIndex.IsValid())
        return;

    // 忽略控制字符（Backspace 等在 OnKeyDown 处理）
    if (ch < 0x20 && ch != L'\r')
        return;

    if (ch == L'\r')
    {
        // 换行：与打开文件的行尾风格无关，一律写入 \r\n
        InsertTextAtCaret(L"\r\n");
        return;
    }

    InsertTextAtCaret(std::wstring(1, ch));
}
