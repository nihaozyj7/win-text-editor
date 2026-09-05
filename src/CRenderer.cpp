#include "CRenderer.h"
#include <algorithm>

namespace
{
    // Emoji 主区块（补充平面，代理对表示），用于强制映射 Segoe UI Emoji
    bool IsEmojiCodePoint(unsigned int cp)
    {
        return (cp >= 0x1F000 && cp <= 0x1FAFF) ||  // 表情符号区
               (cp >= 0x20000 && cp <= 0x2FFFF) ||  // 补充平面生僻字（保守含入无碍）
               (cp >= 0xE0000 && cp <= 0xE01EF);    // 标签/变体选择符
    }

    // 均匀行距下基线约占行高的 80%（中文/西文观感均衡）
    float BaselineRatio() { return 0.8f; }
}

CRenderer::CRenderer()
    : m_hwnd(nullptr)
    , m_pD2DFactory(nullptr)
    , m_pRT(nullptr)
    , m_pDWriteFactory(nullptr)
    , m_pDWriteFactory2(nullptr)
    , m_pSystemFallback(nullptr)
    , m_pCustomFallback(nullptr)
    , m_pTextFormat(nullptr)
    , m_pEmojiFormat(nullptr)
    , m_pGutterFormat(nullptr)
    , m_pTextBrush(nullptr)
    , m_pTokBrush{}

    , m_pBackgroundBrush(nullptr)
    , m_pCaretBrush(nullptr)
    , m_pSelectionBrush(nullptr)
    , m_pGutterBgBrush(nullptr)
    , m_pGutterTextBrush(nullptr)
    , m_pGutterLineBrush(nullptr)
    , m_pScrollbarBrush(nullptr)
    , m_lineHeight(20.0f)
    , m_lineHeightFactor(1.2f)
    , m_fontFamily(L"Consolas")
    , m_fontFallbackFamily(L"微软雅黑")
    , m_fontSize(14.0f)
    , m_wordWrap(true)
    , m_dark(false)
    , m_showLineNumbers(false)
    , m_lineNumberTotal(0)
    , m_gutterWidth(0.0f)
{
}

CRenderer::~CRenderer()
{
    Destroy();
}

HRESULT CRenderer::Init(HWND hwnd)
{
    m_hwnd = hwnd;
    return CreateDeviceResources();
}

void CRenderer::Destroy()
{
    ReleaseTextObjects();
    ReleaseDeviceResources();
    m_hwnd = nullptr;
}

