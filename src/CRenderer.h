#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_2.h>
#include <string>
#include <vector>

// D2D/DWrite 渲染器：HwndRenderTarget 生命周期 + 可见行文本绘制
// 输入仅为"可见行列表"（由调用方按需解码），天然满足大文件虚拟滚动
// 支持系统字体回退（中文/Emoji 彩色）与选区高亮
class CRenderer
{
public:
    struct Row
    {
        std::wstring text;  // 行文本（已去除行尾换行符）
        DWORD        row;   // 逻辑行号（用于光标命中判断）
    };

    struct Selection
    {
        bool  active;
        DWORD startRow;
        DWORD startCol;
        DWORD endRow;
        DWORD endCol;
    };

    CRenderer();
    ~CRenderer();

    HRESULT Init(HWND hwnd);
    void    Destroy();

    // 设备丢失 / 尺寸变化后重建 render target
    HRESULT Resize();

    // 绘制一帧可见行；cursorVisible=false 时不画光标
    void Render(const std::vector<Row>& rows, int lineHeight, int clientWidth,
                DWORD caretRow, DWORD caretCol, bool caretVisible,
                const Selection& sel);

    // 通过 HitTestPoint 反向解析点击位置；返回命中行的 (行号, UTF-16 列)
    bool HitTestPoint(const std::vector<Row>& rows, int lineHeight, int clientWidth,
                      float x, float y, DWORD* pRow, DWORD* pCol) const;

    // 将文本行某范围绘制为带颜色的绘制文本（供选区文字反色用）
    // 返回该行文字总像素宽（未截断时可用于横向滚动计算）
    float GetLineHeight() const;

private:
    HRESULT CreateDeviceResources();
    void    ReleaseDeviceResources();
    void    ReleaseTextObjects();

    // 创建某一行 text layout（调用方负责 Release）；应用系统字体回退
    IDWriteTextLayout* CreateLayoutForRow(const wchar_t* text, UINT32 len,
                                          float clientWidth, float lineHeight) const;

    // 把 Emoji 代理对范围强制映射到 Segoe UI Emoji（保证彩色字形）
    void ApplyEmojiFontMapping(IDWriteTextLayout* pLayout, const wchar_t* text, UINT32 len) const;

private:
    HWND m_hwnd;

    ID2D1Factory*          m_pD2DFactory;
    ID2D1HwndRenderTarget* m_pRT;
    IDWriteFactory*        m_pDWriteFactory;
    IDWriteFactory2*       m_pDWriteFactory2;   // 可能为 null（老系统回退）
    IDWriteFontFallback*   m_pSystemFallback;   // 系统字体回退表
    IDWriteTextFormat*     m_pTextFormat;
    IDWriteTextFormat*     m_pEmojiFormat;      // Segoe UI Emoji 格式（保证彩色）
    ID2D1SolidColorBrush*  m_pTextBrush;
    ID2D1SolidColorBrush*  m_pBackgroundBrush;
    ID2D1SolidColorBrush*  m_pCaretBrush;
    ID2D1SolidColorBrush*  m_pSelectionBrush;

    float m_lineHeight;
    std::wstring m_fontFamily;
    float m_fontSize;
};