#include "CLineIndex.h"
#include <cstring>
#include <algorithm>

CLineIndex::CLineIndex()
    : m_lineCount(0)
    , m_editEpoch(0)
{
    m_scan.reader = nullptr;
    m_scan.size = 0;
    m_scan.enc = Encoding::Utf8;
    m_scan.unitBytes = 1;
    m_scan.utf16Swap = false;
    m_scan.block = nullptr;
    m_scan.blockStart = 0;
    m_scan.blockLen = 0;
    m_blockStorage.resize(1 << 16);
    m_scan.block = m_blockStorage.data();
}

void CLineIndex::Build(const unsigned char* base, uint64_t size, Encoding enc)
{
    ByteReader r;
    if (base && size > 0)
        r = [base](uint64_t ofs, unsigned char* dst, uint64_t maxLen) -> uint64_t {
            return maxLen; // caller never requests beyond size; keep simple
        };
    // 上述 lambda 无实际意义，直接构建连续内存读取器：
    Build(
        [base](uint64_t ofs, unsigned char* dst, uint64_t maxLen) -> uint64_t {
            std::memcpy(dst, base + ofs, maxLen);
            return maxLen;
        },
        size, enc);
}

void CLineIndex::Build(ByteReader reader, uint64_t size, Encoding enc)
{
    m_scan.reader = reader;
    m_scan.size = size;
    m_scan.enc = enc;
    m_scan.unitBytes = (enc == Encoding::Utf16LE || enc == Encoding::Utf16BE) ? 2 : 1;
    m_scan.utf16Swap = (enc == Encoding::Utf16BE);
    m_scan.blockStart = 0;
    m_scan.blockLen = 0;
    m_editEpoch = 0;
    m_lineCount = 0;
    m_keyFrames.clear();
    BuildAll();
}

void CLineIndex::BuildAll()
{
    m_lineCount = 0;
    m_keyFrames.clear();
    if (!m_scan.reader)
        return;

    // 空文档视为 1 个空行（保证新建/清空后可编辑）
    if (m_scan.size == 0)
    {
        m_lineCount = 1;
        m_keyFrames.push_back({ 0, 0 });
        return;
    }

    // 扫描 code units，统计行；每 kFrameInterval 行记录一个关键帧
    uint64_t breaks = 0;
    uint64_t pos = 0;
    m_keyFrames.push_back({ 0, 0 });

    while (pos < m_scan.size)
    {
        int64_t unit = m_scan.ReadUnit(pos);
        if (unit < 0)
            break;
        if (unit == 0x0A || unit == 0x0D)
        {
            ++breaks;
            pos = m_scan.SkipBreak(pos);
            if (breaks % kFrameInterval == 0)
                m_keyFrames.push_back({ breaks, pos });
        }
        else
        {
            pos += m_scan.unitBytes;
        }
    }

    bool endsWithBreak = false;
    if (m_scan.size > 0)
    {
        int64_t lastUnit = m_scan.ReadUnit(m_scan.size - m_scan.unitBytes);
        endsWithBreak = (lastUnit == 0x0A || lastUnit == 0x0D);
        // 注意：\r\n 会读到 \n，因此以 \n 结尾即可；以 \r 结尾（罕见）也按换行处理
    }
    m_lineCount = breaks + (endsWithBreak ? 0 : 1);
}

void CLineIndex::NotifyEdit(uint64_t ofs, int64_t deltaBytes, int64_t deltaLines)
{
    if (!m_scan.reader)
        return;
    ++m_editEpoch;

    if (deltaLines == 0 && deltaBytes != 0)
    {
        // 行结构不变：编辑点之后所有行的起始偏移整体平移 deltaBytes
        // 关键帧行号保持对齐（无行增删），只需平移字节偏移
        for (auto& kf : m_keyFrames)
        {
            if (kf.byteOffset >= ofs)
            {
                // byteOffset 恰好 == ofs 的行首：编辑点在行首之后，该行本身也要平移
                kf.byteOffset = static_cast<uint64_t>(
                    static_cast<int64_t>(kf.byteOffset) + deltaBytes);
            }
        }
        return;
    }

    // 行结构变化：行号与偏移都受影响，直接全量重建（普通编辑场景可接受）
    RebuildAll();
}

void CLineIndex::RebuildAll()
{
    // 保留当前 reader/编码配置，重新全量扫描
    uint64_t size = m_scan.size;
    Encoding enc = m_scan.enc;
    ByteReader reader = m_scan.reader;
    m_lineCount = 0;
    m_keyFrames.clear();
    if (!reader)
        return;

    m_scan.size = size;
    m_scan.enc = enc;
    m_scan.unitBytes = (enc == Encoding::Utf16LE || enc == Encoding::Utf16BE) ? 2 : 1;
    m_scan.utf16Swap = (enc == Encoding::Utf16BE);
    m_scan.blockStart = 0;
    m_scan.blockLen = 0;

    BuildAll();
}

uint64_t CLineIndex::GetLineCount() const
{
    return m_lineCount;
}

bool CLineIndex::IsValid() const
{
    return m_scan.reader != nullptr;
}