void CRenderer::CreateThemeBrushes()
{
    if (!m_pRT)
        return;   // RT 重建时按当前 m_dark 创建

    if (m_pTextBrush)      { m_pTextBrush->Release();      m_pTextBrush = nullptr; }
    for (auto& b : m_pTokBrush)
    {
        if (b) { b->Release(); b = nullptr; }
    }
    if (m_pBackgroundBrush){ m_pBackgroundBrush->Release(); m_pBackgroundBrush = nullptr; }
    if (m_pCaretBrush)     { m_pCaretBrush->Release();     m_pCaretBrush = nullptr; }
    if (m_pSelectionBrush) { m_pSelectionBrush->Release(); m_pSelectionBrush = nullptr; }
    if (m_pGutterBgBrush)  { m_pGutterBgBrush->Release();  m_pGutterBgBrush = nullptr; }
    if (m_pGutterTextBrush){ m_pGutterTextBrush->Release(); m_pGutterTextBrush = nullptr; }
    if (m_pGutterLineBrush){ m_pGutterLineBrush->Release(); m_pGutterLineBrush = nullptr; }

    if (m_dark)
    {
        // 深色（VS Code 风格配色）
        m_pRT->CreateSolidColorBrush(D2D1::ColorF(0.831f, 0.831f, 0.831f), &m_pTextBrush);          // #D4D4D4
        m_pRT->CreateSolidColorBrush(D2D1::ColorF(0.118f, 0.118f, 0.118f), &m_pBackgroundBrush);    // #1E1E1E
        m_pRT->CreateSolidColorBrush(D2D1::ColorF(0.910f, 0.910f, 0.910f), &m_pCaretBrush);
        m_pRT->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.35f, 0.55f, 0.55f), &m_pSelectionBrush);
        m_pRT->CreateSolidColorBrush(D2D1::ColorF(0.145f, 0.145f, 0.149f), &m_pGutterBgBrush);      // #252526
        m_pRT->CreateSolidColorBrush(D2D1::ColorF(0.522f, 0.522f, 0.522f), &m_pGutterTextBrush);    // #858585
        m_pRT->CreateSolidColorBrush(D2D1::ColorF(0.247f, 0.247f, 0.275f), &m_pGutterLineBrush);    // #3F3F46
    }
    else
    {
        // 浅色
        m_pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &m_pTextBrush);
        m_pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_pBackgroundBrush);
        m_pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &m_pCaretBrush);
        m_pRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.47f, 0.83f, 0.30f), &m_pSelectionBrush);
        m_pRT->CreateSolidColorBrush(D2D1::ColorF(0.953f, 0.953f, 0.953f), &m_pGutterBgBrush);      // #F3F3F3
        m_pRT->CreateSolidColorBrush(D2D1::ColorF(0.463f, 0.463f, 0.463f), &m_pGutterTextBrush);    // #767676
        m_pRT->CreateSolidColorBrush(D2D1::ColorF(0.851f, 0.851f, 0.851f), &m_pGutterLineBrush);    // #D9D9D9
    }
    // 滚动条滑块颜色随状态变化，用 SetColor 就地改色（浅/深各三档）
    m_pRT->CreateSolidColorBrush(D2D1::ColorF(0.757f, 0.757f, 0.757f), &m_pScrollbarBrush);

    // 语法着色画刷（按 TokKind 下标；配色对齐 VS Code 深色/浅色主题）
    struct TokColor { float r, g, b; };
    static const TokColor kDark[static_cast<int>(TokKind::Count)] = {
        {},                                       // Plain（用正文画刷）
        { 0.337f, 0.612f, 0.839f },               // Keyword  #569CD6
        { 0.306f, 0.788f, 0.690f },               // Type     #4EC9B0
        { 0.808f, 0.569f, 0.471f },               // String   #CE9178
        { 0.416f, 0.600f, 0.333f },               // Comment  #6A9955
        { 0.710f, 0.808f, 0.659f },               // Number   #B5CEA8
        { 0.773f, 0.525f, 0.753f },               // Preproc  #C586C0
        { 0.337f, 0.612f, 0.839f },               // Heading  #569CD6
        { 0.416f, 0.600f, 0.333f },               // Quote    #6A9955
        { 0.843f, 0.729f, 0.490f },               // Marker   #D7BA7D
        { 0.808f, 0.569f, 0.471f },               // Code     #CE9178
        { 0.216f, 0.580f, 1.000f },               // Link     #3794FF
        { 0.957f, 0.529f, 0.443f },               // Error    #F48771
        { 0.800f, 0.655f, 0.000f },               // Warn     #CCA700
        { 0.612f, 0.804f, 0.996f },               // Timestamp#9CDCFE
    };
    static const TokColor kLight[static_cast<int>(TokKind::Count)] = {
        {},                                       // Plain
        { 0.000f, 0.000f, 1.000f },               // Keyword  #0000FF
        { 0.149f, 0.498f, 0.600f },               // Type     #267F99
        { 0.639f, 0.086f, 0.082f },               // String   #A31515
        { 0.000f, 0.502f, 0.000f },               // Comment  #008000
        { 0.035f, 0.525f, 0.345f },               // Number   #098658
        { 0.686f, 0.000f, 0.859f },               // Preproc  #AF00DB
        { 0.016f, 0.318f, 0.647f },               // Heading  #0451A5
        { 0.000f, 0.502f, 0.000f },               // Quote    #008000
        { 0.475f, 0.369f, 0.149f },               // Marker   #795E26
        { 0.639f, 0.086f, 0.082f },               // Code     #A31515
        { 0.000f, 0.416f, 0.694f },               // Link     #006AB1
        { 0.804f, 0.192f, 0.192f },               // Error    #CD3131
        { 0.749f, 0.529f, 0.012f },               // Warn     #BF8803
        { 0.035f, 0.525f, 0.345f },               // Timestamp#098658
    };
    const TokColor* pal = m_dark ? kDark : kLight;
    for (int k = 1; k < static_cast<int>(TokKind::Count); ++k)
    {
        if (!m_pTokBrush[k])
            m_pRT->CreateSolidColorBrush(
                D2D1::ColorF(pal[k].r, pal[k].g, pal[k].b), &m_pTokBrush[k]);
    }
}

