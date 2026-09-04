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
}

CRenderer::CRenderer()
    : m_hwnd(nullptr)
    , m_pD2DFactory(nullptr)
    , m_pRT(nullptr)
    , m_pDWriteFactory(nullptr)
    , m_pDWriteFactory2(nullptr)
    , m_pSystemFallback(nullptr)
    , m_pTextFormat(nullptr)
    , m_pEmojiFormat(nullptr)
    , m_pTextBrush(nullptr)
    , m_pBackgroundBrush(nullptr)
    , m_pCaretBrush(nullptr)
    , m_pSelectionBrush(nullptr)
    , m_lineHeight(20.0f)
    , m_fontFamily(L"Consolas")
    , m_fontSize(14.0f)
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

HRESULT CRenderer::CreateDeviceResources()
{
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pD2DFactory);
    if (FAILED(hr))
        return hr;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&m_pDWriteFactory));
    if (FAILED(hr))
        return hr;

    // 尝试升级到 IDWriteFactory2 以获取系统字体回退表
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

    // 获取用户 locale，保证字体回退按用户语言查找
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);

    hr = m_pDWriteFactory->CreateTextFormat(
        m_fontFamily.c_str(), nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        m_fontSize, locale, &m_pTextFormat);
    if (FAILED(hr))
        return hr;

    // Emoji 专用格式（彩色字形）
    m_pDWriteFactory->CreateTextFormat(
        L"Segoe UI Emoji", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        m_fontSize, locale, &m_pEmojiFormat);

    m_pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &m_pTextBrush);
    m_pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_pBackgroundBrush);
    m_pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &m_pCaretBrush);
    m_pRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.47f, 0.83f, 0.25f), &m_pSelectionBrush);

    m_lineHeight = m_fontSize * 1.35f;

    return S_OK;
}

