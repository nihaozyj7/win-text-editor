#include "CLineIndex.h"

CLineIndex::CLineIndex()
    : m_base(nullptr)
    , m_size(0)
    , m_lineCount(0)
{
}

void CLineIndex::Build(const unsigned char* base, uint64_t size)
{
    m_base = base;
    m_size = size;
    m_lineCount = 0;
    m_keyFrames.clear();

    // 空缓冲：0 行
    if (!base || size == 0)
        return;

    // 扫描换行符；每个换行符结束一行并开始新行
    uint64_t lineBreaks = 0;
    uint64_t pos = 0;

    // 第一帧固定为第 0 行 @ 偏移 0
    m_keyFrames.push_back({ 0, 0 });

    while (pos < size)
    {
        unsigned char c = base[pos];
        if (c == '\n' || c == '\r')
        {
            ++lineBreaks;
            pos = SkipLineBreak(base, size, pos);   // 跨过 \n / \r\n / \r
            if (lineBreaks % kFrameInterval == 0)
                m_keyFrames.push_back({ lineBreaks, pos });
        }
        else
        {
            ++pos;
        }
    }

    // 行数：换行符数 + (末尾非空且不以换行结尾 ? 1 : 0)
    bool endsWithBreak = (size > 0) &&
        (base[size - 1] == '\n' || base[size - 1] == '\r');
    m_lineCount = lineBreaks + (endsWithBreak ? 0 : 1);
}

uint64_t CLineIndex::SkipLineBreak(const unsigned char* base, uint64_t size, uint64_t pos)
{
    if (pos >= size)
        return pos;
    if (base[pos] == '\r' && pos + 1 < size && base[pos + 1] == '\n')
        return pos + 2;
    return pos + 1;
}

uint64_t CLineIndex::GetLineCount() const
{
    return m_lineCount;
}

uint64_t CLineIndex::GetLineStart(uint64_t row) const
{
    if (row == 0)
        return 0;
    if (row >= m_lineCount)
        return m_size;

    uint64_t frameIndex = row / kFrameInterval;
    if (frameIndex >= m_keyFrames.size())
        frameIndex = m_keyFrames.size() - 1;

    const KeyFrame& kf = m_keyFrames[frameIndex];
    uint64_t pos = kf.byteOffset;
    uint64_t currentRow = kf.row;

    // 从关键帧逐行推进到目标行
    while (currentRow < row && pos < m_size)
    {
        // 找到本行末尾换行符
        while (pos < m_size && m_base[pos] != '\n' && m_base[pos] != '\r')
            ++pos;
        if (pos >= m_size)
            break;
        pos = SkipLineBreak(m_base, m_size, pos);
        ++currentRow;
    }
    return pos;
}

uint64_t CLineIndex::GetLineLength(uint64_t row) const
{
    if (row >= m_lineCount)
        return 0;
    uint64_t start = GetLineStart(row);
    uint64_t end = (row + 1 < m_lineCount) ? GetLineStart(row + 1) : m_size;
    return end - start;
}

uint64_t CLineIndex::ByteOffsetToRow(uint64_t offset) const
{
    if (m_lineCount == 0)
        return 0;
    if (offset >= m_size)
        return m_lineCount - 1;

    // 二分定位不超过 offset 的最近关键帧
    uint64_t lo = 0, hi = m_keyFrames.size() - 1, best = 0;
    while (lo <= hi)
    {
        uint64_t mid = (lo + hi) / 2;
        if (m_keyFrames[mid].byteOffset <= offset)
        {
            best = mid;
            lo = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }

    const KeyFrame& kf = m_keyFrames[best];
    uint64_t pos = kf.byteOffset;
    uint64_t row = kf.row;

    // 局部扫描：跨过的换行符数即行号增量
    while (pos < offset)
    {
        while (pos < offset && m_base[pos] != '\n' && m_base[pos] != '\r')
            ++pos;
        if (pos >= offset)
            break;
        uint64_t next = SkipLineBreak(m_base, m_size, pos);
        if (next > offset)
            break;   // offset 落在换行符序列内部，仍属当前行
        pos = next;
        ++row;
    }
    return row;
}