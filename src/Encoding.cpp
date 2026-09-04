#include "Encoding.h"

namespace
{
    // UTF-8 严格校验：单码点解码 + 非法序列提前返回 false
    bool IsValidUtf8(const BYTE* p, DWORD n)
    {
        DWORD i = 0;
        while (i < n)
        {
            BYTE c = p[i];
            if (c < 0x80)
            {
                ++i;
                continue;
            }

            int extra;
            DWORD cp;
            if ((c & 0xE0) == 0xC0)      { extra = 1; cp = c & 0x1F; if (cp < 2) return false; }
            else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
            else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; if (cp > 4) return false; }
            else return false;

            if (i + extra >= n)
                return false; // 尾字节不足

            for (int k = 1; k <= extra; ++k)
            {
                BYTE cc = p[i + k];
                if ((cc & 0xC0) != 0x80)
                    return false; // 续字节必须 10xxxxxx
                cp = (cp << 6) | (cc & 0x3F);
            }

            // 拒绝超长编码与代理区，限制合法 Unicode 码点范围
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                return false;
            if (extra == 1 && cp < 0x80) return false;
            if (extra == 2 && cp < 0x800) return false;
            if (extra == 3 && cp < 0x10000) return false;

            i += extra + 1;
        }
        return true;
    }
}

Encoding DetectEncoding(const BYTE* pBuffer, DWORD size)
{
    if (!pBuffer || size < 2)
        return Encoding::Ansi;

    // 1. BOM 检测
    if (size >= 3 && pBuffer[0] == 0xEF && pBuffer[1] == 0xBB && pBuffer[2] == 0xBF)
        return Encoding::Utf8;
    if (pBuffer[0] == 0xFF && pBuffer[1] == 0xFE)
        return Encoding::Utf16LE;
    if (pBuffer[0] == 0xFE && pBuffer[1] == 0xFF)
        return Encoding::Utf16BE;

    // 2. 无 BOM：优先按 UTF-8 校验（采样前 64KB，避免大文件全量扫描）
    DWORD sample = size < 65536 ? size : 65536;
    if (IsValidUtf8(pBuffer, sample))
        return Encoding::Utf8;

    // 3. 回退系统代码页（中文系统 = GBK/CP936）
    return Encoding::Ansi;
}