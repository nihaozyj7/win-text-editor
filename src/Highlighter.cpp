#include "Highlighter.h"
#include <unordered_map>
#include <algorithm>

// 词法状态编码：各语言自行解释，跨行信息只保留最小集
//   C/Sql/Html/Markdown：bit0 = 处于块注释/围栏中
//   Python：1 = ''' 三引号内，2 = """ 三引号内
namespace
{
    constexpr uint32_t kStateFlag = 0x1u;      // 块级状态（块注释/围栏/HTML 注释）
    constexpr uint32_t kPyTriple1 = 0x1u;      // Python ''' 内
    constexpr uint32_t kPyTriple2 = 0x2u;      // Python """ 内

    bool IsIdentStart(wchar_t c)
    {
        return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'_';
    }
    bool IsIdentChar(wchar_t c)
    {
        return IsIdentStart(c) || (c >= L'0' && c <= L'9');
    }
    bool IsDigit(wchar_t c)
    {
        return c >= L'0' && c <= L'9';
    }
    wchar_t Lower(wchar_t c)
    {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
    }

    // 追加 token；len<=0 忽略（相邻同类 token 不合并，渲染层负责并 run）
    void Push(std::vector<Token>& out, uint32_t start, uint32_t len, TokKind kind)
    {
        if (len > 0)
            out.push_back({ start, len, kind });
    }

    // ---- 关键字表：函数级静态，wstring_view 键避免查表分配 ----
    using WordMap = std::unordered_map<std::wstring_view, TokKind>;

    WordMap MakeMap(std::initializer_list<const wchar_t*> words, TokKind kind)
    {
        WordMap m;
        m.reserve(words.size() * 2);
        for (const wchar_t* w : words)
            m.emplace(w, kind);
        return m;
    }

    // C 家族（C/C++/Java/C#/JS/TS/Go/Rust/Swift/PHP 取并集；多出的词对单个语言无害）
    const WordMap& CWords()
    {
        static const WordMap m = [] {
            WordMap m = MakeMap({
                L"abstract", L"alignas", L"alignof", L"and", L"and_eq", L"asm",
                L"as", L"async", L"await", L"base", L"break", L"byte",
                L"case", L"catch", L"chan", L"char8_t", L"class", L"co_await",
                L"co_return", L"co_yield", L"concept", L"const", L"const_cast",
                L"consteval", L"constexpr", L"constinit", L"continue", L"debugger",
                L"declare", L"def", L"default", L"defer", L"del", L"delete",
                L"do", L"dynamic_cast", L"elif", L"else", L"enum", L"except",
                L"explicit", L"export", L"extends", L"extern", L"external",
                L"false", L"final", L"finally", L"fn", L"for", L"foreach",
                L"friend", L"from", L"func", L"function", L"go", L"goto",
                L"impl", L"implements", L"import", L"in", L"include", L"inline",
                L"instanceof", L"interface", L"internal", L"is", L"lambda",
                L"let", L"loop", L"match", L"mod", L"module", L"mutable",
                L"namespace", L"new", L"noexcept", L"nonlocal", L"not", L"null",
                L"nullptr", L"operator", L"or", L"or_eq", L"override", L"package",
                L"pass", L"private", L"protected", L"public", L"raise", L"range",
                L"ref", L"register", L"reinterpret_cast", L"requires", L"return",
                L"sealed", L"sizeof", L"static", L"static_assert", L"static_cast",
                L"struct", L"super", L"suspend", L"switch", L"template", L"this",
                L"throw", L"throws", L"trait", L"true", L"try", L"type", L"typealias",
                L"typedef", L"typeid", L"typename", L"union", L"unsafe", L"use",
                L"using", L"virtual", L"volatile", L"where", L"while", L"with",
                L"xor", L"xor_eq", L"yield",
            }, TokKind::Keyword);
            for (auto& kv : MakeMap({
                L"Any", L"Bool", L"boolean", L"bool", L"byte", L"char", L"char16_t",
                L"char32_t", L"double", L"float", L"f32", L"f64", L"int", L"i8",
                L"i16", L"i32", L"i64", L"isize", L"long", L"object", L"ptrdiff_t",
                L"short", L"signed", L"size_t", L"str", L"string", L"String",
                L"u8", L"u16", L"u32", L"u64", L"usize", L"uint8_t", L"uint16_t",
                L"uint32_t", L"uint64_t", L"int8_t", L"int16_t", L"int32_t",
                L"int64_t", L"uintptr_t", L"intptr_t", L"unsigned", L"void",
                L"wchar_t",
            }, TokKind::Type))
                m.emplace(kv.first, kv.second);
            return m;
        }();
        return m;
    }

