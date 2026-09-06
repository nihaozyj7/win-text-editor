#include "CLineIndex.h"
#include <cstring>
#include <algorithm>

// 在块内 [p, p+avail) 找 0x0A/0x0D 中较早出现者的指针；找不到返回 nullptr
// （单字节编码用，memchr 走 libc 的 SIMD 实现）
static const unsigned char* FindBreakInBlock(const unsigned char* p, uint64_t avail)
{
    const unsigned char* hLF = static_cast<const unsigned char*>(memchr(p, 0x0A, avail));
    const unsigned char* hCR = static_cast<const unsigned char*>(memchr(p, 0x0D, avail));
    if (hLF && hCR)
        return (hLF < hCR) ? hLF : hCR;
    return hLF ? hLF : hCR;
}

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

    // 扫描 code units，统计行；每 kFrameInterval 行记录一个关键帧。
    // 块内批量扫描（memchr/紧凑循环）替代逐 unit ReadUnit——
    // 逐字节函数调用是大文件构建的主要耗时
    uint64_t breaks = 0;
    uint64_t pos = 0;
    m_keyFrames.push_back({ 0, 0 });

    while (pos < m_scan.size)
    {
        if (pos < m_scan.blockStart || pos >= m_scan.blockStart + m_scan.blockLen)
            m_scan.Refill(pos);
        uint64_t rel = pos - m_scan.blockStart;
        if (rel >= m_scan.blockLen)
            break;   // reader 提前截断（防御）

        if (m_scan.unitBytes == 1)
        {
            // ANSI/UTF-8：块内 memchr 找 0x0A/0x0D 中较早出现者
            const unsigned char* hit = FindBreakInBlock(m_scan.block + rel,
                                                        m_scan.blockLen - rel);
            if (!hit)
            {
                pos = m_scan.blockStart + m_scan.blockLen;   // 本块无换行，跳到下一块
                continue;
            }
            ++breaks;
            pos = m_scan.SkipBreak(m_scan.blockStart + (hit - m_scan.block));
        }
        else
        {
            // UTF-16：块内按 code unit 紧凑扫描
            uint64_t nUnits = (m_scan.blockLen - rel) / 2;
            const unsigned char* q = m_scan.block + rel;
            uint64_t i = 0;
            for (; i < nUnits; ++i, q += 2)
            {
                uint32_t u = m_scan.utf16Swap ? ((q[0] << 8) | q[1])
                                              : (q[0] | (q[1] << 8));
                if (u == 0x0A || u == 0x0D)
                    break;
            }
            if (i < nUnits)
            {
                ++breaks;
                pos = m_scan.SkipBreak(m_scan.blockStart + rel + i * 2);
            }
            else if (nUnits == 0)
            {
                break;   // 剩余不足一个完整 unit（防御）
            }
            else
            {
                pos = m_scan.blockStart + rel + nUnits * 2;   // 本块无换行
                continue;
            }
        }

        if (breaks % kFrameInterval == 0)
            m_keyFrames.push_back({ breaks, pos });
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
    // 插入（无删除区间）
    NotifyEditRange(ofs, ofs, deltaBytes, deltaLines);
}

