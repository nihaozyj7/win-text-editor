#pragma once
#include <cstdint>
#include <vector>
#include <functional>
#include "Encoding.h"

// 稀疏行索引：每 1000 行记录一个关键帧（行号 + 字节偏移）
// 数据源抽象为"顺序读取回调"，可指向 MMF 或 PieceTable 逻辑字节流
// 换行符扫描按编码感知（UTF-16 以 code unit 为单位，避免把字节 0x0A 误判为换行）
class CLineIndex
{
public:
    // 顺序读取：把逻辑偏移 ofs 起最多 maxLen 字节拷贝到 dst，返回实际拷贝数
    using ByteReader = std::function<uint64_t(uint64_t ofs, unsigned char* dst, uint64_t maxLen)>;

    struct KeyFrame
    {
        uint64_t row;        // 该关键帧所在行号（0 起）
        uint64_t byteOffset; // 该行起始字节偏移（逻辑字节）
    };

    CLineIndex();

    // 便捷入口：从连续内存（MMF 等）构建；编码用于换行扫描
    void Build(const unsigned char* base, uint64_t size, Encoding enc = Encoding::Utf8);

    // 设置数据源并全量构建索引；构建时间 O(总字节)
    void Build(ByteReader reader, uint64_t size, Encoding enc);

    // 编辑增量：逻辑字节 [ofs, ofs+len) 处插入 deltaBytes 字节，
    // 净行变化 deltaLines（插入换行 +n / 删除换行 -n，无行结构变化为 0）。
    // 仅平移受影响关键帧的偏移/行号，O(帧数)；帧间距漂移过大时自动全量重建
    void NotifyEdit(uint64_t ofs, int64_t deltaBytes, int64_t deltaLines);

    uint64_t GetLineCount() const;
    bool     IsValid() const;   // 已设置数据源（含空文档，此时视为 1 个空行）

    // 返回第 row 行（0 起）的起始字节偏移与字节长度（含行尾换行符序列）
    uint64_t GetLineStart(uint64_t row) const;
    uint64_t GetLineLength(uint64_t row) const;

    // 给定字节偏移定位所在行号（用于光标 → 行）
    uint64_t ByteOffsetToRow(uint64_t offset) const;

private:
    struct ScanState
    {
        ByteReader reader;
        uint64_t size;
        Encoding enc;
        uint32_t unitBytes;      // 1 (ANSI/UTF-8) 或 2 (UTF-16)
        bool     utf16Swap;      // UTF-16BE 需要交换字节序
        unsigned char* block;    // 块缓存（非拥有，指向 m_block）
        mutable uint64_t blockStart;
        mutable uint64_t blockLen;
        const uint64_t kBlockSize = 1 << 16;

        // 读取一个 code unit（1 或 2 字节，按编码交换字节序）；返回 -1 表示越界
        int64_t ReadUnit(uint64_t byteOfs) const;
        // 找到从 byteOfs 起的下一个行尾 unit 偏移（返回行尾 unit 的字节偏移），无则返回 size
        uint64_t FindBreak(uint64_t byteOfs) const;
        // 从行尾 unit 偏移跳过换行符序列，返回下一行首字节偏移
        uint64_t SkipBreak(uint64_t breakByteOfs) const;
    };

    void BuildAll();
    void RebuildAll();   // 强制全量重建（帧漂移兜底）
    // 定位行号 <= row 的最近关键帧索引
    size_t FindFrame(uint64_t row) const;

private:
    ScanState m_scan;
    std::vector<unsigned char> m_blockStorage;
    uint64_t m_lineCount;
    std::vector<KeyFrame> m_keyFrames;   // 每 1000 行一帧（允许编辑后微小漂移）
    static constexpr uint64_t kFrameInterval = 1000;
    uint64_t m_editEpoch;                // 编辑次数，用于节流全量重建
};