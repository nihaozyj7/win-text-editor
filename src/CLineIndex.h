#pragma once
#include <cstdint>
#include <vector>

// 稀疏行索引：每 1000 行记录一个关键帧（行号 + 字节偏移）
// 支持 O(行数/1000) 定位任意行的字节区间，用于大文件虚拟滚动
class CLineIndex
{
public:
    struct KeyFrame
    {
        uint64_t row;        // 该关键帧所在行号（0 起）
        uint64_t byteOffset; // 该行起始字节偏移（相对文件/缓冲基址）
    };

    CLineIndex();

    // 从字节缓冲构建索引（只读扫描换行符）；构建时间 O(总字节)
    void Build(const unsigned char* base, uint64_t size);

    uint64_t GetLineCount() const;

    // 返回第 row 行（0 起）的起始字节偏移与字节长度（含换行符）
    uint64_t GetLineStart(uint64_t row) const;
    uint64_t GetLineLength(uint64_t row) const;

    // 给定字节偏移定位所在行号（用于光标 → 行）
    uint64_t ByteOffsetToRow(uint64_t offset) const;

private:
    // 处理 \n、\r\n、\r 三种换行
    static uint64_t SkipLineBreak(const unsigned char* base, uint64_t size, uint64_t pos);

private:
    const unsigned char* m_base;
    uint64_t m_size;
    uint64_t m_lineCount;
    std::vector<KeyFrame> m_keyFrames;   // 每 1000 行一帧
    static constexpr uint64_t kFrameInterval = 1000;
};