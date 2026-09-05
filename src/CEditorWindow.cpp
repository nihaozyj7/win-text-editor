// 主编辑器窗口：消息循环、菜单/状态栏、虚拟滚动、光标/选区、
// 键盘编辑(含 IME)、撤销/重做与文件保存
#include "CEditorWindow.h"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <imm.h>

namespace
{
    constexpr wchar_t kWindowClassName[] = L"TextEditorMainWindow";
    constexpr wchar_t kWindowTitle[] = L"无标题";

    constexpr int kNewId       = 101;
    constexpr int kOpenId      = 102;
    constexpr int kSaveId      = 103;
    constexpr int kSaveAsId    = 104;
    constexpr int kExitId      = 105;
    constexpr int kUndoId      = 111;
    constexpr int kRedoId      = 112;
    constexpr int kCopyId      = 113;
    constexpr int kPasteId     = 114;
    constexpr int kStatusBarId = 301;

    // 查看
    constexpr int kLineNumbersId = 310;   // 显示行号
    constexpr int kWordWrapId    = 311;   // 自动换行
    constexpr int kHideMenuBarId  = 312;   // 隐藏菜单栏（隐藏后 Alt 临时呼出）
    // 设置：内边距
    constexpr int kPaddingNoneId   = 320;
    constexpr int kPaddingSmallId  = 321;
    constexpr int kPaddingMediumId = 322;
    constexpr int kPaddingLargeId  = 323;
    // 设置：行高
    constexpr int kLineHeight10Id = 330;
    constexpr int kLineHeight12Id = 331;
    constexpr int kLineHeight15Id = 332;
    constexpr int kLineHeight20Id = 333;
    // 设置：字体（点击弹出 ChooseFont 设置对应槽位；回退链 主→次→系统）
    constexpr int kFontPrimaryId   = 340;
    constexpr int kFontSecondaryId = 341;
    constexpr int kFontSizeUpId     = 342;   // 增大字号 Ctrl+= / Ctrl+滚轮上
    constexpr int kFontSizeDownId   = 343;   // 减小字号 Ctrl+- / Ctrl+滚轮下
    constexpr int kFontSizeResetId  = 344;   // 重置字号 Ctrl+0
    // 设置：主题
    constexpr int kThemeSystemId = 350;
    constexpr int kThemeLightId  = 351;
    constexpr int kThemeDarkId   = 352;
    constexpr int kAboutId       = 400;
    constexpr int kDefaultEditorId = 401;   // 设为系统文本编辑器（注册/注销）

    constexpr UINT_PTR kCaretTimer = 1;
    constexpr UINT      kCaretBlinkMs = 530;

    // 多实例去重：WM_COPYDATA 查询"该文件是否已打开"的魔数标记（过滤外来消息）
    constexpr ULONG_PTR kFileActivateTag = 0x54455831;   // 'TEX1'

    // 光标行视觉高度累计的扫描上限（行数/字符预算），超过则走快速跳转路径
    constexpr DWORD kVisualScanMaxRows = 120;
    constexpr int   kVisualScanCharBudget = 4 * 1024 * 1024;

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

    // 文件对话框公共配置：txt 为默认类型，未输入扩展名时自动补 .txt
    void SetupFileDialogFilter(OPENFILENAMEW& ofn)
    {
        ofn.lpstrFilter =
            L"文本文件(*.txt)\0*.txt\0"
            L"日志文件(*.log)\0*.log\0"
            L"Markdown(*.md)\0*.md\0"
            L"配置文件(*.ini;*.cfg;*.conf)\0*.ini;*.cfg;*.conf\0"
            L"源代码文件(*.cpp *.h *.py 等)\0*.cpp;*.h;*.hpp;*.c;*.cc;*.cs;*.java;*.js;*.ts;*.py;*.go;*.rs;*.json;*.html;*.css;*.sql\0"
            L"所有文件(*.*)\0*.*\0";
        ofn.lpstrDefExt = L"txt";
        ofn.nFilterIndex = 1;
    }

    // ---- "设为系统文本编辑器"注册表辅助（仅写 HKCU，无需管理员）----
    // 方案：每种语言一个 ProgId（TextEditor.<X>，各自绑定专属图标）→ 给该语言
    // 的扩展名写 OpenWithProgids 候选 → 用户在系统"默认应用"设置页确认。
    // Win10/11 的 UserChoice 由系统哈希保护，程序不可直接改写，这是合规途径。

    // exe 内嵌的语言图标资源 ID（resources/editor.rc，生成自 icons/make_icons.py）
    enum FileIconId
    {
        kFileIconCpp = 100, kFileIconCSharp, kFileIconJava, kFileIconJs,
        kFileIconTs, kFileIconGo, kFileIconRust, kFileIconSwift, kFileIconPhp,
        kFileIconPython, kFileIconJson, kFileIconMarkdown, kFileIconConfig,
        kFileIconHtml, kFileIconCss, kFileIconSql, kFileIconLog, kFileIconText,
    };

    struct EditorFileBinding
    {
        const wchar_t*        progId;   // HKCU\Software\Classes 下的 ProgId 名
        const wchar_t*        label;    // 文件类型显示名
        FileIconId            icon;     // exe 内图标资源
        const wchar_t* const* exts;     // 归属该语言的扩展名
        size_t                extCount;
    };

    // 各语言扩展名表
    const wchar_t* const kExtsCpp[] = {
        L".c", L".h", L".cpp", L".cc", L".cxx", L".c++",
        L".hpp", L".hh", L".hxx" };
    const wchar_t* const kExtsCSharp[]   = { L".cs" };
    const wchar_t* const kExtsJava[]     = { L".java" };
    const wchar_t* const kExtsJs[]       = { L".js", L".mjs", L".cjs", L".jsx" };
    const wchar_t* const kExtsTs[]       = { L".ts", L".tsx" };
    const wchar_t* const kExtsGo[]       = { L".go" };
    const wchar_t* const kExtsRust[]     = { L".rs" };
    const wchar_t* const kExtsSwift[]    = { L".swift" };
    const wchar_t* const kExtsPhp[]      = { L".php" };
    const wchar_t* const kExtsPython[]   = { L".py", L".pyw", L".pyi" };
    const wchar_t* const kExtsJson[]     = { L".json", L".jsonc", L".json5" };
    const wchar_t* const kExtsMarkdown[] = { L".md", L".markdown", L".mdown", L".mkd" };
    const wchar_t* const kExtsConfig[]   = {
        L".yml", L".yaml", L".toml", L".ini",
        L".cfg", L".conf", L".properties", L".env" };
    const wchar_t* const kExtsHtml[]     = { L".html", L".htm", L".xhtml", L".xml" };
    const wchar_t* const kExtsCss[]      = { L".css", L".scss", L".less" };
    const wchar_t* const kExtsSql[]      = { L".sql" };
    const wchar_t* const kExtsLog[]      = { L".log" };
    const wchar_t* const kExtsText[]     = { L".txt" };

    const EditorFileBinding kFileBindings[] = {
        { L"TextEditor.CCpp",     L"C/C++ 源文件",    kFileIconCpp,      kExtsCpp,      ARRAYSIZE(kExtsCpp) },
        { L"TextEditor.CSharp",   L"C# 源文件",       kFileIconCSharp,   kExtsCSharp,   ARRAYSIZE(kExtsCSharp) },
        { L"TextEditor.Java",     L"Java 源文件",     kFileIconJava,     kExtsJava,     ARRAYSIZE(kExtsJava) },
        { L"TextEditor.Js",       L"JavaScript 文件", kFileIconJs,       kExtsJs,       ARRAYSIZE(kExtsJs) },
        { L"TextEditor.Ts",       L"TypeScript 文件", kFileIconTs,       kExtsTs,       ARRAYSIZE(kExtsTs) },
        { L"TextEditor.Go",       L"Go 源文件",       kFileIconGo,       kExtsGo,       ARRAYSIZE(kExtsGo) },
        { L"TextEditor.Rust",     L"Rust 源文件",     kFileIconRust,     kExtsRust,     ARRAYSIZE(kExtsRust) },
        { L"TextEditor.Swift",    L"Swift 源文件",    kFileIconSwift,    kExtsSwift,    ARRAYSIZE(kExtsSwift) },
        { L"TextEditor.Php",      L"PHP 源文件",      kFileIconPhp,      kExtsPhp,      ARRAYSIZE(kExtsPhp) },
        { L"TextEditor.Python",   L"Python 源文件",   kFileIconPython,   kExtsPython,   ARRAYSIZE(kExtsPython) },
        { L"TextEditor.Json",     L"JSON 文件",       kFileIconJson,     kExtsJson,     ARRAYSIZE(kExtsJson) },
        { L"TextEditor.Markdown", L"Markdown 文档",   kFileIconMarkdown, kExtsMarkdown, ARRAYSIZE(kExtsMarkdown) },
        { L"TextEditor.Config",   L"配置文件",        kFileIconConfig,   kExtsConfig,   ARRAYSIZE(kExtsConfig) },
        { L"TextEditor.Html",     L"HTML/XML 文档",   kFileIconHtml,     kExtsHtml,     ARRAYSIZE(kExtsHtml) },
        { L"TextEditor.Css",      L"样式表",          kFileIconCss,      kExtsCss,      ARRAYSIZE(kExtsCss) },
        { L"TextEditor.Sql",      L"SQL 脚本",        kFileIconSql,      kExtsSql,      ARRAYSIZE(kExtsSql) },
        { L"TextEditor.Log",      L"日志文件",        kFileIconLog,      kExtsLog,      ARRAYSIZE(kExtsLog) },
        { L"TextEditor.Text",     L"文本文档",        kFileIconText,     kExtsText,     ARRAYSIZE(kExtsText) },
    };