HRESULT CRenderer::CreateDeviceResources()
{
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pD2DFactory);
    if (FAILED(hr))
        return hr;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&m_pDWriteFactory));
    if (FAILED(hr))
        return hr;

    // 尝试升级到 IDWriteFactory2 以获取系统字体回退表与自定义回退构建器
    m_pDWriteFactory->QueryInterface(__uuidof(IDWriteFactory2),
                                     reinterpret_cast<void**>(&m_pDWriteFactory2));
    if (m_pDWriteFactory2)
        m_pDWriteFactory2->GetSystemFontFallback(&m_pSystemFallback);

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(rc.right - rc.left),
        static_cast<UINT32>(rc.bottom - rc.top));

    hr = m_pD2DFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0.0f, 0.0f,
            D2D1_RENDER_TARGET_USAGE_NONE,
            D2D1_FEATURE_LEVEL_DEFAULT),
        D2D1::HwndRenderTargetProperties(m_hwnd, size),
        &m_pRT);
    if (FAILED(hr))
        return hr;

    UpdateDpi();   // render target DPI 对齐窗口所在显示器

    RebuildTextFormats();
    CreateThemeBrushes();

    m_lineHeight = m_fontSize * m_lineHeightFactor;

    return S_OK;
}

void CRenderer::ReleaseTextObjects()
{
    if (m_pGutterFormat)     { m_pGutterFormat->Release();     m_pGutterFormat = nullptr; }
    if (m_pEmojiFormat)      { m_pEmojiFormat->Release();      m_pEmojiFormat = nullptr; }
    if (m_pTextFormat)       { m_pTextFormat->Release();       m_pTextFormat = nullptr; }
    if (m_pCustomFallback)   { m_pCustomFallback->Release();   m_pCustomFallback = nullptr; }
    if (m_pSystemFallback)   { m_pSystemFallback->Release();   m_pSystemFallback = nullptr; }
    if (m_pDWriteFactory2)   { m_pDWriteFactory2->Release();   m_pDWriteFactory2 = nullptr; }
    if (m_pCaretBrush)       { m_pCaretBrush->Release();       m_pCaretBrush = nullptr; }
    for (auto& b : m_pTokBrush)
    {
        if (b) { b->Release(); b = nullptr; }
    }
    if (m_pSelectionBrush)   { m_pSelectionBrush->Release();   m_pSelectionBrush = nullptr; }
    if (m_pGutterBgBrush)    { m_pGutterBgBrush->Release();    m_pGutterBgBrush = nullptr; }
    if (m_pGutterTextBrush)  { m_pGutterTextBrush->Release();  m_pGutterTextBrush = nullptr; }
    if (m_pGutterLineBrush)  { m_pGutterLineBrush->Release();  m_pGutterLineBrush = nullptr; }
    if (m_pScrollbarBrush)   { m_pScrollbarBrush->Release();   m_pScrollbarBrush = nullptr; }
    if (m_pBackgroundBrush)  { m_pBackgroundBrush->Release();  m_pBackgroundBrush = nullptr; }
    if (m_pTextBrush)        { m_pTextBrush->Release();        m_pTextBrush = nullptr; }
}

void CRenderer::ReleaseDeviceResources()
{
    ReleaseTextObjects();
    if (m_pRT)              { m_pRT->Release(); m_pRT = nullptr; }
    if (m_pDWriteFactory)   { m_pDWriteFactory->Release(); m_pDWriteFactory = nullptr; }
    if (m_pD2DFactory)      { m_pD2DFactory->Release(); m_pD2DFactory = nullptr; }
}

HRESULT CRenderer::Resize()
{
    if (!m_pRT)
        return E_FAIL;

    UpdateDpi();   // DPI 可能已随显示器/缩放变化，DIP→像素换算保持正确

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(rc.right - rc.left),
        static_cast<UINT32>(rc.bottom - rc.top));
    HRESULT hr = m_pRT->Resize(size);
    if (hr == D2DERR_RECREATE_TARGET)
    {
        ReleaseDeviceResources();
        hr = CreateDeviceResources();
    }
    return hr;
}

// 窗口所在显示器的当前 DPI（Per-Monitor V2 下每块屏/每次缩放都可能不同）；
// GetDpiForWindow 需 Win10 1607+，老系统回退 96
static float WindowDpi(HWND hwnd)
{
    using Fn = UINT(WINAPI*)(HWND);
    static Fn getDpi = []() -> Fn {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        return u32 ? reinterpret_cast<Fn>(GetProcAddress(u32, "GetDpiForWindow")) : nullptr;
    }();
    UINT dpi = getDpi ? getDpi(hwnd) : 96;
    if (dpi == 0)
        dpi = 96;
    return static_cast<float>(dpi);
}

void CRenderer::UpdateDpi()
{
    if (!m_pRT || !m_hwnd)
        return;
    float dpi = WindowDpi(m_hwnd);
    m_pRT->SetDpi(dpi, dpi);
}

float CRenderer::GetLineHeight() const
{
    return m_lineHeight;
}

