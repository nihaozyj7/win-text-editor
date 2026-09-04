#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>

// D2D/DWrite 渲染器：HwndRenderTarget 生命周期 + 可见行文本绘制
// 输入仅为"可见行列表"（由调用方按需解码），天然满足大文件虚拟滚动
class CRenderer
{
public:
    struct Row
    {
        std::wstring text;  // 行文本（已去除行尾换行符）
        DWORD        row;   // 逻辑行号（用于光标命中判断）
    };

    CRenderer();
    ~CRenderer();

    HRESULT Init(HWND hwnd);
    void    Destroy();

    // 设备丢失 / 尺寸变化后重建 render target；返回重build后是否成功
    HRESULT Resize();

    // 绘制一帧可见行
    void Render(const std::vector<Row>& rows, int lineHeight, int clientWidth,
                DWORD caretRow, DWORD caretCol);

    // 通过 HitTestPoint 反向解析点击位置；返回命中行的 (行号, UTF-16 列)
    // 未命中返回 false
    bool HitTestPoint(const std::vector<Row>& rows, int lineHeight, int clientWidth,
                      float x, float y, DWORD* pRow, DWORD* pCol) const;

    // 当前字体的行高（像素）
    float GetLineHeight() const;

private:
    HRESULT CreateDeviceResources();
    void    ReleaseDeviceResources();

    // 创建某一行 text layout（调用方负责 Release）
    IDWriteTextLayout* CreateLayoutForRow(const wchar_t* text, UINT32 len, float clientWidth, float lineHeight) const;

private:
    HWND m_hwnd;

    ID2D1Factory*          m_pD2DFactory;
    ID2D1HwndRenderTarget* m_pRT;
    IDWriteFactory*        m_pDWriteFactory;
    IDWriteTextFormat*     m_pTextFormat;
    ID2D1SolidColorBrush*  m_pTextBrush;
    ID2D1SolidColorBrush*  m_pBackgroundBrush;
    ID2D1SolidColorBrush*  m_pCaretBrush;

    float m_lineHeight;
    std::wstring m_fontFamily;
    float m_fontSize;
};