    const WordMap& SqlWords()
    {
        static const WordMap m = MakeMap({
            L"add", L"all", L"alter", L"and", L"as", L"asc", L"begin", L"between",
            L"bigint", L"bit", L"blob", L"boolean", L"by", L"cascade", L"case",
            L"char", L"check", L"column", L"commit", L"constraint", L"create",
            L"cross", L"current_date", L"current_time", L"current_timestamp",
            L"database", L"date", L"datetime", L"decimal", L"declare", L"default",
            L"delete", L"desc", L"distinct", L"double", L"drop", L"else", L"end",
            L"exec", L"execute", L"exists", L"foreign", L"from", L"full", L"grant",
            L"group", L"having", L"in", L"index", L"inner", L"insert", L"int",
            L"integer", L"into", L"is", L"join", L"key", L"left", L"like", L"limit",
            L"not", L"null", L"numeric", L"offset", L"on", L"or", L"order", L"outer",
            L"primary", L"procedure", L"real", L"references", L"returning",
            L"revoke", L"right", L"rollback", L"schema", L"select", L"set", L"table",
            L"text", L"then", L"timestamp", L"transaction", L"trigger", L"union",
            L"unique", L"update", L"values", L"varchar", L"view", L"when", L"where",
            L"while",
        }, TokKind::Keyword);
        return m;
    }

    const WordMap& PyWords()
    {
        static const WordMap m = [] {
            WordMap m = MakeMap({
                L"and", L"as", L"assert", L"async", L"await", L"break", L"class",
                L"continue", L"def", L"del", L"elif", L"else", L"except", L"False",
                L"finally", L"for", L"from", L"global", L"if", L"import", L"in",
                L"is", L"lambda", L"None", L"nonlocal", L"not", L"or", L"pass",
                L"raise", L"return", L"True", L"try", L"while", L"with", L"yield",
            }, TokKind::Keyword);
            for (auto& kv : MakeMap({
                L"bool", L"bytes", L"complex", L"dict", L"float", L"frozenset",
                L"int", L"list", L"object", L"range", L"set", L"str", L"tuple",
                L"type",
            }, TokKind::Type))
                m.emplace(kv.first, kv.second);
            return m;
        }();
        return m;
    }

    // 查词：cs=true 时大小写不敏感（SQL）；命中返回对应种类，否则 Plain
    TokKind LookupWord(const WordMap& m, const std::wstring& text,
                       size_t start, size_t len, bool ci)
    {
        if (ci)
        {
            std::wstring tmp;
            tmp.reserve(len);
            for (size_t i = 0; i < len; ++i)
                tmp.push_back(Lower(text[start + i]));
            auto it = m.find(std::wstring_view(tmp));
            return it != m.end() ? it->second : TokKind::Plain;
        }
        auto it = m.find(std::wstring_view(text).substr(start, len));
        return it != m.end() ? it->second : TokKind::Plain;
    }
}