void CRenderer::SetWordWrap(bool wrap)
{
    m_wordWrap = wrap;
}

void CRenderer::SetLineHeightFactor(float factor)
{
    if (factor < 0.8f)
        factor = 0.8f;
    if (factor > 4.0f)
        factor = 4.0f;
    m_lineHeightFactor = factor;
    m_lineHeight = m_fontSize * m_lineHeightFactor;
    ApplyGutterSpacing();   // 行高变了，行号行距同步
}

void CRenderer::SetFontSize(float size)
{
    if (size < 8.0f)
        size = 8.0f;
    if (size > 72.0f)
        size = 72.0f;
    if (size == m_fontSize)
        return;
    m_fontSize = size;
    m_lineHeight = m_fontSize * m_lineHeightFactor;
    RebuildTextFormats();
    SetLineNumbers(m_showLineNumbers, m_lineNumberTotal);   // 字号变了重算行号栏宽
}

void CRenderer::SetFonts(const std::wstring& primary, const std::wstring& cjkFallback)
{
    if (primary == m_fontFamily && cjkFallback == m_fontFallbackFamily)
        return;
    m_fontFamily = primary;
    m_fontFallbackFamily = cjkFallback;
    RebuildTextFormats();
    SetLineNumbers(m_showLineNumbers, m_lineNumberTotal);   // 字体变了重算行号栏宽
}

void CRenderer::SetTheme(bool dark)
{
    if (m_dark == dark)
        return;
    m_dark = dark;
    CreateThemeBrushes();
}

void CRenderer::RebuildTextFormats()
{
    if (m_pGutterFormat)   { m_pGutterFormat->Release();   m_pGutterFormat = nullptr; }
    if (m_pEmojiFormat)    { m_pEmojiFormat->Release();    m_pEmojiFormat = nullptr; }
    if (m_pTextFormat)     { m_pTextFormat->Release();     m_pTextFormat = nullptr; }
    if (!m_pDWriteFactory)
        return;

    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);

    if (FAILED(m_pDWriteFactory->CreateTextFormat(
            m_fontFamily.c_str(), nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            m_fontSize, locale, &m_pTextFormat)))
        return;

    // 制表位宽度 = 4 em（等宽字体下即 4 字符一跳，与记事本/常规编辑器一致）。
    // 显式设置使 \t 的前进宽度不依赖系统默认值
    m_pTextFormat->SetIncrementalTabStop(m_fontSize * 4.0f);

    // Emoji 专用格式（彩色字形）
    m_pDWriteFactory->CreateTextFormat(
        L"Segoe UI Emoji", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        m_fontSize, locale, &m_pEmojiFormat);

    // 行号数字格式（右对齐；行距与正文一致，见 ApplyGutterSpacing）
    if (SUCCEEDED(m_pDWriteFactory->CreateTextFormat(
            m_fontFamily.c_str(), nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            m_fontSize, locale, &m_pGutterFormat)))
    {
        m_pGutterFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        ApplyGutterSpacing();
    }

    RebuildFallback();
}

// 行号与正文同一基线：正文 layout 用均匀行距(基线 80% 行高)，
// 行号若用默认行距会差 1~2px 导致上下错位，这里强制对齐
void CRenderer::ApplyGutterSpacing()
{
    if (m_pGutterFormat)
        m_pGutterFormat->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                                        m_lineHeight, m_lineHeight * BaselineRatio());
}

void CRenderer::RebuildFallback()
{
    if (m_pCustomFallback) { m_pCustomFallback->Release(); m_pCustomFallback = nullptr; }
    if (!m_pDWriteFactory2)
        return;

    // 多字体顺序回退：先尝试主字体，缺字形再落到中文回退字体，最后系统回退兜底
    IDWriteFontFallbackBuilder* builder = nullptr;
    if (FAILED(m_pDWriteFactory2->CreateFontFallbackBuilder(&builder)) || !builder)
        return;

    // 基础拉丁/符号区段 → 主字体
    DWRITE_UNICODE_RANGE latin[] = { { 0x0000u, 0x2E7Fu } };
    const wchar_t* latinFamilies[] = { m_fontFamily.c_str() };
    builder->AddMapping(latin, 1, latinFamilies, 1);

    // CJK 区段 → 回退字体
    DWRITE_UNICODE_RANGE cjk[] = {
        { 0x2E80u,  0x9FFFu  },   // CJK 部首/符号/统一表意
        { 0xF900u,  0xFAFFu  },   // CJK 兼容表意
        { 0xFF00u,  0xFFEFu  },   // 全角形式
        { 0x20000u, 0x2FA1Fu },   // CJK 扩展 B+
    };
    const wchar_t* cjkFamilies[] = { m_fontFallbackFamily.c_str() };
    builder->AddMapping(cjk, 4, cjkFamilies, 1);

    // 其余区段（Emoji 等）交给系统回退兜底
    if (m_pSystemFallback)
        builder->AddMappings(m_pSystemFallback);

    builder->CreateFontFallback(&m_pCustomFallback);
    builder->Release();
}