    std::wstring CurrentExePath()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return path;
    }

    std::wstring ProgIdCommandValue()
    {
        return L"\"" + CurrentExePath() + L"\" \"%1\"";
    }

    // 任一 ProgId 已注册且命令指向当前 exe 即视为"已注册"
    bool IsRegisteredAsTextEditor()
    {
        for (const EditorFileBinding& b : kFileBindings)
        {
            std::wstring sub = std::wstring(L"Software\\Classes\\") + b.progId +
                               L"\\shell\\open\\command";
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_READ, &key)
                != ERROR_SUCCESS)
                continue;
            wchar_t buf[MAX_PATH * 2]{};
            DWORD size = sizeof(buf);
            LSTATUS st = RegQueryValueExW(key, nullptr, nullptr, nullptr,
                                          reinterpret_cast<BYTE*>(buf), &size);
            RegCloseKey(key);
            if (st == ERROR_SUCCESS &&
                wcsstr(buf, CurrentExePath().c_str()) != nullptr)
                return true;
        }
        return false;
    }

    // 注册或注销各语言 ProgId 与扩展名的 OpenWithProgids 候选；返回是否成功
    bool RegisterAsTextEditor(bool add)
    {
        const std::wstring cmd = ProgIdCommandValue();
        const std::wstring exe = CurrentExePath();

        for (const EditorFileBinding& b : kFileBindings)
        {
            if (add)
            {
                std::wstring base = std::wstring(L"Software\\Classes\\") + b.progId;
                HKEY key = nullptr;
                // ProgId 显示名
                if (RegCreateKeyExW(HKEY_CURRENT_USER, base.c_str(), 0, nullptr,
                                    REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                    &key, nullptr) != ERROR_SUCCESS)
                    return false;
                const std::wstring label = b.label;
                RegSetValueExW(key, nullptr, 0, REG_SZ,
                               const_cast<BYTE*>(reinterpret_cast<const BYTE*>(label.c_str())),
                               static_cast<DWORD>((label.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(key);
                // shell\open\command
                std::wstring cmdSub = base + L"\\shell\\open\\command";
                if (RegCreateKeyExW(HKEY_CURRENT_USER, cmdSub.c_str(), 0, nullptr,
                                    REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                    &key, nullptr) != ERROR_SUCCESS)
                    return false;
                RegSetValueExW(key, nullptr, 0, REG_SZ,
                               const_cast<BYTE*>(reinterpret_cast<const BYTE*>(cmd.c_str())),
                               static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(key);
                // DefaultIcon = exe 内嵌语言图标（资源 ID 取负）
                std::wstring iconSub = base + L"\\DefaultIcon";
                if (RegCreateKeyExW(HKEY_CURRENT_USER, iconSub.c_str(), 0, nullptr,
                                    REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                    &key, nullptr) == ERROR_SUCCESS)
                {
                    std::wstring icon = L"\"" + exe + L"\",-"
                        + std::to_wstring(static_cast<int>(b.icon));
                    RegSetValueExW(key, nullptr, 0, REG_SZ,
                                   const_cast<BYTE*>(reinterpret_cast<const BYTE*>(icon.c_str())),
                                   static_cast<DWORD>((icon.size() + 1) * sizeof(wchar_t)));
                    RegCloseKey(key);
                }
            }

            // 各扩展名：<ext>\OpenWithProgids 子键下挂 ProgId 候选（值名 = ProgId）
            for (size_t i = 0; i < b.extCount; ++i)
            {
                std::wstring sub = std::wstring(L"Software\\Classes\\") + b.exts[i] +
                                   L"\\OpenWithProgids";
                HKEY key = nullptr;
                if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr,
                                    REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                    &key, nullptr) != ERROR_SUCCESS)
                    continue;
                if (add)
                    RegSetValueExW(key, b.progId, 0, REG_SZ, nullptr, 0);
                else
                    RegDeleteValueW(key, b.progId);
                RegCloseKey(key);
            }

            if (!add)
                RegDeleteTreeW(HKEY_CURRENT_USER,
                               (std::wstring(L"Software\\Classes\\") + b.progId).c_str());
        }

        // 通知 shell 刷新图标/关联缓存
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return true;
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

    // 设置持久化文件：%APPDATA%\TextEditor\settings.ini
    std::wstring SettingsFilePath()
    {
        wchar_t path[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path)))
            return {};
        std::wstring dir = std::wstring(path) + L"\\TextEditor";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\settings.ini";
    }

    // 把窗口矩形拉回最近显示器的工作区：
    // 显示器被拔掉/分辨率变更后，保存的坐标可能整体落到屏幕外
    void ClampRectToWorkArea(RECT* rc)
    {
        HMONITOR hm = MonitorFromRect(rc, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!hm || !GetMonitorInfoW(hm, &mi))
            return;
        const RECT& wa = mi.rcWork;
        int w = rc->right - rc->left;
        int h = rc->bottom - rc->top;
        // 尺寸不超过工作区（换到更小的屏幕）
        if (w > wa.right - wa.left) w = wa.right - wa.left;
        if (h > wa.bottom - wa.top) h = wa.bottom - wa.top;

        int visW = (rc->right < wa.right ? rc->right : wa.right)
                 - (rc->left > wa.left ? rc->left : wa.left);
        int visH = (rc->bottom < wa.bottom ? rc->bottom : wa.bottom)
                 - (rc->top > wa.top ? rc->top : wa.top);
        if (visW < 120 || visH < 60)
        {
            // 主体在屏幕外：整体居中搬回工作区
            rc->left = wa.left + ((wa.right - wa.left) - w) / 2;
            rc->top  = wa.top  + ((wa.bottom - wa.top) - h) / 2;
        }
        else
        {
            // 部分可见：轻推回来，保证标题栏可抓取
            if (rc->top < wa.top)        rc->top  = wa.top;
            if (rc->bottom > wa.bottom)  rc->top  = wa.bottom - h;
            if (rc->right < wa.left + 120) rc->left = wa.left;
            if (rc->left > wa.right - 120) rc->left = wa.right - w;
        }
        rc->right  = rc->left + w;
        rc->bottom = rc->top + h;
    }

    // 统计系统里已存在的同类主窗口（多实例级联摆放用）
    BOOL CALLBACK CountSameClassWndProc(HWND hwnd, LPARAM lp)
    {
        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, 64) && wcscmp(cls, kWindowClassName) == 0)
            ++*reinterpret_cast<int*>(lp);
        return TRUE;
    }

    // ---- 多实例去重：判定"同一文件"并激活已打开它的窗口 ----

    // 路径规范化（展开相对路径 + 忽略大小写），用于同一文件判定。
    // 不解析符号链接/8.3 短名；资源管理器与打开对话框给出的都是绝对长路径
    std::wstring NormalizePathForCompare(const std::wstring& path)
    {
        wchar_t buf[32768]{};
        DWORD n = GetFullPathNameW(path.c_str(), 32768, buf, nullptr);
        if (n == 0 || n >= 32768)
            return path;
        CharLowerBuffW(buf, n);
        return std::wstring(buf, n);
    }

    struct FileActivateEnumCtx
    {
        const std::wstring* path;   // 待查文件（原始路径）
        HWND found;                 // 已打开该文件的窗口
    };

    // 枚举回调：向每个同类可见窗口转发 WM_COPYDATA 查询，有窗口认领即停
    BOOL CALLBACK FindWindowWithFileWndProc(HWND hwnd, LPARAM lp)
    {
        auto* ctx = reinterpret_cast<FileActivateEnumCtx*>(lp);
        wchar_t cls[64]{};
        if (!GetClassNameW(hwnd, cls, 64) || wcscmp(cls, kWindowClassName) != 0)
            return TRUE;
        if (!IsWindowVisible(hwnd))   // 正在销毁/隐藏的窗口不参与
            return TRUE;

        // 传原始路径，由目标进程自行规范化比对（路径数据在对方进程里）
        COPYDATASTRUCT cds{};
        cds.dwData = kFileActivateTag;
        cds.cbData = static_cast<DWORD>((ctx->path->size() + 1) * sizeof(wchar_t));
        cds.lpData = const_cast<wchar_t*>(ctx->path->c_str());

        DWORD_PTR claimed = 0;
        // SMTO_ABORTIFHUNG：目标进程挂死时不拖住发起方
        if (SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
                                SMTO_ABORTIFHUNG, 2000, &claimed) && claimed)
        {
            ctx->found = hwnd;
            return FALSE;
        }
        return TRUE;
    }

    // 读取上次关闭时保存的窗口位置/大小；无记录或记录非法返回 false。
    // 独立于 LoadSettings：窗口位置必须在 CreateWindowExW 之前拿到
    bool LoadWindowPlacement(RECT* rc, bool* maximized)
    {
        std::wstring ini = SettingsFilePath();
        *maximized = false;
        if (ini.empty())
            return false;

        wchar_t buf[32]{};
        auto readInt = [&](LPCWSTR key, int def) {
            GetPrivateProfileStringW(L"Window", key, L"", buf, 32, ini.c_str());
            return buf[0] ? _wtoi(buf) : def;
        };

        int x = readInt(L"X", 0);
        int y = readInt(L"Y", 0);
        int w = readInt(L"Width", 0);
        int h = readInt(L"Height", 0);
        *maximized = readInt(L"Maximized", 0) != 0;

        if (w <= 0 || h <= 0)
            return false;   // 尚无记录
        if (w < 200) w = 200;   // 过小视为脏数据
        if (h < 120) h = 120;

        *rc = { x, y, x + w, y + h };
        ClampRectToWorkArea(rc);
        return true;
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
    , m_savedUndoDepth(0)
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
    , m_showStatusBar(true)
    , m_hideMenuBar(false)
    , m_menuBarTempShown(false)
    , m_altOtherKey(false)
    , m_showLineNumbers(false)
    , m_wordWrap(true)
    , m_lineHeightFactor(1.2f)
    , m_fontSize(14.0f)
    , m_pad{ 8, 8, 8, 8 }
    , m_themeMode(ThemeFollowSystem)
    , m_fontPrimary(L"Consolas")
    , m_fontSecondary(L"微软雅黑")
    , m_hScrollPos(0.0f)
    , m_maxLineWidth(0.0f)
    , m_visualEpochSeen(0)
    , m_visualEpoch(0)
    , m_statusBgBrush(nullptr)
    , m_statusFgColor(RGB(0, 0, 0))
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
    // 加载 resources/editor.rc 中嵌入的应用图标（ID=1），标题栏/任务栏/Alt+Tab 均显示
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm       = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    // 编辑区显示文本 I 形光标（滚动条/状态栏为子窗口，仍用各自类光标箭头）
    wc.hCursor       = LoadCursorW(nullptr, IDC_IBEAM);
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
    AppendMenuW(hEdit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hEdit, MF_STRING, kCopyId,  L"复制(&C)\tCtrl+C");
    AppendMenuW(hEdit, MF_STRING, kPasteId, L"粘贴(&P)\tCtrl+V");
    AppendMenuW(hMenubar, MF_POPUP, reinterpret_cast<UINT_PTR>(hEdit), L"编辑(&E)");

    // 查看菜单：显示开关 + 外观设置（原"设置"菜单已并入）
    HMENU hView = CreatePopupMenu();
    AppendMenuW(hView, MF_STRING, kStatusBarId,    L"状态栏(&S)");
    AppendMenuW(hView, MF_STRING, kLineNumbersId,  L"显示行号(&L)");
    AppendMenuW(hView, MF_STRING, kWordWrapId,     L"自动换行(&W)");
    AppendMenuW(hView, MF_STRING, kHideMenuBarId,  L"隐藏菜单栏(&M)\t（Alt 临时呼出）");
    AppendMenuW(hView, MF_SEPARATOR, 0, nullptr);

    HMENU hPadding = CreatePopupMenu();
    AppendMenuW(hPadding, MF_STRING, kPaddingNoneId,   L"无");
    AppendMenuW(hPadding, MF_STRING, kPaddingSmallId,  L"小 (4px)");
    AppendMenuW(hPadding, MF_STRING, kPaddingMediumId, L"中 (8px)");
    AppendMenuW(hPadding, MF_STRING, kPaddingLargeId,  L"大 (16px)");
    AppendMenuW(hView, MF_POPUP, reinterpret_cast<UINT_PTR>(hPadding), L"内边距(&P)");

    HMENU hLineHeight = CreatePopupMenu();
    AppendMenuW(hLineHeight, MF_STRING, kLineHeight10Id, L"1.0 倍");
    AppendMenuW(hLineHeight, MF_STRING, kLineHeight12Id, L"1.2 倍");
    AppendMenuW(hLineHeight, MF_STRING, kLineHeight15Id, L"1.5 倍");
    AppendMenuW(hLineHeight, MF_STRING, kLineHeight20Id, L"2.0 倍");
    AppendMenuW(hView, MF_POPUP, reinterpret_cast<UINT_PTR>(hLineHeight), L"行高(&H)");

    HMENU hFont = CreatePopupMenu();
    AppendMenuW(hFont, MF_STRING, kFontPrimaryId,   L"主字体(&P)：Consolas");
    AppendMenuW(hFont, MF_STRING, kFontSecondaryId, L"次要字体(&S)：微软雅黑");
    AppendMenuW(hFont, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hFont, MF_STRING, kFontSizeUpId,    L"增大字号(&I)\tCtrl+=");
    AppendMenuW(hFont, MF_STRING, kFontSizeDownId,  L"减小字号(&D)\tCtrl+-");
    AppendMenuW(hFont, MF_STRING, kFontSizeResetId, L"重置字号(&R)\tCtrl+0");
    AppendMenuW(hView, MF_POPUP, reinterpret_cast<UINT_PTR>(hFont), L"字体(&F)");

    HMENU hTheme = CreatePopupMenu();
    AppendMenuW(hTheme, MF_STRING, kThemeSystemId, L"跟随系统(&S)");
    AppendMenuW(hTheme, MF_STRING, kThemeLightId,  L"浅色(&L)");
    AppendMenuW(hTheme, MF_STRING, kThemeDarkId,   L"深色(&D)");
    AppendMenuW(hView, MF_POPUP, reinterpret_cast<UINT_PTR>(hTheme), L"主题(&T)");

    AppendMenuW(hMenubar, MF_POPUP, reinterpret_cast<UINT_PTR>(hView), L"视图(&V)");

    HMENU hHelp = CreatePopupMenu();
    AppendMenuW(hHelp, MF_STRING, kAboutId, L"关于(&A)");
    AppendMenuW(hHelp, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hHelp, MF_STRING, kDefaultEditorId,
                L"设为系统文本编辑器(&D)");
    AppendMenuW(hMenubar, MF_POPUP, reinterpret_cast<UINT_PTR>(hHelp), L"其他(&O)");

    m_hMenu = hMenubar;

    // 上次关闭时的位置/大小；无记录则走默认（900x600 + CW_USEDEFAULT 位置）
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    int w = 900, h = 600;
    bool maximized = false;
    RECT rcPlacement{};
    if (LoadWindowPlacement(&rcPlacement, &maximized))
    {
        x = rcPlacement.left;
        y = rcPlacement.top;
        w = rcPlacement.right - rcPlacement.left;
        h = rcPlacement.bottom - rcPlacement.top;
    }

    // 多实例级联：创建前统计同类窗口数，创建后整体偏移摆放，避免完全重叠
    int existingWindows = 0;
    EnumWindows(CountSameClassWndProc, reinterpret_cast<LPARAM>(&existingWindows));

    m_hwnd = CreateWindowExW(
        0, kWindowClassName, kWindowTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,   // 滚动条用编辑区子控件（不占状态栏行）
        x, y, w, h,
        nullptr, hMenubar, hInstance, this);

    if (!m_hwnd)
        return FALSE;

    // 已有实例时级联偏移（每级 32px，8 级回绕；最大化时无意义跳过）。
    // 在 ShowWindow 之前调整，窗口尚不可见，不会闪动
    if (existingWindows > 0 && !maximized)
    {
        int off = (existingWindows % 8) * 32;
        RECT rc{};
        if (off > 0 && GetWindowRect(m_hwnd, &rc))
        {
            rc.left += off;
            rc.top += off;
            ClampRectToWorkArea(&rc);
            SetWindowPos(m_hwnd, nullptr, rc.left, rc.top, 0, 0,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
        }
    }

    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ShowWindow(m_hwnd, maximized ? SW_SHOWMAXIMIZED : nCmdShow);
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
    case WM_HSCROLL:
        return 0;   // 滚动条已改为自绘（无子控件消息）；滚轮/拖拽走各自分支

    case WM_MOUSEWHEEL:
    {
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (GetKeyState(VK_CONTROL) & 0x8000)
        {
            // Ctrl+滚轮：字号缩放（上滚增大）
            ChangeFontSize(delta > 0 ? +1.0f : -1.0f);
            return 0;
        }
        if (GetKeyState(VK_SHIFT) & 0x8000)
        {
            // Shift+滚轮：水平滚动（像素）
            SetHScrollPos(m_hScrollPos - static_cast<float>(delta));
            return 0;
        }
        int steps = -delta / WHEEL_DELTA;
        long newLine = static_cast<long>(m_scrollLine) + steps * 3;
        if (newLine < 0)
            newLine = 0;
        long maxScroll = MaxScrollLine();
        if (newLine > maxScroll)
            newLine = maxScroll;
        ScrollToLine(static_cast<DWORD>(newLine));
        return 0;
    }

    case WM_LBUTTONDOWN:
        if (m_menuBarTempShown)
            EndTempMenuBar();   // Alt 临时呼出状态下点击编辑区：收回菜单栏
        // 滚动条优先于文本命中（滚动条绘制在编辑区右缘/底缘之上）
        if (OnScrollbarDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))
            return 0;
        SetFocus(m_hwnd);
        SetCapture(m_hwnd);
        OnMouseClick(wParam, lParam, 1);
        return 0;

    case WM_LBUTTONDBLCLK:
        if (OnScrollbarDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))
            return 0;
        SetFocus(m_hwnd);
        OnMouseClick(wParam, lParam, 2);
        return 0;

    case WM_LBUTTONUP:
        OnScrollbarUp();
        if (GetCapture() == m_hwnd)
            ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE:
        OnScrollbarMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (m_barDrag)
            return 0;   // 拖滑块时不做文本选区拖拽
        if (wParam & MK_LBUTTON)
            OnMouseDrag(lParam);
        return 0;

    case WM_MOUSELEAVE:
        m_mouseTracking = false;
        if (m_vBar.hovered || m_hBar.hovered)
        {
            m_vBar.hovered = m_hBar.hovered = false;
            InvalidateEditor();
        }
        return 0;

    case WM_CAPTURECHANGED:
        m_barDrag = 0;   // 捕获被系统夺走（如弹窗），滑块拖拽终止
        return 0;

    case WM_SETCURSOR:
        // 客户区内：滚动条上用箭头，文本区用 I 形光标
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT pt{};
            GetCursorPos(&pt);
            MapWindowPoints(nullptr, m_hwnd, &pt, 1);
            bool overBar = (m_vBar.visible && InRect(m_vBar.track, pt)) ||
                           (m_hBar.visible && InRect(m_hBar.track, pt));
            SetCursor(LoadCursorW(nullptr, overBar ? IDC_ARROW : IDC_IBEAM));
            return TRUE;
        }
        break;

    case WM_DPICHANGED:
    {
        // 显示器/系统缩放变化：换算渲染尺寸并按系统建议矩形调整窗口
        UINT dpiX = LOWORD(wParam);
        ApplyDpiScale();
        ApplyRendererOptions();
        UpdateStatusFont();
        RECT* sug = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(m_hwnd, nullptr, sug->left, sug->top,
                     sug->right - sug->left, sug->bottom - sug->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_SYSKEYDOWN:
        // Alt 按下：菜单栏隐藏时先临时挂回（后续孤立 Alt 抬起会进入菜单键盘模式）
        if (wParam == VK_MENU && m_hideMenuBar && !m_menuBarTempShown && m_hMenu)
        {
            m_menuBarTempShown = true;
            m_altOtherKey = false;
            SetMenu(m_hwnd, m_hMenu);
        }
        else if (m_menuBarTempShown && wParam != VK_MENU)
        {
            m_altOtherKey = true;   // Alt+组合键（助记符/F4/Tab...），不是孤立 Alt
        }
        break;   // 继续 DefWindowProc：Alt+助记符、孤立 Alt 等默认行为

    case WM_SYSKEYUP:
        if (wParam == VK_MENU && m_menuBarTempShown)
        {
            // 先走默认：孤立 Alt 会在 DefWindowProc 内进入菜单栏键盘模式（模态循环），
            // 循环退出时经 WM_MENUSELECT 关闭信号自动收回；
            // Alt+组合键不进入菜单模式 → 返回后仍是临时显示 → 此处直接收回
            DefWindowProcW(hwnd, msg, wParam, lParam);
            if (m_menuBarTempShown)
                EndTempMenuBar();
            return 0;
        }
        break;

    case WM_MENUSELECT:
        // 菜单模式退出信号（Esc/选择命令/点击菜单外）：HIWORD=0xFFFF 且无菜单句柄
        if (m_menuBarTempShown && HIWORD(wParam) == 0xFFFF && lParam == 0)
            EndTempMenuBar();
        break;

    case WM_ACTIVATE:
        // Alt+Tab 切走时 Alt 的 keyup 常被系统吞掉 → 失活兜底收回
        if (LOWORD(wParam) == WA_INACTIVE && m_menuBarTempShown)
            EndTempMenuBar();
        break;

    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;

    case WM_CHAR:
        OnChar(static_cast<wchar_t>(wParam));
        return 0;

    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
        // 中文 IME：组合开始/进行中把组合窗口锚定到光标位置
        UpdateImeCompositionWindow();
        break;   // 继续交给 DefWindowProc 让默认 IME UI 正常工作

    case WM_COPYDATA:
        // 其他实例查询某文件是否已在本窗口打开：命中则置前自己并回 TRUE
        if (OnFileActivateCopyData(lParam))
            return TRUE;
        break;

    case WM_SETTINGCHANGE:
        // 系统主题切换（跟随系统模式下生效）
        if (m_themeMode == ThemeFollowSystem)
            ApplyThemeToWindow();
        break;

    case WM_DRAWITEM:
    {
        // 状态栏分区 owner-draw（主题着色）
        if (wParam == kStatusBarId)
        {
            OnDrawStatusBarPart(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        break;
    }

    case WM_ERASEBKGND:
        return 1;   // D2D 全量绘制

    case WM_CLOSE:
        // 未保存则先确认：保存 / 不保存 / 取消（保存失败视为取消，不关闭）
        if (m_dirty)
        {
            const wchar_t* name = L"无标题";
            std::wstring fileName;
            if (!m_filePath.empty())
            {
                size_t p = m_filePath.find_last_of(L"\\/");
                fileName = (p == std::wstring::npos) ? m_filePath : m_filePath.substr(p + 1);
                name = fileName.c_str();
            }
            wchar_t msg[1024];
            wsprintfW(msg, L"是否将更改保存到\r\n%s？", name);
            int r = MessageBoxW(m_hwnd, msg, L"文本编辑器", MB_YESNOCANCEL | MB_ICONWARNING);
            if (r == IDCANCEL)
                return 0;
            if (r == IDYES && !SaveDocument())
                return 0;   // 保存失败或用户在另存为对话框取消 → 不关闭
        }
        Destroy();
        return 0;

    case WM_DESTROY:
        SaveSettings();   // 退出前记录窗口位置/大小（其余设置本就即时保存）
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
        SetupFileDialogFilter(ofn);
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH * 4;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn))
            OpenFile(path);
        break;
    }
    case kSaveId:
        SaveDocument();
        break;

    case kSaveAsId:
    {
        wchar_t path[MAX_PATH * 4] = { 0 };
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hwnd;
        SetupFileDialogFilter(ofn);
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH * 4;
        ofn.Flags = OFN_PATHMUSTEXIST;
        if (GetSaveFileNameW(&ofn))
            SaveFile(path);
        break;
    }
    case kExitId:
        // 与标题栏关闭走同一分支，未保存时统一弹确认
        SendMessageW(m_hwnd, WM_CLOSE, 0, 0);
        break;

    case kUndoId:
        Undo();
        break;

    case kRedoId:
        Redo();
        break;

    case kCopyId:
        CopySelection();
        break;

    case kPasteId:
        PasteFromClipboard();
        break;

    case kStatusBarId:
    {
        bool checked = !m_hStatusBar;
        CheckMenuItem(m_hMenu, kStatusBarId, checked ? MF_CHECKED : MF_UNCHECKED);
        if (checked)
        {
            m_hStatusBar = CreateStatusWindowW(WS_CHILD | WS_VISIBLE, L"就绪", m_hwnd, kStatusBarId);
            UpdateStatusBarParts();
            UpdateStatusFont();
            ApplyMenuTheme(IsDarkTheme());
            SetWindowTheme(m_hStatusBar, IsDarkTheme() ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        }
        else
        {
            DestroyWindow(m_hStatusBar);
            m_hStatusBar = nullptr;
        }
        m_showStatusBar = checked;
        SaveSettings();
        OnResize();
        UpdateStatusBar();
        break;
    }

    case kLineNumbersId:
        m_showLineNumbers = !m_showLineNumbers;
        SyncMenuChecks();
        ApplyRendererOptions();
        SaveSettings();
        break;

    case kWordWrapId:
        m_wordWrap = !m_wordWrap;
        SyncMenuChecks();
        ApplyRendererOptions();
        SaveSettings();
        break;

    case kHideMenuBarId:
        m_hideMenuBar = !m_hideMenuBar;
        if (!m_hideMenuBar)
            m_menuBarTempShown = false;   // 恢复常驻显示
        ApplyMenuBarState();
        SyncMenuChecks();
        SaveSettings();
        break;

    case kFontSizeUpId:    ChangeFontSize(+1.0f); break;
    case kFontSizeDownId:  ChangeFontSize(-1.0f); break;
    case kFontSizeResetId: ChangeFontSize(0.0f);  break;

    case kPaddingNoneId:   m_pad = { 0, 0, 0, 0 };       SyncMenuChecks(); ApplyRendererOptions(); SaveSettings(); break;
    case kPaddingSmallId:  m_pad = { 4, 4, 4, 4 };       SyncMenuChecks(); ApplyRendererOptions(); SaveSettings(); break;
    case kPaddingMediumId: m_pad = { 8, 8, 8, 8 };       SyncMenuChecks(); ApplyRendererOptions(); SaveSettings(); break;
    case kPaddingLargeId:  m_pad = { 16, 16, 16, 16 };   SyncMenuChecks(); ApplyRendererOptions(); SaveSettings(); break;

    case kLineHeight10Id: m_lineHeightFactor = 1.0f; SyncMenuChecks(); ApplyRendererOptions(); SaveSettings(); break;
    case kLineHeight12Id: m_lineHeightFactor = 1.2f; SyncMenuChecks(); ApplyRendererOptions(); SaveSettings(); break;
    case kLineHeight15Id: m_lineHeightFactor = 1.5f; SyncMenuChecks(); ApplyRendererOptions(); SaveSettings(); break;
    case kLineHeight20Id: m_lineHeightFactor = 2.0f; SyncMenuChecks(); ApplyRendererOptions(); SaveSettings(); break;

    case kFontPrimaryId:
        PickFont(true);
        break;
    case kFontSecondaryId:
        PickFont(false);
        break;

    case kThemeSystemId: m_themeMode = ThemeFollowSystem; SyncMenuChecks(); ApplyThemeToWindow(); SaveSettings(); break;
    case kThemeLightId:  m_themeMode = ThemeLight;        SyncMenuChecks(); ApplyThemeToWindow(); SaveSettings(); break;
    case kThemeDarkId:   m_themeMode = ThemeDark;         SyncMenuChecks(); ApplyThemeToWindow(); SaveSettings(); break;

    case kDefaultEditorId:
    {
        bool reg = !IsRegisteredAsTextEditor();
        if (RegisterAsTextEditor(reg))
        {
            SyncMenuChecks();
            MessageBoxW(m_hwnd,
                reg ? L"已注册为候选文本编辑器（覆盖常见文本/代码/标记格式）。\n\n"
                      L"Windows 10/11 需在系统设置中确认默认应用：\n"
                      L"即将打开「设置 → 应用 → 默认应用」，按扩展名选择「文本编辑器」。\n\n"
                      L"取消勾选菜单项可注销注册。"
                    : L"已注销系统文本编辑器注册。",
                L"设为系统文本编辑器", MB_OK | MB_ICONINFORMATION);
            if (reg)
                ShellExecuteW(m_hwnd, L"open", L"ms-settings:defaultapps",
                              nullptr, nullptr, SW_SHOWNORMAL);
        }
        else
        {
            MessageBoxW(m_hwnd, L"注册表写入失败。", L"设为系统文本编辑器",
                        MB_OK | MB_ICONERROR);
        }
        break;
    }

    case kAboutId:
        MessageBoxW(m_hwnd,
            L"文本编辑器 1.0\n\n"
            L"Win32 + Direct2D/DirectWrite 实现，支持超大文件打开与编辑\n"
            L"（内存映射只读 + 分片表编辑模型 + 稀疏行索引）。\n\n"
            L"快捷键：\n"
            L"  Ctrl+N/O/S/Shift+S   新建/打开/保存/另存为\n"
            L"  Ctrl+Z/Y             撤销/重做\n"
            L"  Ctrl+C/V/A           复制/粘贴/全选\n"
            L"  Ctrl+=/-/0、滚轮     字号增大/减小/重置（Ctrl+滚轮缩放）\n"
            L"  Alt                  菜单栏隐藏时临时呼出",
            L"关于文本编辑器", MB_OK);
        break;

    default:
        break;
    }
}

