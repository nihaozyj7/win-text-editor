#pragma once
// 语法高亮：轻量词法着色（关键词匹配 + 跨行状态机，非完整语法解析器）
// 输入一行 UTF-16 文本与上一行结束时的词法状态，输出 token 区间列表与新的行末状态。
// 调用方（窗口层）需按行顺序调用并缓存各行状态；语言按文件扩展名检测。
// 同族语言共用同一状态机（如 C 家族），仅关键字表不同。

#include <cstdint>
#include <string>
#include <vector>

// token 种类（渲染层按主题映射为颜色；枚举顺序即画刷数组下标，勿随意调整）
enum class TokKind : uint8_t
{
    Plain = 0,   // 普通文本（渲染层用正文画刷，不进 token 列表）
    Keyword,     // 关键字 / Markdown 粗斜体 / 日志 INFO/DEBUG
    Type,        // 内建类型 / YAML·INI 的键
    String,      // 字符串字面量 / Markdown 行内代码
    Comment,     // 注释
    Number,      // 数字字面量
    Preproc,     // 预处理指令 / DOCTYPE
    Heading,     // Markdown 标题行
    Quote,       // Markdown 引用行
    Marker,      // Markdown 列表标记
    Code,        // Markdown 围栏代码块（整行）
    Link,        // Markdown 链接 [text](url)
    Error,       // 日志 ERROR/FATAL
    Warn,        // 日志 WARN/WARNING
    Timestamp,   // 日志时间戳
    Count
};

struct Token
{
    uint32_t start;   // 起始偏移（UTF-16 单元，含）
    uint32_t len;     // 长度（UTF-16 单元）
    TokKind   kind;
};

// 支持的语言（按扩展名归类；同族共用词法器）
enum class Lang : uint8_t
{
    None = 0,   // 纯文本，不高亮
    C,          // C 家族：C/C++/Java/C#/JS/TS/Go/Rust/Swift/PHP（CSS 复用注释/字符串规则）
    Python,     // 含三引号跨行字符串
    Json,       // JSON / JSONC
    Markdown,   // 围栏代码块跨行
    Config,     // YAML / TOML / INI
    Html,       // HTML / XML
    Sql,
    Log,        // 日志级别/时间戳着色（非语言）
};

class Highlighter
{
public:
    // 按文件路径的扩展名检测语言；无法识别返回 None
    static Lang DetectByExtension(const std::wstring& path);

    // 对一行做词法着色。stateIn 传上一行的行末状态（首行传 0）；
    // token 追加到 out（不清空，方便调用方复用缓冲）；stateOut 为本行行末状态
    static void LexLine(Lang lang, const std::wstring& text, uint32_t stateIn,
                        std::vector<Token>& out, uint32_t& stateOut);
};