void CRenderer::SetLineNumbers(bool show, DWORD totalLines)
{
    m_showLineNumbers = show;
    m_lineNumberTotal = totalLines;
    m_gutterWidth = 0.0f;
    if (!show || !m_pGutterFormat || !m_pDWriteFactory)
        return;

    // 按最大行号位数决定栏宽（至少 2 位，右对齐后左右各留边距）
    DWORD n = totalLines > 0 ? totalLines : 1;
    int digits = 1;
    while (n >= 10) { n /= 10; ++digits; }
    if (digits < 2)
        digits = 2;

    std::wstring probe(static_cast<size_t>(digits), L'8');
    IDWriteTextLayout* layout = nullptr;
    if (SUCCEEDED(m_pDWriteFactory->CreateTextLayout(
            probe.c_str(), static_cast<UINT32>(probe.size()), m_pGutterFormat,
            1000.0f, m_lineHeight, &layout)))
    {
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        layout->Release();
        m_gutterWidth = m.widthIncludingTrailingWhitespace + 14.0f;
    }
    else
    {
        m_gutterWidth = digits * m_fontSize * 0.6f + 14.0f;
    }
}

float CRenderer::GetGutterWidth() const
{
    return m_showLineNumbers ? m_gutterWidth : 0.0f;
}

IDWriteTextLayout* CRenderer::CreateLayoutForRow(const wchar_t* text, UINT32 len,
                                                 float maxWidth) const
{
    if (!m_pDWriteFactory || !m_pTextFormat)
        return nullptr;

    IDWriteTextLayout* pLayout = nullptr;
    if (FAILED(m_pDWriteFactory->CreateTextLayout(text, len, m_pTextFormat,
                                                  (std::max)(1.0f, maxWidth), m_lineHeight, &pLayout)))
        return nullptr;

    // 排版行为与"视觉行堆叠"保持一致：均匀行距 + 当前换行模式。
    // 均匀行距保证换行子行严格按 m_lineHeight 排布，光标/命中的 y 偏移才能对齐。
    pLayout->SetWordWrapping(m_wordWrap ? DWRITE_WORD_WRAPPING_WRAP
                                        : DWRITE_WORD_WRAPPING_NO_WRAP);
    pLayout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                            m_lineHeight, m_lineHeight * BaselineRatio());

    // 显式安装字体回退（优先用户多字体链，其次系统回退）
    IDWriteTextLayout2* pLayout2 = nullptr;
    if (SUCCEEDED(pLayout->QueryInterface(__uuidof(IDWriteTextLayout2),
                                          reinterpret_cast<void**>(&pLayout2))))
    {
        if (m_pCustomFallback)
            pLayout2->SetFontFallback(m_pCustomFallback);
        else if (m_pSystemFallback)
            pLayout2->SetFontFallback(m_pSystemFallback);
        pLayout2->Release();
    }

    return pLayout;
}