Lang Highlighter::DetectByExtension(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"/\\");
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return Lang::None;
    // 转小写逐段比较，避免额外分配
    std::wstring ext = path.substr(dot + 1);
    for (wchar_t& c : ext)
        c = Lower(c);

    struct Entry { const wchar_t* ext; Lang lang; };
    static const Entry kTable[] = {
        { L"c", Lang::C },      { L"h", Lang::C },      { L"cpp", Lang::C },
        { L"cc", Lang::C },     { L"cxx", Lang::C },    { L"c++", Lang::C },
        { L"hpp", Lang::C },    { L"hh", Lang::C },     { L"hxx", Lang::C },
        { L"cs", Lang::C },     { L"java", Lang::C },   { L"kt", Lang::C },
        { L"kts", Lang::C },    { L"go", Lang::C },     { L"rs", Lang::C },
        { L"swift", Lang::C },  { L"m", Lang::C },      { L"mm", Lang::C },
        { L"js", Lang::C },     { L"mjs", Lang::C },    { L"cjs", Lang::C },
        { L"jsx", Lang::C },    { L"ts", Lang::C },     { L"tsx", Lang::C },
        { L"php", Lang::C },    { L"css", Lang::C },    { L"scss", Lang::C },
        { L"less", Lang::C },
        { L"py", Lang::Python },{ L"pyw", Lang::Python },{ L"pyi", Lang::Python },
        { L"json", Lang::Json },{ L"jsonc", Lang::Json },{ L"json5", Lang::Json },
        { L"md", Lang::Markdown },{ L"markdown", Lang::Markdown },
        { L"mdown", Lang::Markdown },{ L"mkd", Lang::Markdown },
        { L"yml", Lang::Config },{ L"yaml", Lang::Config },{ L"toml", Lang::Config },
        { L"ini", Lang::Config },{ L"cfg", Lang::Config },{ L"conf", Lang::Config },
        { L"properties", Lang::Config },{ L"env", Lang::Config },
        { L"html", Lang::Html },{ L"htm", Lang::Html },{ L"xhtml", Lang::Html },
        { L"xml", Lang::Html },{ L"svg", Lang::Html },
        { L"sql", Lang::Sql },
        { L"log", Lang::Log },
    };
    for (const Entry& e : kTable)
        if (ext == e.ext)
            return e.lang;
    return Lang::None;
}

// ---------------- C 家族 / SQL ----------------

// 共用扫描器：'//'、'#'（PHP/Shell 风格按需）、'/* */' 块注释、引号字符串、数字、标识符
static void LexCLine(const std::wstring& t, uint32_t stateIn,
                     std::vector<Token>& out, uint32_t& stateOut,
                     const WordMap& words, bool ci,
                     bool slashSlash, bool hashComment)
{
    stateOut = 0;
    size_t i = 0;
    const size_t n = t.size();
    bool inBlock = (stateIn & kStateFlag) != 0;
    size_t blockStart = 0;   // 块注释起点（跨行时上一行从 0 记入状态）

    while (i < n)
    {
        if (inBlock)
        {
            size_t j = t.find(L"*/", i);
            if (j == std::wstring::npos)
            {
                Push(out, static_cast<uint32_t>(blockStart),
                     static_cast<uint32_t>(n - blockStart), TokKind::Comment);
                stateOut = kStateFlag;
                return;
            }
            i = j + 2;
            Push(out, static_cast<uint32_t>(blockStart),
                 static_cast<uint32_t>(i - blockStart), TokKind::Comment);
            inBlock = false;
            continue;
        }
        wchar_t c = t[i];
        if (c == L'/' && slashSlash && i + 1 < n && t[i + 1] == L'/')
        {
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(n - i), TokKind::Comment);
            return;
        }
        if (c == L'-' && ci && i + 1 < n && t[i + 1] == L'-')   // SQL 行注释
        {
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(n - i), TokKind::Comment);
            return;
        }
        if (c == L'#' && hashComment)
        {
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(n - i), TokKind::Comment);
            return;
        }
        if (c == L'/' && i + 1 < n && t[i + 1] == L'*')
        {
            blockStart = i;
            i += 2;
            inBlock = true;
            continue;
        }
        if (c == L'"' || c == L'\'' || (c == L'`' && !ci))
        {
            wchar_t q = c;
            size_t j = i + 1;
            while (j < n)
            {
                if (t[j] == L'\\' && q != L'\'' && j + 1 < n) { j += 2; continue; }
                if (t[j] == L'\'' && q == L'\'' && j + 1 < n && t[j + 1] == L'\'')
                { j += 2; continue; }   // SQL 的 '' 转义
                if (t[j] == q) { ++j; break; }
                ++j;
            }
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(j - i), TokKind::String);
            i = j;
            continue;
        }
        if (IsDigit(c))
        {
            size_t j = i + 1;
            while (j < n && (IsIdentChar(t[j]) || t[j] == L'.'))
                ++j;
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(j - i), TokKind::Number);
            i = j;
            continue;
        }
        if (IsIdentStart(c))
        {
            size_t j = i + 1;
            while (j < n && IsIdentChar(t[j]))
                ++j;
            TokKind k = LookupWord(words, t, i, j - i, ci);
            if (k != TokKind::Plain)
                Push(out, static_cast<uint32_t>(i),
                     static_cast<uint32_t>(j - i), k);
            i = j;
            continue;
        }
        if (c == L'#' && i == 0 && !ci)
        {
            // 非注释语言（C 家族）行首 #：预处理指令（SQL 等大小写不敏感语言不适用）
            Push(out, 0, static_cast<uint32_t>(n), TokKind::Preproc);
            return;
        }
        ++i;
    }
}