void CLineIndex::NotifyEditRange(uint64_t ofs, uint64_t oldEndOfs, int64_t deltaBytes,
                                 int64_t deltaLines)
{
    if (!m_scan.reader)
        return;
    ++m_editEpoch;

    // 关键：文档字节数本身发生了变化，必须同步
    m_scan.size = static_cast<uint64_t>(static_cast<int64_t>(m_scan.size) + deltaBytes);
    // 失效块缓存，避免读到编辑前的陈旧字节
    m_scan.blockStart = 0;
    m_scan.blockLen = 0;

    if (deltaLines == 0 && deltaBytes != 0 && oldEndOfs == ofs)
    {
        // 行结构不变：编辑点之后所有行的起始偏移整体平移 deltaBytes
        // 编辑点所在行行首 (=ofs，光标在行首) 不变；严格大于 ofs 的行才平移
        for (auto& kf : m_keyFrames)
        {
            if (kf.byteOffset > ofs)
            {
                kf.byteOffset = static_cast<uint64_t>(
                    static_cast<int64_t>(kf.byteOffset) + deltaBytes);
            }
        }
        return;
    }

    if (m_scan.size == 0)
    {
        // 文档被清空：退化为 1 个空行（与 BuildAll 的空文档约定一致）
        RebuildAll();
        return;
    }

    if (deltaLines != 0)
    {
        // 行结构变化：编辑点是一个"字节/行序一致"的切分点——
        //  1) 位于删除区间内部 (ofs, oldEndOfs) 的关键帧：其行首已被删除，移除
        //  2) 起点 >= 平移阈值（删除时为 oldEndOfs，纯插入时为 ofs）的关键帧：
        //     行号 += deltaLines、偏移 += deltaBytes
        //  3) 其余关键帧：不受影响（ofs 处的行首在删除时保持原位）
        const uint64_t shiftFrom = (oldEndOfs > ofs) ? oldEndOfs : ofs;
        std::vector<KeyFrame> adjusted;
        adjusted.reserve(m_keyFrames.size());
        for (auto& kf : m_keyFrames)
        {
            if (kf.byteOffset > ofs && kf.byteOffset < oldEndOfs)
                continue;   // 行首落在删除区间内 → 该行已不存在
            if (kf.byteOffset >= shiftFrom)
            {
                kf.row = static_cast<uint64_t>(
                    static_cast<int64_t>(kf.row) + deltaLines);
                kf.byteOffset = static_cast<uint64_t>(
                    static_cast<int64_t>(kf.byteOffset) + deltaBytes);
            }
            adjusted.push_back(kf);
        }
        m_keyFrames.swap(adjusted);
        m_lineCount = static_cast<uint64_t>(
            static_cast<int64_t>(m_lineCount) + deltaLines);
        if (m_lineCount == 0)
            m_lineCount = 1;   // 防御：文档非空至少 1 行
        return;
    }

    // deltaLines == 0 且有删除区间（行内删除）：编辑点之后的行整体平移 deltaBytes
    for (auto& kf : m_keyFrames)
    {
        if (kf.byteOffset >= oldEndOfs)
        {
            kf.byteOffset = static_cast<uint64_t>(
                static_cast<int64_t>(kf.byteOffset) + deltaBytes);
        }
        else if (kf.byteOffset > ofs)
        {
            // 行首落在删除区间内 → 该行已不存在（行结构未变时不应发生，防御性移除）
            // 保守起见交由全量重建兜底
            RebuildAll();
            return;
        }
    }
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

CLineIndex::LineCursor CLineIndex::CursorAt(uint64_t row) const
{
    LineCursor c;
    c.start = GetLineStart(row);
    return c;
}

bool CLineIndex::CursorNext(LineCursor& c, uint64_t* start, uint64_t* end) const
{
    if (c.start >= m_scan.size)
        return false;
    uint64_t brk = m_scan.FindBreak(c.start);
    if (start)
        *start = c.start;
    if (end)
        *end = brk;
    c.start = (brk < m_scan.size) ? m_scan.SkipBreak(brk) : m_scan.size;
    return true;
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

void CLineIndex::ScanState::Refill(uint64_t byteOfs) const
{
    // 对齐到块大小重新填充缓存块
    blockStart = byteOfs & ~(kBlockSize - 1);
    uint64_t want = kBlockSize;
    if (blockStart + want > size)
        want = size - blockStart;
    blockLen = reader(blockStart, block, want);
}

int64_t CLineIndex::ScanState::ReadUnit(uint64_t byteOfs) const
{
    if (byteOfs >= size)
        return -1;
    if (unitBytes == 1)
    {
        // 块缓存读取
        if (byteOfs < blockStart || byteOfs >= blockStart + blockLen)
            Refill(byteOfs);
        if (byteOfs - blockStart >= blockLen)
            return -1;   // reader 提前截断（防御）
        return block[byteOfs - blockStart];
    }
    else
    {
        if (byteOfs + 1 >= size)
            return -1;
        if (byteOfs + 1 < blockStart || byteOfs >= blockStart + blockLen)
            Refill(byteOfs);
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
        if (byteOfs < blockStart || byteOfs >= blockStart + blockLen)
            Refill(byteOfs);
        uint64_t rel = byteOfs - blockStart;
        if (rel >= blockLen)
            break;   // reader 提前截断（防御）

        if (unitBytes == 1)
        {
            // 块内 memchr 批量找换行（SIMD 加速），替代逐字节 ReadUnit
            const unsigned char* hit = FindBreakInBlock(block + rel, blockLen - rel);
            if (!hit)
            {
                byteOfs = blockStart + blockLen;   // 本块无换行，跳到下一块
                continue;
            }
            return blockStart + (hit - block);
        }
        else
        {
            // UTF-16：块内按 code unit（2 字节）紧凑扫描
            uint64_t nUnits = (blockLen - rel) / 2;
            const unsigned char* q = block + rel;
            for (uint64_t i = 0; i < nUnits; ++i, q += 2)
            {
                uint32_t u = utf16Swap ? ((q[0] << 8) | q[1]) : (q[0] | (q[1] << 8));
                if (u == 0x0A || u == 0x0D)
                    return blockStart + rel + i * 2;
            }
            if (nUnits == 0)
                break;   // 剩余不足一个完整 unit（truncated 尾字节不可能构成换行）
            byteOfs = blockStart + rel + nUnits * 2;   // 本块无换行
        }
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