size_t CLineIndex::FindFrame(uint64_t row) const
{
    // 关键帧行号单调递增，二分找 <= row 的最大帧
    size_t lo = 0, hi = m_keyFrames.size();
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (m_keyFrames[mid].row <= row)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo > 0 ? lo - 1 : 0;
}

uint64_t CLineIndex::GetLineStart(uint64_t row) const
{
    if (row == 0)
        return 0;
    if (row >= m_lineCount || m_keyFrames.empty())
        return m_scan.size;

    const KeyFrame& kf = m_keyFrames[FindFrame(row)];
    uint64_t pos = kf.byteOffset;
    uint64_t currentRow = kf.row;

    // 从关键帧逐行推进到目标行（找行尾 code unit 并跳过换行序列）
    while (currentRow < row && pos < m_scan.size)
    {
        uint64_t brk = m_scan.FindBreak(pos);
        if (brk >= m_scan.size)
            break;
        pos = m_scan.SkipBreak(brk);
        ++currentRow;
    }
    return pos;
}

uint64_t CLineIndex::GetLineLength(uint64_t row) const
{
    if (row >= m_lineCount)
        return 0;
    uint64_t start = GetLineStart(row);
    uint64_t end = (row + 1 < m_lineCount) ? GetLineStart(row + 1) : m_scan.size;
    return end - start;
}

uint64_t CLineIndex::ByteOffsetToRow(uint64_t offset) const
{
    if (m_lineCount == 0)
        return 0;
    if (offset >= m_scan.size)
        return m_lineCount - 1;
    if (m_keyFrames.empty())
        return 0;

    // 二分定位不超过 offset 的最近关键帧
    size_t lo = 0, hi = m_keyFrames.size(), best = 0;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (m_keyFrames[mid].byteOffset <= offset)
        {
            best = mid;
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }

    const KeyFrame& kf = m_keyFrames[best];
    uint64_t pos = kf.byteOffset;
    uint64_t row = kf.row;

    // 局部扫描：跨过的换行符数即行号增量
    while (pos < offset)
    {
        uint64_t brk = m_scan.FindBreak(pos);
        if (brk >= offset)
            break;   // offset 落在换行序列之前/内部
        pos = m_scan.SkipBreak(brk);
        if (pos > offset)
            break;
        ++row;
    }
    return row;
}

// ---------- ScanState 辅助 ----------

int64_t CLineIndex::ScanState::ReadUnit(uint64_t byteOfs) const
{
    if (byteOfs >= size)
        return -1;
    if (unitBytes == 1)
    {
        // 块缓存读取
        if (byteOfs < blockStart || byteOfs >= blockStart + blockLen)
        {
            // 重新填充缓存块（对齐到块大小）
            blockStart = byteOfs & ~(kBlockSize - 1);
            uint64_t want = kBlockSize;
            if (blockStart + want > size)
                want = size - blockStart;
            blockLen = reader(blockStart, block, want);
        }
        return block[byteOfs - blockStart];
    }
    else
    {
        if (byteOfs + 1 >= size)
            return -1;
        if (byteOfs + 1 < blockStart || byteOfs >= blockStart + blockLen)
        {
            blockStart = byteOfs & ~(kBlockSize - 1);
            uint64_t want = kBlockSize;
            if (blockStart + want > size)
                want = size - blockStart;
            blockLen = reader(blockStart, block, want);
        }
        uint64_t rel = byteOfs - blockStart;
        if (rel + 1 >= blockLen)
        {
            // 跨块边界，直接小块读取
            unsigned char tmp[2];
            uint64_t got = reader(byteOfs, tmp, 2);
            if (got < 2)
                return -1;
            return utf16Swap ? ((tmp[0] << 8) | tmp[1]) : (tmp[0] | (tmp[1] << 8));
        }
        unsigned char b0 = block[rel];
        unsigned char b1 = block[rel + 1];
        return utf16Swap ? ((b0 << 8) | b1) : (b0 | (b1 << 8));
    }
}

uint64_t CLineIndex::ScanState::FindBreak(uint64_t byteOfs) const
{
    while (byteOfs < size)
    {
        int64_t unit = ReadUnit(byteOfs);
        if (unit < 0)
            break;
        if (unit == 0x0A || unit == 0x0D)
            return byteOfs;
        byteOfs += unitBytes;
    }
    return size;
}

uint64_t CLineIndex::ScanState::SkipBreak(uint64_t breakByteOfs) const
{
    if (breakByteOfs >= size)
        return size;

    int64_t unit = ReadUnit(breakByteOfs);
    if (unit == 0x0D)
    {
        // 判断是否为 \r\n
        if (breakByteOfs + unitBytes < size)
        {
            int64_t next = ReadUnit(breakByteOfs + unitBytes);
            if (next == 0x0A)
                return breakByteOfs + 2 * unitBytes;
        }
        return breakByteOfs + unitBytes;
    }
    if (unit == 0x0A)
        return breakByteOfs + unitBytes;
    return breakByteOfs;  // 非换行，防御
}