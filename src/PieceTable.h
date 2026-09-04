#pragma once
#include <cstdint>
#include <list>
#include <vector>

// Piece Table：只读原始缓冲 + 追加缓冲的片段链表编辑模型
// canonical 坐标 = 逻辑字节偏移；插入/删除 O(定位成本)，遍历 O(总字节)
// 撤销/重做以"命令"形式内建（记录操作类型、位置、字节内容）
class PieceTable
{
public:
    struct Piece
    {
        enum class Src : uint8_t { Original, Added };
        Src      src;
        uint64_t off;   // Original: 相对 MMF 基址；Added: 相对 addBuf
        uint32_t len;
    };

    // 撤销命令：Insert 记录插入内容；Delete 记录被删内容（供恢复）
    struct EditCommand
    {
        bool              isInsert;
        uint64_t          ofs;
        std::vector<unsigned char> data;
    };

    PieceTable();

    // 设置只读原始缓冲（MMF 基址），编辑前调用一次
    void SetOriginal(const unsigned char* base, uint64_t size);

    // 纯数据结构操作（record=false 表示内部操作，不进撤销栈）
    void Insert(uint64_t ofs, const unsigned char* data, uint32_t len, bool record = true);
    void Erase(uint64_t ofs, uint32_t len, bool record = true);

    // 撤销 / 重做
    bool Undo();
    bool Redo();

    // 最近一次 Undo/Redo 应用命令的字节偏移（供上层复位光标）
    uint64_t UndoOffset() const;
    uint64_t RedoOffset() const;

    // 逻辑总长度（字节）
    uint64_t Size() const;

    // 读取单个逻辑字节
    unsigned char At(uint64_t ofs) const;

    // 范围读取：从 ofs 起拷贝最多 len 字节到 dst（跨片段拼接）
    // 返回实际拷贝的字节数（文件尾不足 len 时少于 len）
    uint64_t ReadRange(uint64_t ofs, unsigned char* dst, uint64_t len) const;

    // 把整个逻辑内容拷贝到 dst（最多 maxLen 字节），返回实际拷贝数
    uint64_t CopyOut(unsigned char* dst, uint64_t maxLen) const;

    // 遍历期间直接访问某逻辑字节（渲染用）
    // 供调用方按片段高效读取：no copy

private:
    std::list<Piece>::iterator SplitAt(uint64_t ofs);
    uint64_t m_size;
    std::list<Piece> m_pieces;
    std::vector<unsigned char> m_addBuf;
    const unsigned char* m_originalBase;
    uint64_t m_originalSize;

    std::vector<EditCommand> m_undoStack;
    std::vector<EditCommand> m_redoStack;
    uint64_t m_lastUndoOfs;
    uint64_t m_lastRedoOfs;
    static constexpr size_t kMaxUndo = 1000;
};