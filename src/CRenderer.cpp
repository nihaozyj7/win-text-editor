#include "CRenderer.h"

CRenderer::CRenderer()
    : m_hwnd(nullptr)
    , m_pD2DFactory(nullptr)
    , m_pRT(nullptr)
    , m_pDWriteFactory(nullptr)
    , m_pTextFormat(nullptr)
    , m_pTextBrush(nullptr)
    , m_pBackgroundBrush(nullptr)
    , m_pCaretBrush(nullptr)
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

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(rc.right - rc.left),
        static_cast<UINT32>(rc.bottom - rc.top));

    hr = m_pD2DFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            0.0f, 0.0f,
            D2D1_RENDER_TARGET_USAGE_NONE,
            D2D1_FEATURE_LEVEL_DEFAULT),
        D2D1::HwndRenderTargetProperties(m_hwnd, size),
        &m_pRT);
    if (FAILED(hr))
        return hr;

    hr = m_pDWriteFactory->CreateTextFormat(
        m_fontFamily.c_str(), nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        m_fontSize, L"", &m_pTextFormat);
    if (FAILED(hr))
        return hr;

    m_pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &m_pTextBrush);
    m_pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_pBackgroundBrush);
    m_pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &m_pCaretBrush);

    // 行高估算（固定比例，简单可靠）
    m_lineHeight = m_fontSize * 1.33f;

    return S_OK;
}

void CRenderer::ReleaseDeviceResources()
{
    if (m_pCaretBrush)      { m_pCaretBrush->Release(); m_pCaretBrush = nullptr; }
    if (m_pBackgroundBrush) { m_pBackgroundBrush->Release(); m_pBackgroundBrush = nullptr; }
    if (m_pTextBrush)       { m_pTextBrush->Release(); m_pTextBrush = nullptr; }
    if (m_pTextFormat)      { m_pTextFormat->Release(); m_pTextFormat = nullptr; }
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
    IDWriteTextLayout* pLayout = nullptr;
    if (FAILED(m_pDWriteFactory->CreateTextLayout(text, len, m_pTextFormat,
                                                  clientWidth, lineHeight, &pLayout)))
        return nullptr;
    return pLayout;
}

void CRenderer::Render(const std::vector<Row>& rows, int lineHeight, int clientWidth,
                       DWORD caretRow, DWORD caretCol)
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

        float y = static_cast<float>(i * lineHeight);
        m_pRT->DrawTextLayout(D2D1::Point2F(0.0f, y), pLayout, m_pTextBrush);

        // 光标行：用 HitTestTextPosition 定位光标 x 坐标
        if (row.row == caretRow)
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

    // 反向 HitTest：x → UTF-16 列
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