// ---------------- Python ----------------

static void LexPythonLine(const std::wstring& t, uint32_t stateIn,
                          std::vector<Token>& out, uint32_t& stateOut)
{
    stateOut = 0;
    size_t i = 0;
    const size_t n = t.size();

    // 延续上一行的三引号字符串
    if (stateIn)
    {
        wchar_t q = (stateIn == kPyTriple1) ? L'\'' : L'"';
        size_t j = t.find(std::wstring(3, q), 0);
        if (j == std::wstring::npos)
        {
            Push(out, 0, static_cast<uint32_t>(n), TokKind::String);
            stateOut = stateIn;
            return;
        }
        Push(out, 0, static_cast<uint32_t>(j + 3), TokKind::String);
        i = j + 3;
    }

    while (i < n)
    {
        wchar_t c = t[i];
        if (c == L'#')
        {
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(n - i), TokKind::Comment);
            return;
        }
        if (c == L'"' || c == L'\'')
        {
            std::wstring tri(3, c);
            if (t.compare(i, 3, tri) == 0)
            {
                size_t j = t.find(tri, i + 3);
                if (j == std::wstring::npos)
                {
                    Push(out, static_cast<uint32_t>(i),
                         static_cast<uint32_t>(n - i), TokKind::String);
                    stateOut = (c == L'\'') ? kPyTriple1 : kPyTriple2;
                    return;
                }
                Push(out, static_cast<uint32_t>(i),
                     static_cast<uint32_t>(j + 3 - i), TokKind::String);
                i = j + 3;
                continue;
            }
            size_t j = i + 1;
            while (j < n && t[j] != c)
                j += (t[j] == L'\\' && j + 1 < n) ? 2 : 1;
            if (j < n) ++j;
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(j - i), TokKind::String);
            i = j;
            continue;
        }
        if (IsDigit(c))
        {
            size_t j = i + 1;
            while (j < n && (IsIdentChar(t[j]) || t[j] == L'.'))
                ++j;
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(j - i), TokKind::Number);
            i = j;
            continue;
        }
        if (IsIdentStart(c))
        {
            size_t j = i + 1;
            while (j < n && IsIdentChar(t[j]))
                ++j;
            TokKind k = LookupWord(PyWords(), t, i, j - i, false);
            if (k != TokKind::Plain)
                Push(out, static_cast<uint32_t>(i),
                     static_cast<uint32_t>(j - i), k);
            i = j;
            continue;
        }
        ++i;
    }
}

// ---------------- JSON ----------------