// ---------------- 设置项应用 ----------------

void CEditorWindow::LoadSettings()
{
    std::wstring ini = SettingsFilePath();
    if (ini.empty())
        return;

    m_showLineNumbers = GetPrivateProfileIntW(L"View", L"ShowLineNumbers", 0, ini.c_str()) != 0;
    m_wordWrap        = GetPrivateProfileIntW(L"View", L"WordWrap", 1, ini.c_str()) != 0;
    m_showStatusBar   = GetPrivateProfileIntW(L"View", L"StatusBar", 1, ini.c_str()) != 0;
    m_hideMenuBar     = GetPrivateProfileIntW(L"View", L"HideMenuBar", 0, ini.c_str()) != 0;

    int theme = GetPrivateProfileIntW(L"Appearance", L"ThemeMode", ThemeFollowSystem, ini.c_str());
    m_themeMode = (theme >= 0 && theme <= 2) ? theme : ThemeFollowSystem;

    int lh = GetPrivateProfileIntW(L"Appearance", L"LineHeightX100", 120, ini.c_str());
    m_lineHeightFactor = lh / 100.0f;
    if (m_lineHeightFactor < 0.8f)  m_lineHeightFactor = 0.8f;
    if (m_lineHeightFactor > 4.0f)  m_lineHeightFactor = 4.0f;

    int pad = GetPrivateProfileIntW(L"Appearance", L"Padding", 8, ini.c_str());
    if (pad < 0)  pad = 0;
    if (pad > 64) pad = 64;
    m_pad = { pad, pad, pad, pad };

    wchar_t buf[260]{};
    GetPrivateProfileStringW(L"Fonts", L"Primary", L"Consolas", buf, 260, ini.c_str());
    m_fontPrimary = buf;
    GetPrivateProfileStringW(L"Fonts", L"Secondary", L"微软雅黑", buf, 260, ini.c_str());
    m_fontSecondary = buf;

    int fs = GetPrivateProfileIntW(L"Fonts", L"FontSize", 14, ini.c_str());
    if (fs < 8)  fs = 8;
    if (fs > 72) fs = 72;
    m_fontSize = static_cast<float>(fs);
}