void CRenderer::ReleaseTextObjects()
{
    if (m_pEmojiFormat)      { m_pEmojiFormat->Release(); m_pEmojiFormat = nullptr; }
    if (m_pTextFormat)       { m_pTextFormat->Release(); m_pTextFormat = nullptr; }
    if (m_pSystemFallback)   { m_pSystemFallback->Release(); m_pSystemFallback = nullptr; }
    if (m_pDWriteFactory2)   { m_pDWriteFactory2->Release(); m_pDWriteFactory2 = nullptr; }
    if (m_pCaretBrush)       { m_pCaretBrush->Release(); m_pCaretBrush = nullptr; }
    if (m_pSelectionBrush)   { m_pSelectionBrush->Release(); m_pSelectionBrush = nullptr; }
    if (m_pBackgroundBrush)  { m_pBackgroundBrush->Release(); m_pBackgroundBrush = nullptr; }
    if (m_pTextBrush)        { m_pTextBrush->Release(); m_pTextBrush = nullptr; }
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

float CRenderer::GetLineHeight() const
{
    return m_lineHeight;
}

IDWriteTextLayout* CRenderer::CreateLayoutForRow(const wchar_t* text, UINT32 len,
                                                 float clientWidth, float lineHeight) const
{
    if (!m_pDWriteFactory || !m_pTextFormat)
        return nullptr;

    IDWriteTextLayout* pLayout = nullptr;
    if (FAILED(m_pDWriteFactory->CreateTextLayout(text, len, m_pTextFormat,
                                                  clientWidth, lineHeight, &pLayout)))
        return nullptr;

    // 显式安装系统字体回退（中文/Emoji 缺字形时自动匹配）
    if (m_pSystemFallback)
    {
        IDWriteTextLayout2* pLayout2 = nullptr;
        if (SUCCEEDED(pLayout->QueryInterface(__uuidof(IDWriteTextLayout2),
                                              reinterpret_cast<void**>(&pLayout2))))
        {
            pLayout2->SetFontFallback(m_pSystemFallback);
            pLayout2->Release();
        }
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

void CRenderer::Render(const std::vector<Row>& rows, int lineHeight, int clientWidth,
                       DWORD caretRow, DWORD caretCol, bool caretVisible,
                       const Selection& sel)
{
    if (!m_pRT)
        return;

    m_pRT->BeginDraw();
    m_pRT->Clear(D2D1::ColorF(D2D1::ColorF::White));

    for (size_t i = 0; i < rows.size(); ++i)
    {
        const Row& row = rows[i];
        IDWriteTextLayout* pLayout = CreateLayoutForRow(
            row.text.c_str(), static_cast<UINT32>(row.text.size()),
            static_cast<float>(clientWidth), static_cast<float>(lineHeight));
        if (!pLayout)
            continue;

        ApplyEmojiFontMapping(pLayout, row.text.c_str(),
                              static_cast<UINT32>(row.text.size()));

        float y = static_cast<float>(i * lineHeight);

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
                // HitTestTextRange 需要调用方预分配 metrics 数组（每字符至多一项）
                UINT32 maxHits = selLen + 1;
                std::vector<DWRITE_HIT_TEST_METRICS> hits(maxHits);
                UINT32 hitCount = 0;
                HRESULT hr = pLayout->HitTestTextRange(
                    static_cast<UINT32>(selStart), static_cast<UINT32>(selLen),
                    0.0f, y, hits.data(), maxHits, &hitCount);
                if (SUCCEEDED(hr) && hitCount > 0)
                {
                    for (UINT32 h = 0; h < hitCount; ++h)
                    {
                        D2D1_RECT_F r = D2D1::RectF(
                            hits[h].left, y + hits[h].top,
                            hits[h].left + hits[h].width,
                            y + hits[h].top + hits[h].height);
                        m_pRT->FillRectangle(r, m_pSelectionBrush);
                    }
                }
            }
        }

        m_pRT->DrawTextLayout(D2D1::Point2F(0.0f, y), pLayout, m_pTextBrush);

        // 光标：仅在可见且位于该行时绘制
        if (caretVisible && row.row == caretRow)
        {
            DWRITE_HIT_TEST_METRICS hit{};
            float x = 0, yAfter = 0;
            UINT32 cp = static_cast<UINT32>(caretCol < row.text.size() ? caretCol : row.text.size());
            pLayout->HitTestTextPosition(cp, FALSE, &x, &yAfter, &hit);
            m_pRT->FillRectangle(
                D2D1::RectF(x, y, x + 1.5f, y + static_cast<float>(lineHeight)),
                m_pCaretBrush);
        }

        pLayout->Release();
    }

    HRESULT hrEnd = m_pRT->EndDraw();
    if (hrEnd == D2DERR_RECREATE_TARGET)
        Resize();
}

bool CRenderer::HitTestPoint(const std::vector<Row>& rows, int lineHeight, int clientWidth,
                             float x, float y, DWORD* pRow, DWORD* pCol) const
{
    if (!pRow || !pCol)
        return false;

    int rowIndex = static_cast<int>(y) / lineHeight;
    if (rowIndex < 0 || rowIndex >= static_cast<int>(rows.size()))
        return false;

    const Row& row = rows[rowIndex];
    *pRow = row.row;

    IDWriteTextLayout* pLayout = CreateLayoutForRow(
        row.text.c_str(), static_cast<UINT32>(row.text.size()),
        static_cast<float>(clientWidth), static_cast<float>(lineHeight));
    if (!pLayout)
    {
        *pCol = 0;
        return true;
    }

    BOOL isTrailing = FALSE;
    BOOL isInside = FALSE;
    DWRITE_HIT_TEST_METRICS hit{};
    HRESULT hr = pLayout->HitTestPoint(x, y - static_cast<float>(rowIndex * lineHeight),
                                       &isTrailing, &isInside, &hit);
    pLayout->Release();

    if (SUCCEEDED(hr))
        *pCol = static_cast<DWORD>(hit.textPosition);
    else
        *pCol = static_cast<DWORD>(row.text.size());

    return true;
}