static void LexJsonLine(const std::wstring& t, uint32_t stateIn,
                        std::vector<Token>& out, uint32_t& stateOut)
{
    stateOut = 0;
    (void)stateIn;
    const size_t n = t.size();
    size_t i = 0;
    while (i < n)
    {
        wchar_t c = t[i];
        if (c == L'/' && i + 1 < n && t[i + 1] == L'/')   // JSONC 注释
        {
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(n - i), TokKind::Comment);
            return;
        }
        if (c == L'"')
        {
            size_t j = i + 1;
            while (j < n && t[j] != L'"')
                j += (t[j] == L'\\' && j + 1 < n) ? 2 : 1;
            if (j < n) ++j;
            // 后随冒号的字符串视为键
            size_t k = j;
            while (k < n && t[k] == L' ') ++k;
            Push(out, static_cast<uint32_t>(i), static_cast<uint32_t>(j - i),
                 (k < n && t[k] == L':') ? TokKind::Keyword : TokKind::String);
            i = j;
            continue;
        }
        if (IsDigit(c) || (c == L'-' && i + 1 < n && IsDigit(t[i + 1])))
        {
            size_t j = i + 1;
            while (j < n && (IsIdentChar(t[j]) || t[j] == L'.' ||
                             t[j] == L'+' || t[j] == L'-'))
                ++j;
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(j - i), TokKind::Number);
            i = j;
            continue;
        }
        if (IsIdentStart(c))
        {
            size_t j = i + 1;
            while (j < n && IsIdentChar(t[j]))
                ++j;
            std::wstring_view w = std::wstring_view(t).substr(i, j - i);
            if (w == L"true" || w == L"false" || w == L"null")
                Push(out, static_cast<uint32_t>(i),
                     static_cast<uint32_t>(j - i), TokKind::Keyword);
            i = j;
            continue;
        }
        ++i;
    }
}

// ---------------- Markdown ----------------

static void LexMarkdownLine(const std::wstring& t, uint32_t stateIn,
                            std::vector<Token>& out, uint32_t& stateOut)
{
    stateOut = 0;
    const size_t n = t.size();

    // 行首缩进
    size_t ind = 0;
    while (ind < n && (t[ind] == L' ' || t[ind] == L'\t'))
        ++ind;

    // 围栏代码块：``` / ~~~ 开关；块内整行按 Code 着色
    bool isFence = ind + 3 <= n &&
        ((t.compare(ind, 3, L"```") == 0) || (t.compare(ind, 3, L"~~~") == 0));
    bool inFence = (stateIn & kStateFlag) != 0;
    if (inFence)
    {
        Push(out, 0, static_cast<uint32_t>(n), TokKind::Code);
        if (isFence)
            stateOut = 0;      // 围栏闭合
        else
            stateOut = kStateFlag;
        return;
    }
    if (isFence)
    {
        Push(out, 0, static_cast<uint32_t>(n), TokKind::Code);
        stateOut = kStateFlag;
        return;
    }

    // 标题 / 引用 / 分隔线：整行着色
    if (ind < n && t[ind] == L'#')
    {
        Push(out, 0, static_cast<uint32_t>(n), TokKind::Heading);
        return;
    }
    if (ind < n && t[ind] == L'>')
    {
        Push(out, 0, static_cast<uint32_t>(n), TokKind::Quote);
        return;
    }
    {
        // --- / *** 分隔线
        size_t k = ind;
        if (k < n && (t[k] == L'-' || t[k] == L'*' || t[k] == L'_'))
        {
            wchar_t c = t[k];
            size_t run = 0;
            bool only = true;
            for (; k < n; ++k)
            {
                if (t[k] == c) ++run;
                else if (t[k] != L' ') { only = false; break; }
            }
            if (only && run >= 3)
            {
                Push(out, 0, static_cast<uint32_t>(n), TokKind::Marker);
                return;
            }
        }
    }

    // 列表标记
    size_t i = ind;
    if (i < n && (t[i] == L'-' || t[i] == L'*' || t[i] == L'+'))
    {
        if (i + 1 < n && t[i + 1] == L' ')
        {
            Push(out, static_cast<uint32_t>(i), 1, TokKind::Marker);
            ++i;
        }
    }
    else if (i < n && IsDigit(t[i]))
    {
        size_t j = i;
        while (j < n && IsDigit(t[j])) ++j;
        if (j < n && (t[j] == L'.' || t[j] == L')') && j + 1 < n && t[j + 1] == L' ')
        {
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(j + 1 - i), TokKind::Marker);
            i = j + 1;
        }
    }

    // 行内：`代码`、**粗体**、*斜体*、[链接](url)
    while (i < n)
    {
        wchar_t c = t[i];
        if (c == L'`')
        {
            size_t j = t.find(L'`', i + 1);
            if (j == std::wstring::npos)
            {
                ++i;
                continue;
            }
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(j + 1 - i), TokKind::Code);
            i = j + 1;
            continue;
        }
        if (c == L'*' || c == L'_')
        {
            // ** / __ 双标记优先
            bool dbl = (i + 1 < n && t[i + 1] == c);
            std::wstring close(dbl ? 2 : 1, c);
            size_t j = t.find(close, i + close.size());
            if (j != std::wstring::npos)
            {
                Push(out, static_cast<uint32_t>(i),
                     static_cast<uint32_t>(j + close.size() - i), TokKind::Keyword);
                i = j + close.size();
                continue;
            }
            ++i;
            continue;
        }
        if (c == L'[')
        {
            size_t j = t.find(L"](", i + 1);
            if (j != std::wstring::npos)
            {
                size_t k = t.find(L')', j + 2);
                if (k != std::wstring::npos)
                {
                    Push(out, static_cast<uint32_t>(i),
                         static_cast<uint32_t>(k + 1 - i), TokKind::Link);
                    i = k + 1;
                    continue;
                }
            }
            ++i;
            continue;
        }
        if (c == L'\\' && i + 1 < n)   // 转义
        {
            i += 2;
            continue;
        }
        ++i;
    }
}

