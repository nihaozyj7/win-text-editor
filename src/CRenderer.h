#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_2.h>
#include <string>
#include <vector>

// D2D/DWrite 渲染器：HwndRenderTarget 生命周期 + 可见行文本绘制
// 输入仅为"可见行列表"（由调用方按需解码），天然满足大文件虚拟滚动
// 支持系统字体回退（中文/Emoji 彩色）、多字体顺序回退（主字体 + 中文回退字体）、
// 自动换行的视觉行堆叠、行号栏、浅色/深色主题与选区高亮
class CRenderer
{
public:
    struct Row
    {
        std::wstring text;      // 行文本（已去除行尾换行符）
        DWORD        row;       // 逻辑行号（用于光标命中判断）
        float        yTop;      // 本逻辑行首条视觉线的 y 坐标（含内边距/换行累计）
        UINT         visualLines;  // 自动换行后占用的视觉行数（≥1）
    };

    struct Selection
    {
        bool  active;
        DWORD startRow;
        DWORD startCol;
        DWORD endRow;
        DWORD endCol;
    };

    // 自定义滚动条几何（客户区坐标；由窗口层计算，渲染层只管画）
    struct ScrollbarDraw
    {
        bool visible = false;
        bool hovered = false;   // 悬停加深
        bool dragged = false;   // 拖拽中最深
        D2D1_RECT_F track{ 0, 0, 0, 0 };   // 整条轨道（供命中测试/布局参考）
        D2D1_RECT_F thumb{ 0, 0, 0, 0 };   // 滑块（圆角矩形）
    };

    CRenderer();
    ~CRenderer();

    HRESULT Init(HWND hwnd);
    void    Destroy();

    // 设备丢失 / 尺寸变化后重建 render target
    HRESULT Resize();

    float GetLineHeight() const;

    // ---- 排版 / 外观设置（下次绘制立即生效）----
    void SetWordWrap(bool wrap);                    // 自动换行开关
    void SetLineHeightFactor(float factor);         // 行高 = 字号 × factor
    void SetFontSize(float size);                  // 字号（重建格式/行高/行号栏宽）
    float GetFontSize() const { return m_fontSize; }
    void SetFonts(const std::wstring& primary,      // 主字体（英文优先）
                  const std::wstring& cjkFallback); // 缺字形时的中文回退字体
    void SetTheme(bool dark);                       // 浅色(false)/深色(true)
    void SetLineNumbers(bool show, DWORD totalLines); // 行号栏开关（重算栏宽）
    float GetGutterWidth() const;                   // 行号栏宽度（关闭时为 0）

    // 绘制一帧可见行；cursorVisible=false 时不画光标
    // textAreaWidth：文本排版可用宽度（去掉内边距/行号栏）
    // originX：文本绘制原点 x（含内边距/行号栏，以及水平滚动偏移）
    // pMaxRowWidth：输出本帧最宽一行的像素宽度（水平滚动范围用，可为 null）
    // vBar/hBar：自定义滚动条几何（visible=false 跳过绘制）
    void Render(const std::vector<Row>& rows, int lineHeight, float textAreaWidth,
                float clientHeight, float originX,
                DWORD caretRow, DWORD caretCol, bool caretVisible,
                const Selection& sel, float* pMaxRowWidth,
                const ScrollbarDraw* vBar = nullptr,
                const ScrollbarDraw* hBar = nullptr);

    // 窗口 DPI 变化后同步 render target 的 DPI（DIP→物理像素换算）
    void UpdateDpi();

    // 反向解析点击位置；返回命中行的 (行号, UTF-16 列)
    // x 相对文本区左缘（调用方已减去内边距/行号栏并加回水平滚动），y 为客户区绝对坐标
    // 返回 false 表示点击在最后一行之下的空白区（调用方按"跳到文末"处理）
    bool HitTestPoint(const std::vector<Row>& rows, int lineHeight, float textAreaWidth,
                      float x, float y, DWORD* pRow, DWORD* pCol) const;

    // 某逻辑行自动换行后占用的视觉行数
    UINT GetRowVisualCount(const std::wstring& text, float maxWidth) const;

    // 光标 (行内列 col) 的相对坐标：x 相对文本区左缘，y 相对该逻辑行顶部
    // （供 IME 组合窗口定位 / 光标可见性计算）
    bool GetCaretPoint(const std::wstring& text, DWORD col, float maxWidth,
                       float* px, float* py) const;

private:
    HRESULT CreateDeviceResources();
    void    ReleaseDeviceResources();
    void    ReleaseTextObjects();
    void    CreateThemeBrushes();
    // 画单条滚动条滑块（圆角、随主题/悬停/拖拽着色）
    void    DrawScrollbar(const ScrollbarDraw& sb);

    // 按 m_fontFamily / m_fontFallbackFamily 重建 TextFormat 与回退链
    void RebuildTextFormats();
    // 构建多字体顺序回退：拉丁区段→主字体，CJK 区段→回退字体，其余走系统回退
    void RebuildFallback();
    // 行号格式行距与正文对齐（均匀行距 + 80% 基线），行高变化后需重调
    void ApplyGutterSpacing();

    // 创建某一行 text layout（调用方负责 Release）；应用行距/换行模式/字体回退
    IDWriteTextLayout* CreateLayoutForRow(const wchar_t* text, UINT32 len,
                                          float maxWidth) const;

    // 把 Emoji 代理对范围强制映射到 Segoe UI Emoji（保证彩色字形）
    void ApplyEmojiFontMapping(IDWriteTextLayout* pLayout, const wchar_t* text, UINT32 len) const;

private:
    HWND m_hwnd;

    ID2D1Factory*          m_pD2DFactory;
    ID2D1HwndRenderTarget* m_pRT;
    IDWriteFactory*        m_pDWriteFactory;
    IDWriteFactory2*       m_pDWriteFactory2;   // 可能为 null（老系统回退）
    IDWriteFontFallback*   m_pSystemFallback;   // 系统字体回退表
    IDWriteFontFallback*   m_pCustomFallback;   // 用户多字体顺序回退链
    IDWriteTextFormat*     m_pTextFormat;
    IDWriteTextFormat*     m_pEmojiFormat;      // Segoe UI Emoji 格式（保证彩色）
    IDWriteTextFormat*     m_pGutterFormat;     // 行号数字格式（右对齐）
    ID2D1SolidColorBrush*  m_pTextBrush;
    ID2D1SolidColorBrush*  m_pBackgroundBrush;
    ID2D1SolidColorBrush*  m_pCaretBrush;
    ID2D1SolidColorBrush*  m_pSelectionBrush;
    ID2D1SolidColorBrush*  m_pGutterBgBrush;
    ID2D1SolidColorBrush*  m_pGutterTextBrush;
    ID2D1SolidColorBrush*  m_pGutterLineBrush;
    ID2D1SolidColorBrush*  m_pScrollbarBrush;

    float m_lineHeight;
    float m_lineHeightFactor;   // 行高 = 字号 × factor
    std::wstring m_fontFamily;
    std::wstring m_fontFallbackFamily;   // 中文回退字体
    float m_fontSize;
    bool  m_wordWrap;
    bool  m_dark;

    bool  m_showLineNumbers;
    DWORD m_lineNumberTotal;
    float m_gutterWidth;
};
