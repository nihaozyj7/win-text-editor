#include "PieceTable.h"
#include <algorithm>
#include <cstring>
#include <utility>

PieceTable::PieceTable()
    : m_size(0)
    , m_originalBase(nullptr)
    , m_originalSize(0)
{
}

void PieceTable::SetOriginal(const unsigned char* base, uint64_t size)
{
    m_originalBase = base;
    m_originalSize = size;
    m_pieces.clear();
    m_addBuf.clear();
    m_undoStack.clear();
    m_redoStack.clear();

    if (m_originalSize > 0)
        m_pieces.push_back({ Piece::Src::Original, 0, static_cast<uint32_t>(m_originalSize) });

    m_size = m_originalSize;
}

void PieceTable::Insert(uint64_t ofs, const unsigned char* data, uint32_t len, bool record)
{
    if (len == 0)
        return;

    if (record)
    {
        EditCommand cmd;
        cmd.isInsert = true;
        cmd.ofs = ofs;
        cmd.data.assign(data, data + len);
        m_undoStack.push_back(std::move(cmd));
        m_redoStack.clear();
        if (m_undoStack.size() > kMaxUndo)
            m_undoStack.erase(m_undoStack.begin());
    }

    // 追加到 addBuf，记录偏移
    uint64_t addOff = m_addBuf.size();
    m_addBuf.insert(m_addBuf.end(), data, data + len);

    auto it = SplitAt(ofs);
    m_pieces.insert(it, { Piece::Src::Added, addOff, len });
    m_size += len;
}

void PieceTable::Erase(uint64_t ofs, uint32_t len, bool record)
{
    if (len == 0)
        return;

    if (record)
    {
        EditCommand cmd;
        cmd.isInsert = false;
        cmd.ofs = ofs;

        // 记录被删内容（供撤销恢复）
        uint64_t remain = len;
        auto it = SplitAt(ofs);
        while (remain > 0 && it != m_pieces.end())
        {
            uint64_t take = (it->len < remain) ? it->len : remain;
            const unsigned char* src = (it->src == Piece::Src::Original)
                ? m_originalBase + it->off
                : m_addBuf.data() + it->off;
            cmd.data.insert(cmd.data.end(), src, src + take);
            it->off += take;
            it->len -= static_cast<uint32_t>(take);
            remain -= take;
            if (it->len == 0)
                it = m_pieces.erase(it);
        }

        m_undoStack.push_back(std::move(cmd));
        m_redoStack.clear();
        if (m_undoStack.size() > kMaxUndo)
            m_undoStack.erase(m_undoStack.begin());

        m_size -= (len - remain);
        return;
    }

    // 非记录模式：直接删除，不采集内容
    uint64_t remain = len;
    auto it = SplitAt(ofs);
    while (remain > 0 && it != m_pieces.end())
    {
        uint64_t take = (it->len < remain) ? it->len : remain;
        it->off += take;
        it->len -= static_cast<uint32_t>(take);
        remain -= take;
        if (it->len == 0)
            it = m_pieces.erase(it);
    }
    m_size -= (len - remain);
}

bool PieceTable::Undo()
{
    if (m_undoStack.empty())
        return false;

    EditCommand cmd = std::move(m_undoStack.back());
    m_undoStack.pop_back();

    if (cmd.isInsert)
        Erase(cmd.ofs, static_cast<uint32_t>(cmd.data.size()), false);
    else
        Insert(cmd.ofs, cmd.data.data(), static_cast<uint32_t>(cmd.data.size()), false);

    m_redoStack.push_back(std::move(cmd));
    return true;
}

bool PieceTable::Redo()
{
    if (m_redoStack.empty())
        return false;

    EditCommand cmd = std::move(m_redoStack.back());
    m_redoStack.pop_back();

    if (cmd.isInsert)
        Insert(cmd.ofs, cmd.data.data(), static_cast<uint32_t>(cmd.data.size()), false);
    else
        Erase(cmd.ofs, static_cast<uint32_t>(cmd.data.size()), false);

    m_undoStack.push_back(std::move(cmd));
    return true;
}

uint64_t PieceTable::Size() const
{
    return m_size;
}

std::list<PieceTable::Piece>::iterator PieceTable::SplitAt(uint64_t ofs)
{
    uint64_t acc = 0;
    for (auto it = m_pieces.begin(); it != m_pieces.end(); ++it)
    {
        uint64_t end = acc + it->len;
        if (ofs == acc)
            return it;   // 恰在片段开头
        if (ofs < end)
        {
            // 落在片段内部，拆成两段
            uint64_t leftLen = ofs - acc;
            uint64_t rightLen = it->len - leftLen;
            Piece left{ it->src, it->off, static_cast<uint32_t>(leftLen) };
            Piece right{ it->src, it->off + leftLen, static_cast<uint32_t>(rightLen) };
            *it = left;
            auto next = it;
            ++next;
            return m_pieces.insert(next, right);
        }
        acc = end;
    }
    return m_pieces.end();  // ofs == 总长
}

unsigned char PieceTable::At(uint64_t ofs) const
{
    uint64_t acc = 0;
    for (const auto& p : m_pieces)
    {
        if (ofs < acc + p.len)
        {
            uint64_t rel = ofs - acc;
            return (p.src == Piece::Src::Original)
                ? m_originalBase[p.off + rel]
                : m_addBuf[p.off + rel];
        }
        acc += p.len;
    }
    return 0;
}

uint64_t PieceTable::ReadRange(uint64_t ofs, unsigned char* dst, uint64_t len) const
{
    if (len == 0 || ofs >= m_size)
        return 0;

    // 定位到包含 ofs 的片段
    uint64_t acc = 0;
    auto it = m_pieces.begin();
    while (it != m_pieces.end() && ofs >= acc + it->len)
    {
        acc += it->len;
        ++it;
    }

    uint64_t copied = 0;
    uint64_t remain = len;
    while (it != m_pieces.end() && remain > 0)
    {
        uint64_t rel = (ofs >= acc) ? (ofs - acc) : 0;
        uint64_t take = (it->len - rel < remain) ? (it->len - rel) : remain;
        if (take == 0)
            break;
        const unsigned char* src = (it->src == Piece::Src::Original)
            ? m_originalBase + it->off + rel
            : m_addBuf.data() + it->off + rel;
        std::memcpy(dst + copied, src, take);
        copied += take;
        remain -= take;
        acc += it->len;
        ++it;
    }
    return copied;
}

uint64_t PieceTable::CopyOut(unsigned char* dst, uint64_t maxLen) const
{
    uint64_t copied = 0;
    for (const auto& p : m_pieces)
    {
        if (copied >= maxLen)
            break;
        uint64_t take = (p.len < maxLen - copied) ? p.len : (maxLen - copied);
        const unsigned char* src = (p.src == Piece::Src::Original)
            ? m_originalBase + p.off
            : m_addBuf.data() + p.off;
        std::memcpy(dst + copied, src, take);
        copied += take;
    }
    return copied;
}