// ---------------- 配置（YAML/TOML/INI）----------------

static void LexConfigLine(const std::wstring& t, uint32_t stateIn,
                          std::vector<Token>& out, uint32_t& stateOut)
{
    stateOut = 0;
    (void)stateIn;
    const size_t n = t.size();
    size_t i = 0;
    while (i < n && t[i] == L' ') ++i;

    if (i < n && (t[i] == L'#' || t[i] == L';'))
    {
        Push(out, static_cast<uint32_t>(i),
             static_cast<uint32_t>(n - i), TokKind::Comment);
        return;
    }
    if (i < n && t[i] == L'[')
    {
        size_t j = t.find(L']', i + 1);
        if (j == std::wstring::npos) j = n - 1;
        Push(out, static_cast<uint32_t>(i),
             static_cast<uint32_t>(j + 1 - i), TokKind::Keyword);
        i = j + 1;
    }

    // 键 = 值 / 键: 值：键名着色（Type），其余按字符串/数字/字面量
    size_t k = i;
    while (k < n && t[k] != L'=' && t[k] != L':' && t[k] != L'#')
        ++k;
    if (k > i && k < n && (t[k] == L'=' || t[k] == L':'))
    {
        Push(out, static_cast<uint32_t>(i),
             static_cast<uint32_t>(k - i), TokKind::Type);
        i = k;
    }
    while (i < n)
    {
        wchar_t c = t[i];
        if (c == L'#')
        {
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(n - i), TokKind::Comment);
            return;
        }
        if (c == L'"' || c == L'\'')
        {
            size_t j = i + 1;
            while (j < n && t[j] != c)
                j += (t[j] == L'\\' && j + 1 < n) ? 2 : 1;
            if (j < n) ++j;
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(j - i), TokKind::String);
            i = j;
            continue;
        }
        if (IsDigit(c))
        {
            size_t j = i + 1;
            while (j < n && (IsIdentChar(t[j]) || t[j] == L'.' || t[j] == L'-'))
                ++j;
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(j - i), TokKind::Number);
            i = j;
            continue;
        }
        if (IsIdentStart(c))
        {
            size_t j = i + 1;
            while (j < n && IsIdentChar(t[j]))
                ++j;
            std::wstring_view w = std::wstring_view(t).substr(i, j - i);
            if (w == L"true" || w == L"false" || w == L"null" || w == L"yes" ||
                w == L"no" || w == L"on" || w == L"off")
                Push(out, static_cast<uint32_t>(i),
                     static_cast<uint32_t>(j - i), TokKind::Keyword);
            i = j;
            continue;
        }
        ++i;
    }
}

// ---------------- HTML/XML ----------------