void CEditorWindow::SaveSettings()
{
    std::wstring ini = SettingsFilePath();
    if (ini.empty())
        return;

    auto writeInt = [&](LPCWSTR section, LPCWSTR key, int value) {
        wchar_t b[16];
        wsprintfW(b, L"%d", value);
        WritePrivateProfileStringW(section, key, b, ini.c_str());
    };

    writeInt(L"View", L"ShowLineNumbers", m_showLineNumbers ? 1 : 0);
    writeInt(L"View", L"WordWrap", m_wordWrap ? 1 : 0);
    writeInt(L"View", L"StatusBar", m_hStatusBar ? 1 : 0);
    writeInt(L"View", L"HideMenuBar", m_hideMenuBar ? 1 : 0);
    writeInt(L"Appearance", L"ThemeMode", m_themeMode);
    writeInt(L"Appearance", L"LineHeightX100",
             static_cast<int>(m_lineHeightFactor * 100.0f + 0.5f));
    writeInt(L"Appearance", L"Padding", m_pad.left);
    WritePrivateProfileStringW(L"Fonts", L"Primary", m_fontPrimary.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Fonts", L"Secondary", m_fontSecondary.c_str(), ini.c_str());
    writeInt(L"Fonts", L"FontSize", static_cast<int>(m_fontSize + 0.5f));

    // 窗口位置/大小：取"还原态"矩形（最大化/最小化时 rcNormalPosition 仍是还原尺寸），
    // 配合 Maximized 标记，下次启动恢复
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (m_hwnd && GetWindowPlacement(m_hwnd, &wp))
    {
        writeInt(L"Window", L"X", wp.rcNormalPosition.left);
        writeInt(L"Window", L"Y", wp.rcNormalPosition.top);
        writeInt(L"Window", L"Width",
                 wp.rcNormalPosition.right - wp.rcNormalPosition.left);
        writeInt(L"Window", L"Height",
                 wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
        writeInt(L"Window", L"Maximized",
                 wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    }
}

void CEditorWindow::ApplyRendererOptions()
{
    if (!m_renderer)
        return;
    m_renderer->SetFonts(m_fontPrimary, m_fontSecondary);
    m_renderer->SetFontSize(m_fontSize * m_dpiScale);   // 96 基准字号 → 当前 DPI
    m_renderer->SetLineHeightFactor(m_lineHeightFactor);
    m_renderer->SetWordWrap(m_wordWrap);
    DWORD total = m_lineIndex.IsValid()
        ? static_cast<DWORD>(m_lineIndex.GetLineCount()) : 1;
    m_renderer->SetLineNumbers(m_showLineNumbers, total);
    if (m_wordWrap)
    {
        m_hScrollPos = 0.0f;
        m_maxLineWidth = 0.0f;
    }
    BumpVisualEpoch();   // 字体/换行变了，视觉行数缓存全部失效
    UpdateScrollBar();
    InvalidateEditor();
}

bool CEditorWindow::IsDarkTheme() const
{
    if (m_themeMode == ThemeDark)
        return true;
    if (m_themeMode == ThemeLight)
        return false;

    // 跟随系统：读 AppsUseLightTheme（缺省视为浅色）
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD value = 1, size = sizeof(value), type = 0;
        LRESULT r = RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(hKey);
        if (r == ERROR_SUCCESS && type == REG_DWORD)
            return value == 0;
    }
    return false;
}

void CEditorWindow::SetDarkTitleBar(bool dark)
{
    if (!m_hwnd)
        return;
    BOOL v = dark ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE：Win10 20H1+ 为 20，旧版预览为 19
    if (FAILED(DwmSetWindowAttribute(m_hwnd, 20, &v, sizeof(v))))
        DwmSetWindowAttribute(m_hwnd, 19, &v, sizeof(v));
}

void CEditorWindow::ApplyThemeToWindow()
{
    bool dark = IsDarkTheme();
    if (m_renderer)
        m_renderer->SetTheme(dark);
    SetDarkTitleBar(dark);
    ApplyMenuTheme(dark);

    // 状态栏 owner-draw 配色
    if (m_statusBgBrush)
    {
        DeleteObject(m_statusBgBrush);
        m_statusBgBrush = nullptr;
    }
    m_statusBgBrush = CreateSolidBrush(dark ? RGB(0x25, 0x25, 0x26) : RGB(0xF3, 0xF3, 0xF3));
    m_statusFgColor = dark ? RGB(0xD4, 0xD4, 0xD4) : RGB(0, 0, 0);
    if (m_hStatusBar)
        InvalidateRect(m_hStatusBar, nullptr, TRUE);
    InvalidateEditor();
}

// uxtheme 未公开序号：SetPreferredAppMode(135) + FlushMenuThemes(136)，
// 让菜单栏/弹出菜单按应用主题渲染（Win10 1809+；不支持的系统上静默失败）
void CEditorWindow::ApplyMenuTheme(bool dark)
{
    static HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
    if (uxtheme)
    {
        static auto setPref = reinterpret_cast<int(WINAPI*)(int)>(
            GetProcAddress(uxtheme, LPCSTR(135)));
        static auto flush = reinterpret_cast<void(WINAPI*)()>(
            GetProcAddress(uxtheme, LPCSTR(136)));
        if (setPref)
            setPref(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
        if (flush)
            flush();
    }
    // 滚动条深色主题（失败无害）
    SetWindowTheme(m_hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    if (m_hStatusBar)
        SetWindowTheme(m_hStatusBar, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

// ---- DPI 缩放 ----

// 读取窗口所在显示器 DPI，换算缩放系数（Per-Monitor V2 下随移动/缩放实时变化）
void CEditorWindow::ApplyDpiScale()
{
    UINT dpi = 96;
    if (m_hwnd)
    {
        using Fn = UINT(WINAPI*)(HWND);
        static Fn getDpi = []() -> Fn {
            HMODULE u32 = GetModuleHandleW(L"user32.dll");
            return u32 ? reinterpret_cast<Fn>(GetProcAddress(u32, "GetDpiForWindow")) : nullptr;
        }();
        if (getDpi)
            dpi = getDpi(m_hwnd);
    }
    if (dpi == 0)
        dpi = 96;
    m_dpiScale = static_cast<float>(dpi) / 96.0f;
}

int CEditorWindow::Scale(int v) const
{
    return static_cast<int>(v * m_dpiScale + 0.5f);
}

// 状态栏字体随 DPI 缩放（默认字体只按系统 DPI，不跟窗口所在显示器）
void CEditorWindow::UpdateStatusFont()
{
    int h = Scale(-11);
    if (m_statusFont)
        DeleteObject(m_statusFont);
    m_statusFont = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (m_hStatusBar && m_statusFont)
        SendMessageW(m_hStatusBar, WM_SETFONT, reinterpret_cast<WPARAM>(m_statusFont), TRUE);
}

void CEditorWindow::SyncMenuChecks()
{
    if (!m_hMenu)
        return;

    CheckMenuItem(m_hMenu, kStatusBarId, m_hStatusBar ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m_hMenu, kLineNumbersId, m_showLineNumbers ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m_hMenu, kWordWrapId, m_wordWrap ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m_hMenu, kHideMenuBarId, m_hideMenuBar ? MF_CHECKED : MF_UNCHECKED);

    int padIdx = 2;
    if (m_pad.left == 0) padIdx = 0;
    else if (m_pad.left <= 4) padIdx = 1;
    else if (m_pad.left >= 16) padIdx = 3;
    CheckMenuRadioItem(m_hMenu, kPaddingNoneId, kPaddingLargeId, padIdx, MF_BYCOMMAND);

    int lhIdx = 1;
    if (m_lineHeightFactor < 1.1f) lhIdx = 0;
    else if (m_lineHeightFactor < 1.35f) lhIdx = 1;
    else if (m_lineHeightFactor < 1.75f) lhIdx = 2;
    else lhIdx = 3;
    CheckMenuRadioItem(m_hMenu, kLineHeight10Id, kLineHeight20Id, lhIdx, MF_BYCOMMAND);

    CheckMenuRadioItem(m_hMenu, kThemeSystemId, kThemeDarkId, m_themeMode, MF_BYCOMMAND);
    CheckMenuItem(m_hMenu, kDefaultEditorId,
                  IsRegisteredAsTextEditor() ? MF_CHECKED : MF_UNCHECKED);
}

void CEditorWindow::PickFont(bool primary)
{
    LOGFONTW lf{};
    lf.lfHeight = -20;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, LF_FACESIZE,
              (primary ? m_fontPrimary : m_fontSecondary).c_str(), _TRUNCATE);

    CHOOSEFONTW cf{};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = m_hwnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_INITTOLOGFONTSTRUCT | CF_SCREENFONTS;
    if (ChooseFontW(&cf) && lf.lfFaceName[0])
    {
        if (primary)
            m_fontPrimary = lf.lfFaceName;
        else
            m_fontSecondary = lf.lfFaceName;
        UpdateFontMenuLabels();
        ApplyRendererOptions();
        SaveSettings();
    }
}

void CEditorWindow::UpdateFontMenuLabels()
{
    if (!m_hMenu)
        return;
    std::wstring p = L"主字体(&P)：" + m_fontPrimary;
    std::wstring s = L"次要字体(&S)：" + m_fontSecondary;
    ModifyMenuW(m_hMenu, kFontPrimaryId, MF_BYCOMMAND | MF_STRING, kFontPrimaryId, p.c_str());
    ModifyMenuW(m_hMenu, kFontSecondaryId, MF_BYCOMMAND | MF_STRING, kFontSecondaryId, s.c_str());
    wchar_t z[48];
    wsprintfW(z, L"重置字号(&R)\tCtrl+0（当前 %dpt）", static_cast<int>(m_fontSize + 0.5f));
    ModifyMenuW(m_hMenu, kFontSizeResetId, MF_BYCOMMAND | MF_STRING, kFontSizeResetId, z);
}

// ---------------- 菜单栏显隐 / 字号 ----------------

void CEditorWindow::ApplyMenuBarState()
{
    if (!m_hwnd || !m_hMenu)
        return;
    if (m_hideMenuBar && !m_menuBarTempShown)
        SetMenu(m_hwnd, nullptr);
    else
        SetMenu(m_hwnd, m_hMenu);
}

void CEditorWindow::EndTempMenuBar()
{
    if (!m_menuBarTempShown)
        return;
    m_menuBarTempShown = false;
    if (m_hideMenuBar)
        SetMenu(m_hwnd, nullptr);
}

void CEditorWindow::ChangeFontSize(float delta)
{
    float size = (delta == 0.0f) ? 14.0f : m_fontSize + delta;
    if (size < 8.0f)
        size = 8.0f;
    if (size > 72.0f)
        size = 72.0f;
    if (size == m_fontSize)
        return;
    m_fontSize = size;
    UpdateFontMenuLabels();
    ApplyRendererOptions();
    EnsureCaretVisible(false);   // 行高变化后保持光标可见
    SaveSettings();
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

    // 行号栏宽随总行数变化；水平滚动状态复位
    if (m_renderer)
    {
        DWORD total = m_lineIndex.IsValid()
            ? static_cast<DWORD>(m_lineIndex.GetLineCount()) : 1;
        m_renderer->SetLineNumbers(m_showLineNumbers, total);
    }
    m_maxLineWidth = 0.0f;
    m_hScrollPos = 0.0f;
    // 语言按扩展名重检测；词法状态缓存全量失效
    m_lang = Highlighter::DetectByExtension(m_filePath);
    ClearHighlightCache();
    BumpVisualEpoch();
}

// ---------------- 多实例去重 ----------------

// 查询该文件是否已被某个实例窗口打开：命中则激活那个窗口并返回 true
bool CEditorWindow::ActivateExistingForFile(LPCWSTR szPath)
{
    if (!szPath || !szPath[0])
        return false;

    std::wstring path = szPath;
    FileActivateEnumCtx ctx{ &path, nullptr };
    EnumWindows(FindWindowWithFileWndProc, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.found)
        return false;

    // 恢复并置前。刚由用户操作（如资源管理器双击）启动的进程拥有前台激活
    // 权限，由它置前最可靠；目标进程收到查询时也会自行尝试作兜底
    if (IsIconic(ctx.found))
        ShowWindow(ctx.found, SW_RESTORE);
    SetForegroundWindow(ctx.found);
    return true;
}

// WM_COPYDATA 查询处理：文件已在本窗口打开则置前自己并返回 true
bool CEditorWindow::OnFileActivateCopyData(LPARAM lParam)
{
    auto* pcs = reinterpret_cast<COPYDATASTRUCT*>(lParam);
    if (!pcs || pcs->dwData != kFileActivateTag || !pcs->lpData ||
        pcs->cbData < sizeof(wchar_t) || pcs->cbData % sizeof(wchar_t) != 0)
        return false;
    if (m_filePath.empty())
        return false;

    size_t chars = pcs->cbData / sizeof(wchar_t);
    std::wstring incoming(static_cast<const wchar_t*>(pcs->lpData), chars - 1);

    if (NormalizePathForCompare(incoming) != NormalizePathForCompare(m_filePath))
        return false;

    if (IsIconic(m_hwnd))
        ShowWindow(m_hwnd, SW_RESTORE);
    SetForegroundWindow(m_hwnd);
    return true;
}

BOOL CEditorWindow::OpenFile(LPCWSTR szPath)
{
    // 未保存则先确认（与 WM_CLOSE 同一套提示）：保存 / 不保存 / 取消
    if (m_dirty)
    {
        const wchar_t* name = L"无标题";
        std::wstring fileName;
        if (!m_filePath.empty())
        {
            size_t p = m_filePath.find_last_of(L"\\/");
            fileName = (p == std::wstring::npos) ? m_filePath : m_filePath.substr(p + 1);
            name = fileName.c_str();
        }
        wchar_t msg[1024];
        wsprintfW(msg, L"是否将更改保存到\r\n%s？", name);
        int r = MessageBoxW(m_hwnd, msg, L"文本编辑器", MB_YESNOCANCEL | MB_ICONWARNING);
        if (r == IDCANCEL)
            return FALSE;
        if (r == IDYES && !SaveDocument())
            return FALSE;   // 保存失败或在另存为对话框取消 → 放弃本次打开
    }

    // 新建空白文档
    if (!szPath || !szPath[0])
    {
        m_buffer.reset();
        m_filePath.clear();
        RebuildDocument();
        m_savedUndoDepth = m_piece.HistoryPosition();
        m_dirty = false;
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

    // 该文件已在某个窗口（含本窗口）打开 → 聚焦那个窗口，不重复打开
    if (ActivateExistingForFile(szPath))
        return TRUE;

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
    RebuildDocument();
    m_savedUndoDepth = m_piece.HistoryPosition();
    m_dirty = false;

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

    const bool targetIsOpen = m_buffer && m_filePath == szPath;

    // 目标正是当前已映射打开的文件：Windows 禁止对有活动映射的文件截断/替换
    // （ERROR_THE_MAPPING / 共享冲突），所以先解除映射，直写后再重新映射。
    // 文件本身以 FILE_SHARE_WRITE 打开，直写无需替换。
    if (targetIsOpen)
    {
        m_buffer->CloseFile();

        HANDLE hFile = CreateFileW(szPath, GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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
            MessageBoxW(m_hwnd, L"写入文件失败", L"错误", MB_OK | MB_ICONERROR);
            return FALSE;
        }

        // 重新映射并重建（PieceTable 基址随映射更换，SetOriginal 同步清空
        // pieces/撤销栈；文件内容刚被保存，重建后的状态与当前一致）
        if (!m_buffer->OpenFile(szPath))
        {
            MessageBoxW(m_hwnd, L"保存成功，但重新读取文件失败", L"警告", MB_OK | MB_ICONWARNING);
        }
        RebuildDocument();

        m_savedUndoDepth = m_piece.HistoryPosition();
        m_dirty = false;
        m_filePath = szPath;
        UpdateTitle();
        UpdateStatusBar();
        return TRUE;
    }

    // 目标不是当前打开的文件：写临时文件后原子替换
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
        MessageBoxW(m_hwnd, L"替换文件失败（文件可能被其他程序独占）", L"错误", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    m_savedUndoDepth = m_piece.HistoryPosition();
    m_dirty = false;
    m_filePath = szPath;
    UpdateTitle();
    UpdateStatusBar();
    return TRUE;
}

// ---------------- 标题 / 状态栏 ----------------

void CEditorWindow::SyncDirtyFlag()
{
    // 内容是否偏离"保存时的历史位置"：编辑前进、撤销后退，
    // 回到保存点（编辑后又撤销/重做）即视为已保存状态
    bool dirty = m_piece.HistoryPosition() != m_savedUndoDepth;
    if (dirty != m_dirty)
    {
        m_dirty = dirty;
        UpdateTitle();
        UpdateStatusBar();
    }
}

bool CEditorWindow::SaveDocument()
{
    if (m_filePath.empty())
    {
        // 无路径 → 走另存为（用户取消则视为保存失败，调用方据此中止关闭）
        wchar_t path[MAX_PATH * 4] = { 0 };
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = m_hwnd;
        SetupFileDialogFilter(ofn);
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH * 4;
        ofn.Flags = OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&ofn))
            return false;
        return SaveFile(path) != FALSE;
    }
    return SaveFile(m_filePath.c_str()) != FALSE;
}

void CEditorWindow::UpdateTitle()
{
    // 标题栏只显示文件名；完整路径在状态栏
    std::wstring name = L"无标题";
    if (!m_filePath.empty())
    {
        size_t p = m_filePath.find_last_of(L"\\/");
        name = (p == std::wstring::npos) ? m_filePath : m_filePath.substr(p + 1);
    }
    std::wstring title = (m_dirty ? L"* " : L"") + name;
    SetWindowTextW(m_hwnd, title.c_str());
}

void CEditorWindow::UpdateStatusBarParts()
{
    if (!m_hStatusBar || !m_hwnd)
        return;
    RECT rc = EditorRect();
    int w = rc.right - rc.left;
    if (w < 100)
        w = 100;
    int parts[4] = { w * 45 / 100, w * 70 / 100, w * 85 / 100, -1 };
    SendMessageW(m_hStatusBar, SB_SETPARTS, 4, reinterpret_cast<LPARAM>(parts));
}

void CEditorWindow::UpdateStatusBar()
{
    if (!m_hStatusBar)
        return;

    // 第 0 格固定显示文件完整路径（需求 8）
    m_statusText[0] = m_filePath.empty() ? std::wstring(L"（未命名）") : m_filePath;

    if (!m_lineIndex.IsValid())
    {
        m_statusText[1] = L"就绪";
        m_statusText[2].clear();
        m_statusText[3].clear();
    }
    else
    {
        std::wstringstream pos;
        pos << L"行 " << (m_caretRow + 1) << L", 列 " << (m_caretCol + 1)
            << L" | 总行 " << m_lineIndex.GetLineCount()
            << (m_dirty ? L" | 已修改" : L"");
        m_statusText[1] = pos.str();
        m_statusText[2] = EncodingName(m_encoding);
        m_statusText[3] = FormatSize(static_cast<LONGLONG>(m_piece.Size()));
    }

    // 各分区 owner-draw（WM_DRAWITEM 按主题着色）
    for (int i = 0; i < 4; ++i)
    {
        SendMessageW(m_hStatusBar, SB_SETTEXTW, i | SBT_OWNERDRAW,
                     reinterpret_cast<LPARAM>(m_statusText[i].c_str()));
    }
}

void CEditorWindow::OnDrawStatusBarPart(DRAWITEMSTRUCT* dis)
{
    if (!dis || dis->itemID >= 4)
        return;

    HDC hdc = dis->hDC;
    FillRect(hdc, &dis->rcItem, m_statusBgBrush ? m_statusBgBrush
                                                : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, m_statusFgColor);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(m_hStatusBar, WM_GETFONT, 0, 0));
    HGDIOBJ old = SelectObject(hdc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    RECT rc = dis->rcItem;
    rc.left += 6;
    DrawTextW(hdc, m_statusText[dis->itemID].c_str(), -1, &rc,
              DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(hdc, old);
}

void CEditorWindow::OnResize()
{
    if (m_hStatusBar)
        SendMessageW(m_hStatusBar, WM_SIZE, 0, 0);
    UpdateStatusBarParts();
    if (m_renderer)
        m_renderer->Resize();
    BumpVisualEpoch();   // 视口宽度变化 → 换行行数变化
    UpdateScrollBar();
    InvalidateEditor();
}

void CEditorWindow::OnCreate(HWND hwnd)
{
    m_hwnd = hwnd;

    LoadSettings();
    ApplyDpiScale();   // DPI 缩放需在字体/渲染选项应用前生效

    if (m_showStatusBar)
    {
        m_hStatusBar = CreateStatusWindowW(WS_CHILD | WS_VISIBLE, L"就绪", hwnd, kStatusBarId);
        UpdateStatusBarParts();
        UpdateStatusFont();
    }

    if (m_renderer)
        m_renderer->Init(hwnd);

    RebuildDocument();
    ApplyThemeToWindow();
    SyncMenuChecks();
    UpdateFontMenuLabels();
    ApplyRendererOptions();   // 应用持久化的换行/行高/字体/行号等
    ApplyMenuBarState();      // 应用持久化的"隐藏菜单栏"
    UpdateStatusBar();
    UpdateScrollBar();
}

void CEditorWindow::Destroy()
{
    if (m_statusFont)
    {
        DeleteObject(m_statusFont);
        m_statusFont = nullptr;
    }
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

// ---------------- 布局几何 ----------------

RECT CEditorWindow::EditorRect() const
{
    RECT rc{};
    if (m_hwnd)
        GetClientRect(m_hwnd, &rc);
    if (m_hStatusBar && IsWindowVisible(m_hStatusBar))
    {
        // 状态栏顶边映射到客户区坐标。
        // 注意：不要用 GetWindowRect+ScreenToClient（高 DPI 下两套坐标空间混用会把
        // 客户区高度放大近一倍，导致滚动/光标可见性计算全部失真）
        POINT pt{ 0, 0 };
        MapWindowPoints(m_hStatusBar, m_hwnd, &pt, 1);
        rc.bottom = pt.y;
    }
    return rc;
}

float CEditorWindow::ViewportHeight() const
{
    RECT rc = EditorRect();
    float h = static_cast<float>(rc.bottom - rc.top);
    return h > 0.0f ? h : 0.0f;
}

float CEditorWindow::TextOriginX() const
{
    return static_cast<float>(Scale(m_pad.left)) + (m_renderer ? m_renderer->GetGutterWidth() : 0.0f);
}

float CEditorWindow::TextAreaWidth() const
{
    RECT rc = EditorRect();
    float w = static_cast<float>(rc.right - rc.left) - TextOriginX()
            - static_cast<float>(Scale(m_pad.right));
    // 自绘垂直滚动条覆盖编辑区右缘，排版宽度需让开它
    if (VScrollVisible())
        w -= SbarWidth();
    return w > 50.0f ? w : 50.0f;
}

// ---- 自定义滚动条 ----

float CEditorWindow::SbarWidth() const
{
    return 12.0f * m_dpiScale;
}

// 需求：内容总高度不足视口 1.5 倍时不显示垂直滚动条
bool CEditorWindow::VScrollVisible() const
{
    if (!m_lineIndex.IsValid())
        return false;
    float lineH = m_renderer ? m_renderer->GetLineHeight() : 20.0f;
    if (lineH <= 0.0f)
        return false;
    float contentH = static_cast<float>(m_lineIndex.GetLineCount()) * lineH;
    return contentH >= ViewportHeight() * 1.5f;
}

bool CEditorWindow::InRect(const D2D1_RECT_F& r, const POINT& pt) const
{
    return pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom;
}

void CEditorWindow::SetHScrollPos(float px)
{
    if (px < 0.0f)
        px = 0.0f;
    if (px > m_hMax)
        px = m_hMax;
    if (px == m_hScrollPos)
        return;
    m_hScrollPos = px;
    UpdateScrollBar();
    InvalidateEditor();
}

int CEditorWindow::MaxScrollLine() const
{
    if (!m_lineIndex.IsValid())
        return 0;
    DWORD total = static_cast<DWORD>(m_lineIndex.GetLineCount());
    float lineH = m_renderer ? m_renderer->GetLineHeight() : 20.0f;
    if (lineH <= 0.0f)
        lineH = 20.0f;
    int vpLines = static_cast<int>(ViewportHeight() / lineH);
    if (vpLines < 1)
        vpLines = 1;
    int blank = vpLines * 2 / 3;   // 文末预留约 2/3 视口高度的留白（需求 11）
    // 总高 = 内容 + 留白；可滚动范围 = 总高 - 视口。
    // 内容不足视口 1/3 时总高 ≤ 视口，整体不可滚动
    long long maxScroll = static_cast<long long>(total) + blank - vpLines;
    return maxScroll > 0 ? static_cast<int>(maxScroll) : 0;
}

void CEditorWindow::InvalidateEditor()
{
    if (!m_hwnd)
        return;
    RECT rc = EditorRect();
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

// ---- 可见行缓存 ----

void CEditorWindow::BumpVisualEpoch()
{
    ++m_visualEpoch;
}

const CEditorWindow::VisualRow& CEditorWindow::VisualRowOf(DWORD row) const
{
    if (m_visualEpochSeen != m_visualEpoch)
    {
        m_visualCache.clear();
        m_visualEpochSeen = m_visualEpoch;
    }
    auto it = m_visualCache.find(row);
    if (it != m_visualCache.end())
        return it->second;

    VisualRow vr;
    vr.text = GetLineText(row);
    vr.visualLines = (m_renderer && m_wordWrap)
        ? m_renderer->GetRowVisualCount(vr.text, TextAreaWidth()) : 1;
    return m_visualCache.emplace(row, std::move(vr)).first->second;
}

UINT CEditorWindow::RowVisualCount(DWORD row) const
{
    return VisualRowOf(row).visualLines;
}

void CEditorWindow::BuildVisibleRows(std::vector<CRenderer::Row>& rows) const
{
    rows.clear();
    if (!m_lineIndex.IsValid() || !m_renderer)
        return;

    DWORD totalLines = static_cast<DWORD>(m_lineIndex.GetLineCount());
    if (totalLines == 0)
        return;

    float lineH = m_renderer->GetLineHeight();
    float viewportH = ViewportHeight();

    // 逐逻辑行累计换行后的视觉行高（修复自动换行内容重叠的关键）；
    // 行文本/视觉行数走缓存（编辑/设置/尺寸变化时整代失效）
    rows.reserve(64);
    float y = static_cast<float>(Scale(m_pad.top));
    for (DWORD row = m_scrollLine; row < totalLines && y < viewportH; ++row)
    {
        const VisualRow& vr = VisualRowOf(row);
        CRenderer::Row r;
        r.row = row;
        r.text = vr.text;
        r.yTop = y;
        r.visualLines = vr.visualLines;
        if (m_lang != Lang::None)
        {
            uint32_t st = StateBeforeLine(row);
            Highlighter::LexLine(m_lang, r.text, st, r.tokens, st);
        }
        y += static_cast<float>(r.visualLines) * lineH;
        rows.push_back(std::move(r));
    }
}

// ---- 语法高亮状态缓存 ----

uint32_t CEditorWindow::StateAfterLine(DWORD row) const
{
    while (m_hlStatesValid <= row)
    {
        size_t i = m_hlStatesValid;
        uint32_t st = (i > 0) ? m_hlStates[i - 1] : 0;
        std::vector<Token> scratch;   // 只取行末状态，token 丢弃
        Highlighter::LexLine(m_lang, GetLineText(static_cast<DWORD>(i)), st,
                             scratch, st);
        m_hlStates.push_back(st);
        ++m_hlStatesValid;
    }
    return m_hlStates[row];
}

uint32_t CEditorWindow::StateBeforeLine(DWORD row) const
{
    return (row == 0) ? 0 : StateAfterLine(row - 1);
}

void CEditorWindow::InvalidateHighlightFrom(DWORD line)
{
    if (m_hlStatesValid > line)
        m_hlStatesValid = line;   // 前缀状态仍有效，只收缩有效区
}

void CEditorWindow::ClearHighlightCache()
{
    m_hlStates.clear();
    m_hlStatesValid = 0;
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

    RECT rc = EditorRect();

    std::vector<CRenderer::Row> rows;
    BuildVisibleRows(rows);

    int lineHeight = static_cast<int>(m_renderer->GetLineHeight());
    float frameMax = 0.0f;
    m_renderer->Render(rows, lineHeight, TextAreaWidth(),
                       static_cast<float>(rc.bottom - rc.top),
                       TextOriginX() - m_hScrollPos,
                       m_caretRow, m_caretCol, m_caretVisible && m_hasFocus,
                       GetRenderSelection(), &frameMax, &m_vBar, &m_hBar);

    // 关闭自动换行时，用见过的最宽行撑开水平滚动范围
    if (!m_wordWrap && frameMax > m_maxLineWidth)
    {
        m_maxLineWidth = frameMax;
        UpdateScrollBar();
    }

    EndPaint(m_hwnd, &ps);
}

// 计算自绘滚动条的轨道/滑块几何（绘制在 Render 内完成，这里只算矩形）
void CEditorWindow::UpdateScrollBar()
{
    if (!m_hwnd)
        return;

    RECT rc = EditorRect();
    float vpH = static_cast<float>(rc.bottom - rc.top);
    float lineH = m_renderer ? m_renderer->GetLineHeight() : 20.0f;
    if (lineH <= 0.0f)
        lineH = 20.0f;
    int vpLines = static_cast<int>(vpH / lineH);
    if (vpLines < 1)
        vpLines = 1;

    float barW = SbarWidth();
    float inset = Scale(2);
    float minThumb = Scale(24);
    float areaW = static_cast<float>(rc.right - rc.left);

    // 水平滚动条：仅在关闭自动换行且内容超宽时显示
    bool hVisible = false;
    float maxPx = 0.0f;
    if (m_lineIndex.IsValid() && !m_wordWrap)
    {
        maxPx = m_maxLineWidth + TextOriginX() + static_cast<float>(Scale(m_pad.right)) - areaW;
        if (maxPx < 0)
            maxPx = 0;
        hVisible = maxPx > 0;
    }
    bool vVisible = VScrollVisible();

    float vTrackH = vpH - (hVisible ? barW : 0.0f);
    float hTrackW = areaW - (vVisible ? barW : 0.0f);

    m_vBar.visible = vVisible;
    m_hBar.visible = hVisible;

    // 垂直：滑块长度 ∝ 视口行数 / (可滚动行数 + 视口行数)
    if (vVisible)
    {
        int maxScroll = MaxScrollLine();
        m_vMax = maxScroll;
        m_vPage = vpLines;
        float trackH = vTrackH - 2 * inset;
        if (trackH < minThumb)
            trackH = minThumb;
        float content = static_cast<float>(maxScroll + vpLines);
        float thumbH = trackH * static_cast<float>(vpLines) / content;
        if (thumbH < minThumb)
            thumbH = minThumb;
        if (thumbH > trackH)
            thumbH = trackH;
        float frac = maxScroll > 0
            ? static_cast<float>(m_scrollLine) / static_cast<float>(maxScroll) : 0.0f;
        if (frac > 1.0f)
            frac = 1.0f;
        float tTop = rc.top + inset + (trackH - thumbH) * frac;
        m_vBar.track = D2D1::RectF(areaW - barW, static_cast<float>(rc.top),
                                   areaW, static_cast<float>(rc.top) + vTrackH);
        m_vBar.thumb = D2D1::RectF(areaW - barW + inset, tTop,
                                   areaW - inset, tTop + thumbH);
    }

    // 水平：滑块长度 ∝ 视口宽 / (内容总宽)
    if (hVisible)
    {
        m_hMax = maxPx;
        m_hPage = hTrackW;
        float trackW = hTrackW - 2 * inset;
        if (trackW < minThumb)
            trackW = minThumb;
        float content = maxPx + hTrackW;
        float thumbW = trackW * hTrackW / content;
        if (thumbW < minThumb)
            thumbW = minThumb;
        if (thumbW > trackW)
            thumbW = trackW;
        float frac = maxPx > 0 ? m_hScrollPos / maxPx : 0.0f;
        if (frac > 1.0f)
            frac = 1.0f;
        float tLeft = rc.left + inset + (trackW - thumbW) * frac;
        m_hBar.track = D2D1::RectF(static_cast<float>(rc.left),
                                   static_cast<float>(rc.bottom) - barW,
                                   static_cast<float>(rc.left) + hTrackW,
                                   static_cast<float>(rc.bottom));
        m_hBar.thumb = D2D1::RectF(tLeft,
                                   static_cast<float>(rc.bottom) - barW + inset,
                                   tLeft + thumbW,
                                   static_cast<float>(rc.bottom) - inset);
    }
}

// 滚动条按下：滑块→开始拖拽；轨道空白→向该方向翻页并接续拖拽。
// 返回是否命中滚动条（命中则消息不再下发给文本区）
bool CEditorWindow::OnScrollbarDown(int x, int y)
{
    POINT pt{ x, y };

    if (m_vBar.visible && InRect(m_vBar.track, pt))
    {
        SetCapture(m_hwnd);
        m_barDrag = 1;
        if (pt.y < m_vBar.thumb.top)
        {
            long t = static_cast<long>(m_scrollLine) - std::max(1, m_vPage);
            ScrollToLine(t < 0 ? 0 : static_cast<DWORD>(t));
            m_barDragOfs = (m_vBar.thumb.bottom - m_vBar.thumb.top) * 0.5f;
        }
        else if (pt.y > m_vBar.thumb.bottom)
        {
            ScrollToLine(m_scrollLine + static_cast<DWORD>(std::max(1, m_vPage)));
            m_barDragOfs = (m_vBar.thumb.bottom - m_vBar.thumb.top) * 0.5f;
        }
        else
        {
            m_barDragOfs = static_cast<float>(pt.y) - m_vBar.thumb.top;
        }
        m_vBar.dragged = true;
        InvalidateEditor();
        return true;
    }

    if (m_hBar.visible && InRect(m_hBar.track, pt))
    {
        SetCapture(m_hwnd);
        m_barDrag = 2;
        if (pt.x < m_hBar.thumb.left)
        {
            SetHScrollPos(m_hScrollPos - m_hPage);
            m_barDragOfs = (m_hBar.thumb.right - m_hBar.thumb.left) * 0.5f;
        }
        else if (pt.x > m_hBar.thumb.right)
        {
            SetHScrollPos(m_hScrollPos + m_hPage);
            m_barDragOfs = (m_hBar.thumb.right - m_hBar.thumb.left) * 0.5f;
        }
        else
        {
            m_barDragOfs = static_cast<float>(pt.x) - m_hBar.thumb.left;
        }
        m_hBar.dragged = true;
        InvalidateEditor();
        return true;
    }

    return false;
}

void CEditorWindow::OnScrollbarMove(int x, int y)
{
    POINT pt{ x, y };

    if (m_barDrag == 1)
    {
        float inset = Scale(2);
        float trackH = (m_vBar.track.bottom - m_vBar.track.top) - 2 * inset;
        float thumbH = m_vBar.thumb.bottom - m_vBar.thumb.top;
        float slide = trackH - thumbH;
        float frac = slide > 0
            ? (static_cast<float>(pt.y) - m_barDragOfs - (m_vBar.track.top + inset)) / slide : 0.0f;
        if (frac < 0.0f)
            frac = 0.0f;
        if (frac > 1.0f)
            frac = 1.0f;
        ScrollToLine(static_cast<DWORD>(frac * m_vMax + 0.5f));
        return;
    }
    if (m_barDrag == 2)
    {
        float inset = Scale(2);
        float trackW = (m_hBar.track.right - m_hBar.track.left) - 2 * inset;
        float thumbW = m_hBar.thumb.right - m_hBar.thumb.left;
        float slide = trackW - thumbW;
        float frac = slide > 0
            ? (static_cast<float>(pt.x) - m_barDragOfs - (m_hBar.track.left + inset)) / slide : 0.0f;
        if (frac < 0.0f)
            frac = 0.0f;
        if (frac > 1.0f)
            frac = 1.0f;
        SetHScrollPos(frac * m_hMax);
        return;
    }

    // 悬停着色
    bool vH = m_vBar.visible && InRect(m_vBar.track, pt);
    bool hH = m_hBar.visible && InRect(m_hBar.track, pt);
    if ((vH || hH) && !m_mouseTracking)
    {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, m_hwnd, 0 };
        TrackMouseEvent(&tme);
        m_mouseTracking = true;
    }
    if (vH != m_vBar.hovered || hH != m_hBar.hovered)
    {
        m_vBar.hovered = vH;
        m_hBar.hovered = hH;
        InvalidateEditor();
    }
}

void CEditorWindow::OnScrollbarUp()
{
    if (!m_barDrag)
        return;
    m_barDrag = 0;
    m_vBar.dragged = false;
    m_hBar.dragged = false;
    InvalidateEditor();
}

void CEditorWindow::ScrollToLine(DWORD line)
{
    if (!m_lineIndex.IsValid())
        return;
    int maxScroll = MaxScrollLine();
    if (static_cast<int>(line) > maxScroll)
        line = static_cast<DWORD>(maxScroll);

    if (line != m_scrollLine)
    {
        m_scrollLine = line;
        UpdateScrollBar();
        InvalidateEditor();
    }
}

// ---------------- 光标可见性 / 滚动 ----------------

// 从当前滚动行向下累计视觉行高，计算光标行顶部 y
// 返回 false = 距离过远或超大行导致开销过大（调用方走快速跳转路径）
bool CEditorWindow::VisualTopOfCaretRow(float lineH, float* yTop)
{
    if (!m_renderer || !m_lineIndex.IsValid())
        return false;
    if (m_caretRow < m_scrollLine)
        return false;
    if (m_caretRow - m_scrollLine > kVisualScanMaxRows)
        return false;   // 太远：精确累计开销过大，走快速路径

    float y = 0.0f;
    int budget = kVisualScanCharBudget;
    for (DWORD r = m_scrollLine; r < m_caretRow; ++r)
    {
        budget -= static_cast<int>(VisualRowOf(r).text.size());
        if (budget < 0)
            return false;
        y += static_cast<float>(RowVisualCount(r)) * lineH;
    }
    *yTop = y;
    return true;
}

// typingMode=true：输入（插入/删除/撤销）触发——光标块底部一旦越过 2/3 视口高度
// 就立即最小滚动回舒适区（而非等完全滚出屏幕才大跳）
// typingMode=false：普通移动/点击，最小滚动保证光标可见
void CEditorWindow::EnsureCaretVisible(bool typingMode)
{
    if (!m_lineIndex.IsValid())
        return;

    float lineH = m_renderer ? m_renderer->GetLineHeight() : 20.0f;
    float viewportH = ViewportHeight();
    if (viewportH <= 0.0f || lineH <= 0.0f)
        return;

    DWORD total = static_cast<DWORD>(m_lineIndex.GetLineCount());
    if (total == 0)
        return;
    if (m_caretRow >= total)
    {
        m_caretRow = total - 1;
        DWORD len = static_cast<DWORD>(GetLineText(m_caretRow).size());
        if (m_caretCol > len)
            m_caretCol = len;
        m_selCaretRow = m_caretRow;
        m_selCaretCol = m_caretCol;
    }

    if (typingMode)
    {
        // 输入（插入/删除/撤销）触发：内容变更后直接把光标行定位到
        // 视口约 2/3 高度（舒适区），不再“先越界再最小回滚”，
        // 光标始终停在舒适区
        ScrollCaretToComfortHeight();
        return;
    }

    float comfortBottom = viewportH * 2.0f / 3.0f;
    float limit = viewportH;

    int vpLines = static_cast<int>(viewportH / lineH);
    if (vpLines < 1)
        vpLines = 1;
    int comfortOffset = vpLines * 2 / 3;

    if (m_caretRow < m_scrollLine)
    {
        ScrollToLine(m_caretRow);
        return;
    }
    if (m_caretRow - m_scrollLine > kVisualScanMaxRows)
    {
        // 光标远离视口：直接跳转
        ScrollToLine(typingMode
            ? ComfortScrollLineFar(lineH, viewportH)
            : m_caretRow);
        return;
    }

    float yTop = 0.0f;
    if (!VisualTopOfCaretRow(lineH, &yTop))
    {
        ScrollToLine(typingMode
            ? ComfortScrollLineFar(lineH, viewportH)
            : m_caretRow);
        return;
    }

    UINT caretLines = RowVisualCount(m_caretRow);
    float caretH = static_cast<float>(caretLines) * lineH;

    // 光标块底部仍在舒适区/视口内 → 不滚动
    if (yTop + caretH <= limit)
        return;

    if (typingMode)
    {
        // 最小滚动：从光标行向上收行，直到光标块底部回到约 2/3 视口高度。
        // 用户连续输入换行时每次只滚一行，光标始终贴着舒适区下缘（而非大跳）
        float acc = 0.0f;
        DWORD k = 0;
        int budget = kVisualScanCharBudget;
        while (k < m_caretRow - m_scrollLine && k < kVisualScanMaxRows)
        {
            DWORD r = m_caretRow - 1 - k;
            budget -= static_cast<int>(VisualRowOf(r).text.size());
            if (budget < 0)
                break;
            float h = static_cast<float>(RowVisualCount(r)) * lineH;
            if (acc + h + caretH <= comfortBottom)
            {
                acc += h;
                ++k;
            }
            else
                break;
        }
        ScrollToLine(m_caretRow - k);
    }
    else
    {
        // 最小滚动：从光标行向上收行，直到光标块底部进入视口
        float acc = 0.0f;
        DWORD k = 0;
        int budget = kVisualScanCharBudget;
        while (k < m_caretRow - m_scrollLine && k < kVisualScanMaxRows)
        {
            DWORD r = m_caretRow - 1 - k;
            budget -= static_cast<int>(VisualRowOf(r).text.size());
            if (budget < 0)
                break;
            float h = static_cast<float>(RowVisualCount(r)) * lineH;
            if (acc + h + caretH <= viewportH)
            {
                acc += h;
                ++k;
            }
            else
                break;
        }
        ScrollToLine(m_caretRow - k);
    }
}

// 远距离快速路径：无法精确累计时按逻辑行粗略估算舒适区滚动目标
DWORD CEditorWindow::ComfortScrollLineFar(float lineH, float viewportH)
{
    int vpLines = static_cast<int>(viewportH / lineH);
    if (vpLines < 1)
        vpLines = 1;
    return static_cast<DWORD>(std::max(0, static_cast<int>(m_caretRow) - vpLines * 2 / 3));
}

// 计算把光标行顶部定位到视口 ratio 高度处的目标滚动行：
// 从光标行向上按视觉高度累加，直到累计高度达到目标。exact=false 表示距离过远
// （超出扫描上限/字符预算），调用方应退回按逻辑行的粗略估算
DWORD CEditorWindow::ComfortScrollLine(float lineH, float viewportH, float ratio, bool* exact)
{
    *exact = true;
    float target = viewportH * ratio;
    float acc = 0.0f;
    DWORD k = 0;
    int budget = kVisualScanCharBudget;
    while (k < m_caretRow && k < kVisualScanMaxRows)
    {
        DWORD r = m_caretRow - 1 - k;
        budget -= static_cast<int>(VisualRowOf(r).text.size());
        if (budget < 0)
        {
            *exact = false;
            break;
        }
        float h = static_cast<float>(RowVisualCount(r)) * lineH;
        if (acc + h <= target)
        {
            acc += h;
            ++k;
        }
        else
            break;
    }
    return m_caretRow - k;
}

// 需求 9/11：点击空白区跳到文末后，把光标行定位到视口约 2/3 高度处
void CEditorWindow::ScrollCaretToComfortHeight()
{
    if (!m_lineIndex.IsValid())
        return;
    float lineH = m_renderer ? m_renderer->GetLineHeight() : 20.0f;
    float viewportH = ViewportHeight();
    if (lineH <= 0.0f || viewportH <= 0.0f)
        return;

    bool exact = false;
    DWORD targetLine = ComfortScrollLine(lineH, viewportH, 2.0f / 3.0f, &exact);
    if (!exact)
    {
        int vpLines = static_cast<int>(viewportH / lineH);
        if (vpLines < 1)
            vpLines = 1;
        targetLine = static_cast<DWORD>(std::max(0, static_cast<int>(m_caretRow) - vpLines * 2 / 3));
    }
    ScrollToLine(targetLine);
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

    // 整行编码一次，再按编码在字节流上走 code point（逐字符调 WideCharToMultiByte
    // 在长行上是 O(n) 次 API 调用 + 分配）。字节步进与 API 输出一致：
    // UTF-8 按前导字节定长；UTF-16 每 code unit 2 字节；ANSI/GBK 高字节为双字节首字节
    std::string enc = EncodeFromWide(line, m_encoding);
    if (rel >= enc.size())
    {
        *pCol = static_cast<DWORD>(line.size());
        return;
    }

    DWORD col = 0;
    uint64_t acc = 0;
    size_t i = 0;
    while (col < line.size() && i < enc.size())
    {
        size_t step;
        if (m_encoding == Encoding::Utf16LE || m_encoding == Encoding::Utf16BE)
        {
            step = 2;
        }
        else
        {
            unsigned char c = static_cast<unsigned char>(enc[i]);
            if (m_encoding == Encoding::Utf8)
                step = (c & 0x80) == 0x00 ? 1
                     : (c & 0xE0) == 0xC0 ? 2
                     : (c & 0xF0) == 0xE0 ? 3
                     : 4;
            else
                step = (c >= 0x80) ? 2 : 1;   // GBK 双字节 / ASCII（含 '?' 替换）
        }
        if (i + step > enc.size())
            step = enc.size() - i;
        if (acc + step > rel)
            break;   // 目标偏移落在该 code point 中间 → 停在其之前
        acc += step;
        i += step;
        // 列号与行内文本的 UTF-16 code unit 对齐：UTF-8 下增补平面字符占 2 列
        col += (m_encoding == Encoding::Utf8 && step == 4) ? 2u : 1u;
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
    SyncDirtyFlag();

    // 行结构增量更新（含跨行插入）：O(关键帧数)，避免大文件全量重建造成卡顿
    int breaks = CountLineBreaks(text);
    int64_t lineDelta = breaks;
    m_lineIndex.NotifyEditRange(ofs, ofs,
                                static_cast<int64_t>(bytes.size()), lineDelta);
    InvalidateHighlightFrom(m_caretRow);   // 自插入行起词法状态失效（前缀不变）
    BumpVisualEpoch();

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
    EnsureCaretVisible(true);   // 输入触发的自动滚动（约 2/3 高度定位）
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
    SyncDirtyFlag();

    // 删除跨了多少行换行
    uint64_t rowCount = static_cast<uint64_t>(endRow) - startRow;

    // 行结构增量更新（含跨行删除）：O(关键帧数)，避免大文件全量重建造成卡顿
    m_lineIndex.NotifyEditRange(startByte, endByte,
                                -static_cast<int64_t>(len),
                                -static_cast<int64_t>(rowCount));
    InvalidateHighlightFrom(startRow);
    BumpVisualEpoch();

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
    EnsureCaretVisible(true);
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
        SyncDirtyFlag();
        // 重建行索引 + 光标置于改动前位置（用命令内偏移）
        uint64_t ofs = m_piece.UndoOffset();
        m_lineIndex.Build(
            [this](uint64_t o, unsigned char* d, uint64_t m) -> uint64_t {
                return DocRead(o, d, m);
            },
            m_piece.Size(), m_encoding);
        ClearHighlightCache();
        ByteToPos(ofs, &m_caretRow, &m_caretCol);
        m_selAnchorValid = false;
        m_selCaretRow = m_caretRow;
        m_selCaretCol = m_caretCol;
        BumpVisualEpoch();
        UpdateStatusBar();
        EnsureCaretVisible(true);
        InvalidateEditor();
    }
}

void CEditorWindow::Redo()
{
    if (m_piece.Redo())
    {
        SyncDirtyFlag();
        uint64_t ofs = m_piece.RedoOffset();
        m_lineIndex.Build(
            [this](uint64_t o, unsigned char* d, uint64_t m) -> uint64_t {
                return DocRead(o, d, m);
            },
            m_piece.Size(), m_encoding);
        ClearHighlightCache();
        ByteToPos(ofs, &m_caretRow, &m_caretCol);
        m_selAnchorValid = false;
        m_selCaretRow = m_caretRow;
        m_selCaretCol = m_caretCol;
        BumpVisualEpoch();
        UpdateStatusBar();
        EnsureCaretVisible(true);
        InvalidateEditor();
    }
}

// ---------------- 剪贴板 ----------------

bool CEditorWindow::SetClipboardText(const std::wstring& text)
{
    if (!OpenClipboard(m_hwnd))
        return false;
    EmptyClipboard();

    SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h)
    {
        CloseClipboard();
        return false;
    }

    wchar_t* p = static_cast<wchar_t*>(GlobalLock(h));
    if (!p)
    {
        GlobalFree(h);
        CloseClipboard();
        return false;
    }
    memcpy(p, text.c_str(), bytes);
    GlobalUnlock(h);

    // 成功后所有权移交系统；失败才需要自行释放
    if (!SetClipboardData(CF_UNICODETEXT, h))
        GlobalFree(h);
    CloseClipboard();
    return true;
}

void CEditorWindow::CopySelection()
{
    if (!HasSelection())
        return;
    NormalizeSelection();

    std::wstring text;
    if (m_selAnchorRow == m_selCaretRow)
    {
        std::wstring line = GetLineText(m_selAnchorRow);
        if (m_selAnchorCol < line.size())
        {
            DWORD end = std::min(m_selCaretCol, static_cast<DWORD>(line.size()));
            text = line.substr(m_selAnchorCol, end - m_selAnchorCol);
        }
    }
    else
    {
        // 跨行：首行从锚点到行尾，中间整行，末行到光标；行间以 \r\n 连接
        for (DWORD row = m_selAnchorRow; row <= m_selCaretRow; ++row)
        {
            std::wstring line = GetLineText(row);
            DWORD start = (row == m_selAnchorRow) ? m_selAnchorCol : 0;
            DWORD end   = (row == m_selCaretRow)
                        ? std::min(m_selCaretCol, static_cast<DWORD>(line.size()))
                        : static_cast<DWORD>(line.size());
            if (start < end)
                text += line.substr(start, end - start);
            if (row < m_selCaretRow)
                text += L"\r\n";
        }
    }

    if (!text.empty())
        SetClipboardText(text);
}

void CEditorWindow::PasteFromClipboard()
{
    if (!m_lineIndex.IsValid())
        return;
    if (!OpenClipboard(m_hwnd))
        return;

    std::wstring text;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h)
    {
        const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h));
        if (p)
        {
            // 按 GlobalSize 兜底扫描 NUL，不依赖缓冲一定以 NUL 结尾
            SIZE_T maxLen = GlobalSize(h) / sizeof(wchar_t);
            SIZE_T len = 0;
            while (len < maxLen && p[len] != L'\0')
                ++len;
            text.assign(p, static_cast<size_t>(len));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();

    if (!text.empty())
        InsertTextAtCaret(text);
}

// ---------------- 鼠标 / 键盘 / IME ----------------

// 客户区坐标 → (逻辑行, 列)。返回 false = 点击在最后一行之下的空白区
bool CEditorWindow::HitTestClient(int x, int y, DWORD* pRow, DWORD* pCol)
{
    if (!m_renderer || !m_lineIndex.IsValid())
        return false;

    std::vector<CRenderer::Row> rows;
    BuildVisibleRows(rows);
    if (rows.empty())
    {
        // 文档至少 1 行；可见行为空说明当前滚动在文末留白区 → 视为"最后一行之下"
        return false;
    }

    float textX = static_cast<float>(x) - TextOriginX() + m_hScrollPos;
    return m_renderer->HitTestPoint(rows, static_cast<int>(m_renderer->GetLineHeight()),
                                    TextAreaWidth(), textX, static_cast<float>(y),
                                    pRow, pCol);
}

POINT CEditorWindow::GetCaretClientPoint()
{
    POINT pt{ 0, 0 };
    if (!m_renderer || !m_lineIndex.IsValid())
        return pt;

    std::vector<CRenderer::Row> rows;
    BuildVisibleRows(rows);
    float lineH = m_renderer->GetLineHeight();

    for (const auto& r : rows)
    {
        if (r.row == m_caretRow)
        {
            float x = 0.0f, y = 0.0f;
            m_renderer->GetCaretPoint(r.text, m_caretCol, TextAreaWidth(), &x, &y);
            pt.x = static_cast<int>(TextOriginX() - m_hScrollPos + x);
            pt.y = static_cast<int>(r.yTop + y);
            return pt;
        }
    }

    // 光标行不在可见范围（罕见）：按逻辑行估算
    DWORD rel = m_caretRow > m_scrollLine ? m_caretRow - m_scrollLine : 0;
    pt.y = Scale(m_pad.top) + static_cast<int>(static_cast<float>(rel) * lineH);
    pt.x = static_cast<int>(TextOriginX());
    return pt;
}

bool CEditorWindow::IsImeOpen() const
{
    if (!m_hwnd)
        return false;
    HIMC himc = ImmGetContext(m_hwnd);
    if (!himc)
        return false;
    BOOL open = ImmGetOpenStatus(himc);
    ImmReleaseContext(m_hwnd, himc);
    return open != FALSE;
}

// 需求 2：把 IME 组合窗口锚定到当前光标位置
void CEditorWindow::UpdateImeCompositionWindow()
{
    if (!m_hwnd || !IsImeOpen())
        return;

    HIMC himc = ImmGetContext(m_hwnd);
    if (!himc)
        return;

    POINT pt = GetCaretClientPoint();
    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = pt;
    ImmSetCompositionWindow(himc, &cf);
    ImmReleaseContext(m_hwnd, himc);
}

void CEditorWindow::OnMouseClick(WPARAM wParam, LPARAM lParam, UINT clickCount)
{
    if (!m_lineIndex.IsValid())
        return;

    int x = GET_X_LPARAM(lParam);
    int y = GET_Y_LPARAM(lParam);

    if (m_hStatusBar && IsWindowVisible(m_hStatusBar))
    {
        POINT pt{ x, y };
        MapWindowPoints(m_hwnd, m_hStatusBar, &pt, 1);
        if (pt.y >= 0)
            return;   // 状态栏
    }

    DWORD row = 0, col = 0;
    bool inText = HitTestClient(x, y, &row, &col);
    bool shift = (wParam & MK_SHIFT) != 0;

    if (!inText)
    {
        // 需求 9：点击最后一行之下的空白区 → 光标跳到文末
        // 需求 11：跳转后把光标定位到视口约 2/3 高度处（而非底部）
        DWORD total = static_cast<DWORD>(m_lineIndex.GetLineCount());
        if (total == 0)
            return;
        row = total - 1;
        col = static_cast<DWORD>(GetLineText(row).size());

        m_caretRow = row;
        m_caretCol = col;
        m_selAnchorValid = true;
        m_selAnchorRow = row;
        m_selAnchorCol = col;
        m_selCaretRow = row;
        m_selCaretCol = col;

        UpdateStatusBar();
        // 点击不是编辑：保持当前滚动位置不动，避免光标跳到舒适区
        UpdateImeCompositionWindow();
        InvalidateEditor();
        return;
    }

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

    EnsureCaretVisible(false);
    UpdateStatusBar();
    UpdateImeCompositionWindow();
    InvalidateEditor();
}

void CEditorWindow::OnMouseDrag(LPARAM lParam)
{
    if (!m_lineIndex.IsValid())
        return;

    int x = GET_X_LPARAM(lParam);
    int y = GET_Y_LPARAM(lParam);

    if (m_hStatusBar && IsWindowVisible(m_hStatusBar))
    {
        POINT pt{ x, y };
        MapWindowPoints(m_hwnd, m_hStatusBar, &pt, 1);
        if (pt.y >= 0)
            return;
    }

    DWORD row = 0, col = 0;
    if (!HitTestClient(x, y, &row, &col))
    {
        // 拖出最后一行之下 → 吸附到文末
        DWORD total = static_cast<DWORD>(m_lineIndex.GetLineCount());
        row = total > 0 ? total - 1 : 0;
        col = static_cast<DWORD>(GetLineText(row).size());
    }

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
    RECT cr = EditorRect();
    if (y < scrollMargin && m_scrollLine > 0)
        ScrollToLine(m_scrollLine - 1);
    else if (y > cr.bottom - scrollMargin)
        ScrollToLine(m_scrollLine + 1);

    UpdateStatusBar();
    InvalidateEditor();
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

    EnsureCaretVisible(false);
    UpdateStatusBar();
    UpdateImeCompositionWindow();
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

void CEditorWindow::OnKeyDown(WPARAM wParam)
{
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    if (ctrl)
    {
        switch (wParam)
        {
        case 'S': case 's': OnCommand(shift ? kSaveAsId : kSaveId); return;
        case 'Z': case 'z': Undo(); return;
        case 'Y': case 'y': Redo(); return;
        case 'N': case 'n': OnCommand(kNewId); return;
        case 'O': case 'o': OnCommand(kOpenId); return;
        case 'C': case 'c': CopySelection(); return;
        case 'V': case 'v': PasteFromClipboard(); return;
        // 字号缩放（Ctrl+= / Ctrl+- / Ctrl+0，含小键盘 + -）
        case '=': case VK_OEM_PLUS:    case VK_ADD:    ChangeFontSize(+1.0f); return;
        case '-': case VK_OEM_MINUS:   case VK_SUBTRACT: ChangeFontSize(-1.0f); return;
        case '0': case VK_NUMPAD0:     ChangeFontSize(0.0f);  return;
        case VK_RETURN:
        {
            // Ctrl+回车：在当前行下方插入一个空行，光标落到新行行首
            if (!m_lineIndex.IsValid())
                return;
            m_selAnchorValid = false;
            m_caretCol = static_cast<DWORD>(GetLineText(m_caretRow).size());
            m_selCaretRow = m_caretRow;
            m_selCaretCol = m_caretCol;
            InsertTextAtCaret(L"\r\n");
            return;
        }
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

    // 忽略控制字符（Backspace 等在 OnKeyDown 处理）；\t 制表符与 \r 换行例外
    if (ch < 0x20 && ch != L'\r' && ch != L'\t')
        return;

    if (ch == L'\r')
    {
        // 换行：与打开文件的行尾风格无关，一律写入 \r\n
        InsertTextAtCaret(L"\r\n");
        return;
    }

    InsertTextAtCaret(std::wstring(1, ch));
}