void CRenderer::ApplyEmojiFontMapping(IDWriteTextLayout* pLayout,
                                      const wchar_t* text, UINT32 len) const
{
    if (!pLayout || !m_pEmojiFormat)
        return;

    // 扫描代理对；发现 Emoji 则把对应字符范围映射到 Segoe UI Emoji
    DWRITE_TEXT_RANGE range = { 0, 0 };
    bool inEmoji = false;

    auto flush = [&]() {
        if (inEmoji && range.length > 0)
            pLayout->SetFontFamilyName(L"Segoe UI Emoji", range);
    };

    for (UINT32 i = 0; i < len; ++i)
    {
        unsigned int cp = static_cast<unsigned int>(text[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len &&
            text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
        {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (text[i + 1] - 0xDC00);
        }

        bool isEmoji = IsEmojiCodePoint(cp);
        if (isEmoji != inEmoji)
        {
            flush();
            inEmoji = isEmoji;
            range.startPosition = i;
            range.length = 0;
        }
        if (inEmoji)
        {
            // 一个代码点占 1 或 2 个 UTF-16 单元；累加到范围长度
            range.length += (cp > 0xFFFF) ? 2 : 1;
            if (cp > 0xFFFF)
                ++i;   // 跳过代理对的低代理单元
        }
    }
    flush();
}

UINT CRenderer::GetRowVisualCount(const std::wstring& text, float maxWidth) const
{
    if (!m_wordWrap)
        return 1;
    IDWriteTextLayout* layout = CreateLayoutForRow(
        text.c_str(), static_cast<UINT32>(text.size()), maxWidth);
    if (!layout)
        return 1;
    UINT32 lineCount = 0;
    layout->GetLineMetrics(nullptr, 0, &lineCount);
    layout->Release();
    return lineCount > 0 ? static_cast<UINT>(lineCount) : 1;
}

bool CRenderer::GetCaretPoint(const std::wstring& text, DWORD col, float maxWidth,
                              float* px, float* py) const
{
    if (px) *px = 0.0f;
    if (py) *py = 0.0f;
    IDWriteTextLayout* layout = CreateLayoutForRow(
        text.c_str(), static_cast<UINT32>(text.size()), maxWidth);
    if (!layout)
        return false;
    ApplyEmojiFontMapping(layout, text.c_str(), static_cast<UINT32>(text.size()));

    UINT32 cp = static_cast<UINT32>(col < text.size() ? col : text.size());
    float x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS hit{};
    layout->HitTestTextPosition(cp, FALSE, &x, &y, &hit);
    layout->Release();
    if (px) *px = x;
    if (py) *py = y;
    return true;
}

void CRenderer::Render(const std::vector<Row>& rows, int lineHeight, float textAreaWidth,
                       float clientHeight, float originX,
                       DWORD caretRow, DWORD caretCol, bool caretVisible,
                       const Selection& sel, float* pMaxRowWidth,
                       const ScrollbarDraw* vBar, const ScrollbarDraw* hBar)
{
    if (!m_pRT || !m_pBackgroundBrush || !m_pTextBrush)
        return;   // 资源初始化失败（极端情况）时不绘制，避免空指针崩溃

    if (pMaxRowWidth)
        *pMaxRowWidth = 0.0f;

    m_pRT->BeginDraw();
    m_pRT->Clear(m_pBackgroundBrush->GetColor());

    float gutterW = GetGutterWidth();

    // 行号栏背景 + 分隔线（不随水平滚动移动）
    if (m_showLineNumbers && gutterW > 0.0f)
    {
        m_pRT->FillRectangle(D2D1::RectF(0.0f, 0.0f, gutterW, clientHeight),
                             m_pGutterBgBrush);
        m_pRT->FillRectangle(D2D1::RectF(gutterW - 1.0f, 0.0f, gutterW, clientHeight),
                             m_pGutterLineBrush);
    }

    for (size_t i = 0; i < rows.size(); ++i)
    {
        const Row& row = rows[i];
        IDWriteTextLayout* pLayout = CreateLayoutForRow(
            row.text.c_str(), static_cast<UINT32>(row.text.size()), textAreaWidth);
        if (!pLayout)
            continue;

        ApplyEmojiFontMapping(pLayout, row.text.c_str(),
                              static_cast<UINT32>(row.text.size()));

        float y = row.yTop;

        DWRITE_TEXT_METRICS metrics{};
        pLayout->GetMetrics(&metrics);
        if (pMaxRowWidth && metrics.widthIncludingTrailingWhitespace > *pMaxRowWidth)
            *pMaxRowWidth = metrics.widthIncludingTrailingWhitespace;

        // 选区高亮：本行与选区交叠的字符区间绘制半透明背景
        if (sel.active)
        {
            DWORD textLen = static_cast<DWORD>(row.text.size());
            DWORD selStart = 0, selLen = 0;

            if (row.row > sel.startRow && row.row < sel.endRow)
            {
                selStart = 0;
                selLen = textLen;
            }
            else if (row.row == sel.startRow && row.row == sel.endRow)
            {
                if (sel.endCol > sel.startCol)
                {
                    selStart = std::min(sel.startCol, textLen);
                    selLen = std::min(sel.endCol, textLen) - selStart;
                }
            }
            else if (row.row == sel.startRow)
            {
                if (sel.endRow > sel.startRow)
                {
                    selStart = std::min(sel.startCol, textLen);
                    selLen = textLen - selStart;
                }
            }
            else if (row.row == sel.endRow)
            {
                if (sel.endRow > sel.startRow)
                {
                    selStart = 0;
                    selLen = std::min(sel.endCol, textLen);
                }
            }

            if (selLen > 0)
            {
                // HitTestTextRange 返回的 metrics 已含传入的 origin 偏移，
                // 传 (originX, y) 后 left/top 即为最终客户区坐标，不能再加 y
                UINT32 maxHits = selLen + 1;
                std::vector<DWRITE_HIT_TEST_METRICS> hits(maxHits);
                UINT32 hitCount = 0;
                HRESULT hr = pLayout->HitTestTextRange(
                    static_cast<UINT32>(selStart), static_cast<UINT32>(selLen),
                    originX, y, hits.data(), maxHits, &hitCount);
                if (SUCCEEDED(hr) && hitCount > 0)
                {
                    if (hitCount > maxHits)
                        hitCount = maxHits;
                    for (UINT32 h = 0; h < hitCount; ++h)
                    {
                        D2D1_RECT_F r = D2D1::RectF(
                            hits[h].left, hits[h].top,
                            hits[h].left + hits[h].width,
                            hits[h].top + hits[h].height);
                        m_pRT->FillRectangle(r, m_pSelectionBrush);
                    }
                }
            }
        }

        // 语法着色：按 token 区间分段裁剪整行 layout 绘制（每个像素只画一次，
        // 颜色来自该段画刷）。走 DrawTextLayout 保留 Emoji 彩色字形能力。
        // 超长行的 token 数过多时退回整行单色，避免每帧大量裁剪绘制
        if (row.tokens.empty() || row.tokens.size() > 64)
        {
            m_pRT->DrawTextLayout(
                D2D1::Point2F(originX, y), pLayout, m_pTextBrush,
                D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        }
        else
        {
            const UINT32 lineLen = static_cast<UINT32>(row.text.size());
            auto drawRun = [&](uint32_t s, uint32_t e, TokKind kind)
            {
                if (e <= s)
                    return;
                ID2D1Brush* brush = m_pTextBrush;
                int k = static_cast<int>(kind);
                if (k > 0 && k < static_cast<int>(TokKind::Count) && m_pTokBrush[k])
                    brush = m_pTokBrush[k];

                UINT32 maxHits = (e - s) + 1;
                std::vector<DWRITE_HIT_TEST_METRICS> hits(maxHits);
                UINT32 hitCount = 0;
                if (FAILED(pLayout->HitTestTextRange(s, e - s, originX, y,
                                                     hits.data(), maxHits, &hitCount)))
                    return;
                if (hitCount > maxHits)
                    hitCount = maxHits;
                for (UINT32 h = 0; h < hitCount; ++h)
                {
                    m_pRT->PushAxisAlignedClip(
                        D2D1::RectF(hits[h].left, hits[h].top,
                                    hits[h].left + hits[h].width,
                                    hits[h].top + hits[h].height),
                        D2D1_ANTIALIAS_MODE_ALIASED);
                    m_pRT->DrawTextLayout(
                        D2D1::Point2F(originX, y), pLayout, brush,
                        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                    m_pRT->PopAxisAlignedClip();
                }
            };

            uint32_t pos = 0;
            for (const Token& t : row.tokens)
            {
                uint32_t s = std::min<uint32_t>(t.start, lineLen);
                uint32_t e = std::min<uint32_t>(t.start + t.len, lineLen);
                if (s > pos)
                    drawRun(pos, s, TokKind::Plain);   // token 之间的间隙
                drawRun(s, e, t.kind);
                pos = std::max(pos, e);
            }
            if (pos < lineLen)
                drawRun(pos, lineLen, TokKind::Plain);
        }

        // 行号数字（右对齐于行号栏内）
        if (m_showLineNumbers && gutterW > 0.0f && m_pGutterFormat)
        {
            wchar_t num[16];
            wsprintfW(num, L"%u", static_cast<unsigned>(row.row + 1));
            D2D1_RECT_F numRect = D2D1::RectF(4.0f, y, gutterW - 6.0f, y + lineHeight);
            m_pRT->DrawTextW(num, static_cast<UINT32>(wcslen(num)), m_pGutterFormat,
                             numRect, m_pGutterTextBrush);
        }

        // 光标：仅在可见且位于该行时绘制。
        // HitTestTextPosition 的 y 含换行子行偏移，自动换行后光标跟随行尾。
        // 光标高度 = 字体高度（约 1.2 倍字号），底部对齐基线（均匀行距下基线
        // 位于 80% 行高处），而非整个行高
        if (caretVisible && row.row == caretRow)
        {
            DWRITE_HIT_TEST_METRICS hit{};
            float x = 0, yAfter = 0;
            UINT32 cp = static_cast<UINT32>(caretCol < row.text.size() ? caretCol : row.text.size());
            pLayout->HitTestTextPosition(cp, FALSE, &x, &yAfter, &hit);
            float caretH = m_fontSize * 1.2f;
            float baseline = y + yAfter + m_lineHeight * BaselineRatio();
            float caretTop = baseline - caretH;
            if (caretTop < y)
                caretTop = y;   // 防御：行高过小时不越过该行顶部
            m_pRT->FillRectangle(
                D2D1::RectF(originX + x, caretTop, originX + x + 1.5f, baseline),
                m_pCaretBrush);
        }

        pLayout->Release();
    }

    // 自定义滚动条（最后画，覆盖在内容之上）
    if (vBar)
        DrawScrollbar(*vBar);
    if (hBar)
        DrawScrollbar(*hBar);

    HRESULT hrEnd = m_pRT->EndDraw();
    if (hrEnd == D2DERR_RECREATE_TARGET)
        Resize();
}

// 现代扁平风格滑块：无箭头、圆角、悬停/拖拽加深，颜色随主题
void CRenderer::DrawScrollbar(const ScrollbarDraw& sb)
{
    if (!sb.visible || !m_pScrollbarBrush)
        return;

    float w = sb.thumb.right - sb.thumb.left;
    float h = sb.thumb.bottom - sb.thumb.top;
    if (w <= 0 || h <= 0)
        return;

    D2D1_COLOR_F color;
    if (m_dark)
    {
        // 深色：#424242 → hover #686868 → drag #9B9B9B
        color = sb.dragged ? D2D1::ColorF(0.608f, 0.608f, 0.608f)
              : sb.hovered ? D2D1::ColorF(0.408f, 0.408f, 0.408f)
                           : D2D1::ColorF(0.259f, 0.259f, 0.259f);
    }
    else
    {
        // 浅色：#C1C1C1 → hover #A8A8A8 → drag #787878
        color = sb.dragged ? D2D1::ColorF(0.471f, 0.471f, 0.471f)
              : sb.hovered ? D2D1::ColorF(0.659f, 0.659f, 0.659f)
                           : D2D1::ColorF(0.757f, 0.757f, 0.757f);
    }
    m_pScrollbarBrush->SetColor(color);

    float radius = (w < h ? w : h) * 0.5f;
    D2D1_ROUNDED_RECT rr = { sb.thumb, radius, radius };
    m_pRT->FillRoundedRectangle(rr, m_pScrollbarBrush);
}

bool CRenderer::HitTestPoint(const std::vector<Row>& rows, int lineHeight, float textAreaWidth,
                             float x, float y, DWORD* pRow, DWORD* pCol) const
{
    if (!pRow || !pCol || rows.empty())
        return false;

    // 按 yTop 区间定位逻辑行（自动换行后一个逻辑行占多条视觉线）
    const Row* hitRow = nullptr;
    float yLocal = 0.0f;
    if (y < rows.front().yTop)
    {
        hitRow = &rows.front();
        yLocal = y - rows.front().yTop;   // 吸附到首行顶部
    }
    else
    {
        for (const auto& r : rows)
        {
            float bottom = r.yTop + static_cast<float>(r.visualLines) * lineHeight;
            if (y < bottom)
            {
                hitRow = &r;
                yLocal = y - r.yTop;
                break;
            }
        }
        if (!hitRow)
            return false;   // 最后一行之下的空白区 → 调用方按"跳到文末"处理
    }

    *pRow = hitRow->row;

    IDWriteTextLayout* pLayout = CreateLayoutForRow(
        hitRow->text.c_str(), static_cast<UINT32>(hitRow->text.size()), textAreaWidth);
    if (!pLayout)
    {
        *pCol = 0;
        return true;
    }

    // 与 Render 保持一致：Emoji 字体映射会影响字符宽度，命中才能对齐
    this->ApplyEmojiFontMapping(pLayout, hitRow->text.c_str(),
                                static_cast<UINT32>(hitRow->text.size()));

    BOOL isTrailing = FALSE;
    BOOL isInside = FALSE;
    DWRITE_HIT_TEST_METRICS hit{};
    HRESULT hr = pLayout->HitTestPoint(x, yLocal, &isTrailing, &isInside, &hit);
    pLayout->Release();

    if (SUCCEEDED(hr))
    {
        // 命中簇的右半区（isTrailing）→ 光标落在本簇末尾（+length），
        // 否则点在字符中间会被吸附到簇首，选区比用户点的位置少一个字符
        if (isTrailing)
            *pCol = hit.textPosition + hit.length;
        else
            *pCol = hit.textPosition;
    }
    else
        *pCol = static_cast<DWORD>(hitRow->text.size());

    if (*pCol > hitRow->text.size())
        *pCol = static_cast<DWORD>(hitRow->text.size());

    return true;
}