static void LexHtmlLine(const std::wstring& t, uint32_t stateIn,
                        std::vector<Token>& out, uint32_t& stateOut)
{
    stateOut = 0;
    size_t i = 0;
    const size_t n = t.size();
    bool inComment = (stateIn & kStateFlag) != 0;

    if (inComment)
    {
        size_t j = t.find(L"-->", 0);
        if (j == std::wstring::npos)
        {
            Push(out, 0, static_cast<uint32_t>(n), TokKind::Comment);
            stateOut = kStateFlag;
            return;
        }
        Push(out, 0, static_cast<uint32_t>(j + 3), TokKind::Comment);
        i = j + 3;
    }

    while (i < n)
    {
        if (t.compare(i, 4, L"<!--") == 0)
        {
            size_t j = t.find(L"-->", i + 4);
            if (j == std::wstring::npos)
            {
                Push(out, static_cast<uint32_t>(i),
                     static_cast<uint32_t>(n - i), TokKind::Comment);
                stateOut = kStateFlag;
                return;
            }
            Push(out, static_cast<uint32_t>(i),
                 static_cast<uint32_t>(j + 3 - i), TokKind::Comment);
            i = j + 3;
            continue;
        }
        if (t[i] == L'<')
        {
            // 完整标签：<tag attr="v" ...> 或 </tag>
            if (i + 1 < n && (IsIdentStart(t[i + 1]) || t[i + 1] == L'/' ||
                              t[i + 1] == L'!' || t[i + 1] == L'?'))
            {
                size_t j = i + 1;
                while (j < n && t[j] != L'>')
                    ++j;
                if (j >= n)   // 跨行标签：本行按标签着色（状态从简不跨行）
                {
                    Push(out, static_cast<uint32_t>(i),
                         static_cast<uint32_t>(n - i), TokKind::Keyword);
                    return;
                }
                // 标签内细分：标签名 Keyword，属性名 Type，属性值 String
                size_t p = i + 1;
                if (t[p] == L'/') ++p;
                if (t[p] == L'!' || t[p] == L'?')
                {
                    Push(out, static_cast<uint32_t>(i),
                         static_cast<uint32_t>(j + 1 - i), TokKind::Preproc);
                    i = j + 1;
                    continue;
                }
                size_t nameStart = p;
                while (p < j && IsIdentChar(t[p])) ++p;
                Push(out, static_cast<uint32_t>(i),
                     static_cast<uint32_t>(p - i), TokKind::Keyword);
                while (p < j)
                {
                    if (t[p] == L'"' || t[p] == L'\'')
                    {
                        wchar_t q = t[p];
                        size_t q2 = t.find(q, p + 1);
                        if (q2 == std::wstring::npos || q2 > j) q2 = j;
                        Push(out, static_cast<uint32_t>(p),
                             static_cast<uint32_t>(q2 + 1 - p), TokKind::String);
                        p = q2 + 1;
                        continue;
                    }
                    if (IsIdentStart(t[p]))
                    {
                        size_t a = p;
                        while (p < j && IsIdentChar(t[p])) ++p;
                        bool isAttr = (p < j && (t[p] == L'='));
                        if (isAttr)
                            Push(out, static_cast<uint32_t>(a),
                                 static_cast<uint32_t>(p - a), TokKind::Type);
                        p = (p > a) ? p : a + 1;
                        continue;
                    }
                    ++p;
                }
                Push(out, static_cast<uint32_t>(j), 1, TokKind::Keyword);  // '>'
                i = j + 1;
                continue;
            }
            ++i;
            continue;
        }
        ++i;
    }
}

// ---------------- 日志 ----------------

