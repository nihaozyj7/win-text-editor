#pragma once
#include <windows.h>
#include <string>

// 文本编码类型（覆盖 v1.0 需要的 UTF-8/UTF-16/ANSI/GBK）
enum class Encoding
{
    Utf8,       // UTF-8（含 BOM 或无 BOM）
    Utf16LE,    // UTF-16 小端（BOM FF FE）
    Utf16BE,    // UTF-16 大端（BOM FE FF）
    Ansi,       // 系统代码页（中文系统即 GBK/CP936）
};

// 检测编码：优先 BOM，其次 UTF-8 有效性校验，最后回退系统代码页 AN/GBK
// pBuffer 只需提供文件开头若干字节（建议 4KB）即可
Encoding DetectEncoding(const BYTE* pBuffer, DWORD size);

// 把指定编码的字节流解码为 UTF-16（wstring）；pBuffer 不要求以 NUL 结尾
std::wstring DecodeToWide(const BYTE* pBuffer, DWORD size, Encoding enc);

// 把 UTF-16 文本编码为指定编码的字节流；UTF-8 不含 BOM（保存时由调用方决定是否写 BOM）
std::string EncodeFromWide(const std::wstring& text, Encoding enc);