static void LexLogLine(const std::wstring& t, uint32_t stateIn,
                       std::vector<Token>& out, uint32_t& stateOut)
{
    stateOut = 0;
    (void)stateIn;
    const size_t n = t.size();
    size_t i = 0;
    while (i < n)
    {
        wchar_t c = t[i];
        // 时间戳：2026-09-05T12:34:56 / 2026/09/05 12:34:56 / [12:34:56] / 12:34:56.789
        bool ts = false;
        size_t tsLen = 0;
        if (IsDigit(c))
        {
            // 日期前缀 YYYY-MM-DD 或 YYYY/MM/DD
            if (i + 9 < n && IsDigit(t[i + 1]) && IsDigit(t[i + 2]) &&
                IsDigit(t[i + 3]) && t[i + 4] == L'-' &&
                (t[i + 7] == L'-' || t[i + 7] == L'/'))
            {
                ts = true;
                tsLen = 10;
            }
            else if (i + 7 < n && IsDigit(t[i + 1]) && IsDigit(t[i + 2]) &&
                     t[i + 3] == L':')
            {
                ts = true;   // HH:MM:SS
                tsLen = 8;
            }
            if (ts)
            {
                // 吞掉后续时间部分（T12:34:56.789 / 12:34:56 / 时区等）
                size_t j = i + tsLen;
                while (j < n && (IsIdentChar(t[j]) || t[j] == L':' ||
                                 t[j] == L'.' || t[j] == L'+' || t[j] == L'-' ||
                                 t[j] == L'Z'))
                    ++j;
                Push(out, static_cast<uint32_t>(i),
                     static_cast<uint32_t>(j - i), TokKind::Timestamp);
                i = j;
                continue;
            }
        }
        if (c == L'[' && i + 1 < n && IsDigit(t[i + 1]))
        {
            size_t j = t.find(L']', i);
            if (j != std::wstring::npos && j - i <= 32)
            {
                Push(out, static_cast<uint32_t>(i),
                     static_cast<uint32_t>(j + 1 - i), TokKind::Timestamp);
                i = j + 1;
                continue;
            }
        }
        if (IsIdentStart(c))
        {
            size_t j = i + 1;
            while (j < n && IsIdentChar(t[j]))
                ++j;
            std::wstring_view w = std::wstring_view(t).substr(i, j - i);
            TokKind k = TokKind::Plain;
            if (w == L"ERROR" || w == L"FATAL" || w == L"Error" || w == L"error" ||
                w == L"Error:" || w == L"CRITICAL")
                k = TokKind::Error;
            else if (w == L"WARN" || w == L"WARNING" || w == L"Warn" ||
                     w == L"warn" || w == L"warning")
                k = TokKind::Warn;
            else if (w == L"INFO" || w == L"DEBUG" || w == L"Trace" || w == L"trace")
                k = TokKind::Keyword;
            if (k != TokKind::Plain)
                Push(out, static_cast<uint32_t>(i),
                     static_cast<uint32_t>(j - i), k);
            i = j;
            continue;
        }
        ++i;
    }
}

// ---------------- 分发 ----------------

void Highlighter::LexLine(Lang lang, const std::wstring& text, uint32_t stateIn,
                          std::vector<Token>& out, uint32_t& stateOut)
{
    switch (lang)
    {
    case Lang::C:
        LexCLine(text, stateIn, out, stateOut, CWords(), false, true, false);
        break;
    case Lang::Sql:
        LexCLine(text, stateIn, out, stateOut, SqlWords(), true, false, false);
        break;
    case Lang::Python:
        LexPythonLine(text, stateIn, out, stateOut);
        break;
    case Lang::Json:
        LexJsonLine(text, stateIn, out, stateOut);
        break;
    case Lang::Markdown:
        LexMarkdownLine(text, stateIn, out, stateOut);
        break;
    case Lang::Config:
        LexConfigLine(text, stateIn, out, stateOut);
        break;
    case Lang::Html:
        LexHtmlLine(text, stateIn, out, stateOut);
        break;
    case Lang::Log:
        LexLogLine(text, stateIn, out, stateOut);
        break;
    default:
        stateOut = 0;
        break;
    }

    // 防御：token 越界或乱序都会破坏渲染层的并 run 逻辑
    if (!out.empty())
    {
        std::sort(out.begin(), out.end(),
                  [](const Token& a, const Token& b) { return a.start < b.start; });
        uint32_t n = static_cast<uint32_t>(text.size());
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [n](const Token& t) {
                                     return t.start >= n || t.len == 0 ||
                                            t.start + t.len > n;
                                 }),
                  out.end());
    }
}
