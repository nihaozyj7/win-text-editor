// 无 GUI 核心单元测试：PieceTable 往返一致性 + 编码检测 + 行索引
// 编译为一个独立控制台程序，不依赖窗口/D2D
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/PieceTable.h"
#include "../src/Encoding.h"
#include "../src/CLineIndex.h"
#include "../src/Highlighter.h"

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static std::string Dump(PieceTable& pt)
{
    std::vector<unsigned char> buf(pt.Size());
    pt.CopyOut(buf.data(), buf.size());
    return std::string(reinterpret_cast<char*>(buf.data()), buf.size());
}

static void TestPieceTableInsertDelete()
{
    const char* original = "Hello World";
    PieceTable pt;
    pt.SetOriginal(reinterpret_cast<const unsigned char*>(original), 11);

    // 初始内容
    CHECK(Dump(pt) == "Hello World");

    // 中部插入
    const char* ins = ", nice";
    pt.Insert(5, reinterpret_cast<const unsigned char*>(ins), 6);
    CHECK(Dump(pt) == "Hello, nice World");

    // 删除
    pt.Erase(5, 6);
    CHECK(Dump(pt) == "Hello World");

    // 头部插入
    const char* hi = ">> ";
    pt.Insert(0, reinterpret_cast<const unsigned char*>(hi), 3);
    CHECK(Dump(pt) == ">> Hello World");

    // 尾部插入
    const char* tail = " <<";
    pt.Insert(pt.Size(), reinterpret_cast<const unsigned char*>(tail), 3);
    CHECK(Dump(pt) == ">> Hello World <<");
}

static void TestPieceTableUndoRedo()
{
    const char* original = "abc";
    PieceTable pt;
    pt.SetOriginal(reinterpret_cast<const unsigned char*>(original), 3);

    const char* x = "X";
    const char* y = "Y";

    pt.Insert(1, reinterpret_cast<const unsigned char*>(x), 1);  // aXbc
    pt.Insert(3, reinterpret_cast<const unsigned char*>(y), 1);  // aXbYc

    CHECK(Dump(pt) == "aXbYc");

    CHECK(pt.Undo());   // aXbc
    CHECK(Dump(pt) == "aXbc");

    CHECK(pt.Undo());   // abc
    CHECK(Dump(pt) == "abc");

    CHECK(pt.Redo());   // aXbc
    CHECK(Dump(pt) == "aXbc");

    CHECK(pt.Redo());   // aXbYc
    CHECK(Dump(pt) == "aXbYc");

    // 删除后撤销恢复
    pt.Erase(1, 2, true);  // 删除 "Xb"，剩 aYc
    CHECK(Dump(pt) == "aYc");
    CHECK(pt.Undo());
    CHECK(Dump(pt) == "aXbYc");
}

static void TestPieceTableManyUndo()
{
    const char* original = "";
    PieceTable pt;
    pt.SetOriginal(reinterpret_cast<const unsigned char*>(original), 0);

    // 连续插入 100 个字符，逐步撤销
    for (char c = 'a'; c <= 'h'; ++c)
    {
        const unsigned char ch = static_cast<unsigned char>(c);
        pt.Insert(pt.Size(), &ch, 1);
    }
    CHECK(Dump(pt) == "abcdefgh");

    for (int i = 0; i < 8; ++i)
        CHECK(pt.Undo());
    CHECK(Dump(pt) == "");

    for (int i = 0; i < 8; ++i)
        CHECK(pt.Redo());
    CHECK(Dump(pt) == "abcdefgh");
}

static void TestEncodingDetection()
{
    // UTF-8 BOM
    {
        const BYTE b[] = { 0xEF, 0xBB, 0xBF, 0x48, 0x65 };
        CHECK(DetectEncoding(b, sizeof(b)) == Encoding::Utf8);
    }
    // UTF-16 LE BOM
    {
        const BYTE b[] = { 0xFF, 0xFE, 0x48, 0x00 };
        CHECK(DetectEncoding(b, sizeof(b)) == Encoding::Utf16LE);
    }
    // UTF-16 BE BOM
    {
        const BYTE b[] = { 0xFE, 0xFF, 0x00, 0x48 };
        CHECK(DetectEncoding(b, sizeof(b)) == Encoding::Utf16BE);
    }
    // 纯 ASCII 无 BOM → 判为 UTF-8
    {
        const BYTE b[] = { 'H', 'e', 'l', 'l', 'o' };
        CHECK(DetectEncoding(b, sizeof(b)) == Encoding::Utf8);
    }
    // 合法 UTF-8（中文"你好"）
    {
        const BYTE b[] = { 0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD };
        CHECK(DetectEncoding(b, sizeof(b)) == Encoding::Utf8);
    }
    // 非法 UTF-8 序列（GBK 高字节）→ 回退 ANSI
    {
        const BYTE b[] = { 0xC4, 0xE3, 0xBA, 0xC3, 0xFF };
        CHECK(DetectEncoding(b, sizeof(b)) == Encoding::Ansi);
    }
}

static void TestLineIndex()
{
    // 文本："abc\n""def\r\n""ghi\r""jkl"（结尾无换行）
    const char* text = "abc\ndef\r\nghi\rjkl";
    CLineIndex idx;
    idx.Build(reinterpret_cast<const unsigned char*>(text), std::strlen(text));

    CHECK(idx.GetLineCount() == 4);

    // 每行起始"字节"偏移
    CHECK(idx.GetLineStart(0) == 0);   // "abc\n"
    CHECK(idx.GetLineStart(1) == 4);   // "def\r\n"
    CHECK(idx.GetLineStart(2) == 9);   // "ghi\r"
    CHECK(idx.GetLineStart(3) == 13);  // "jkl"

    // 字节偏移 → 行号
    CHECK(idx.ByteOffsetToRow(0) == 0);    // 'a'
    CHECK(idx.ByteOffsetToRow(4) == 1);    // 'd'
    CHECK(idx.ByteOffsetToRow(9) == 2);    // 'g'
    CHECK(idx.ByteOffsetToRow(13) == 3);   // 'j'
}

// 行索引基于 PieceTable 逻辑字节源构建（编辑后使用路径）
static void TestLineIndexOnPieceTable()
{
    const char* text = "line1\nline2\nline3\nline4";
    PieceTable pt;
    pt.SetOriginal(reinterpret_cast<const unsigned char*>(text), std::strlen(text));

    CLineIndex idx;
    idx.Build(
        [&pt](uint64_t ofs, unsigned char* dst, uint64_t maxLen) -> uint64_t {
            return pt.ReadRange(ofs, dst, maxLen);
        },
        pt.Size(), Encoding::Utf8);

    CHECK(idx.GetLineCount() == 4);
    CHECK(idx.GetLineStart(2) == 12);   // "line3"

    // 在 line2 末尾插入换行 → line3 前多一行
    const char* nl = "\n";
    pt.Insert(11, reinterpret_cast<const unsigned char*>(nl), 1);
    idx.Build(
        [&pt](uint64_t ofs, unsigned char* dst, uint64_t maxLen) -> uint64_t {
            return pt.ReadRange(ofs, dst, maxLen);
        },
        pt.Size(), Encoding::Utf8);

    CHECK(pt.Size() == std::strlen(text) + 1);
    CHECK(idx.GetLineCount() == 5);
    CHECK(idx.GetLineStart(3) == 13);   // 原 line3 前多出空行，行首后移 1 字节
    CHECK(idx.GetLineStart(2) == 12);   // 空行
}

// UTF-16 LE 换行扫描（BOM 后为 code units）
static void TestLineIndexUtf16()
{
    // "a\nb\r\nc" 以 UTF-16LE 表示
    const unsigned char bytes[] = {
        'a', 0, '\n', 0,     // a\n
        'b', 0, '\r', 0, '\n', 0,  // b\r\n
        'c', 0
    };
    CLineIndex idx;
    idx.Build(bytes, sizeof(bytes), Encoding::Utf16LE);

    CHECK(idx.GetLineCount() == 3);
    CHECK(idx.GetLineStart(1) == 4);
    CHECK(idx.GetLineStart(2) == 10);
    CHECK(idx.ByteOffsetToRow(3) == 0);   // 'a' 行尾 \n 内部
    CHECK(idx.ByteOffsetToRow(4) == 1);   // 'b'
    CHECK(idx.ByteOffsetToRow(10) == 2);  // 'c'
}

// 编辑增量：验证 NotifyEdit 之后文档尺寸同步 + 关键帧平移正确
static void TestLineIndexIncrementalEdit()
{
    const char* text = "aa\nbb\ncc";
    PieceTable pt;
    pt.SetOriginal(reinterpret_cast<const unsigned char*>(text), std::strlen(text));

    CLineIndex idx;
    idx.Build(
        [&pt](uint64_t ofs, unsigned char* dst, uint64_t maxLen) -> uint64_t {
            return pt.ReadRange(ofs, dst, maxLen);
        },
        pt.Size(), Encoding::Utf8);

    CHECK(idx.GetLineCount() == 3);
    CHECK(idx.GetLineStart(0) == 0);
    CHECK(idx.GetLineStart(1) == 3);   // "aa\n"
    CHECK(idx.GetLineStart(2) == 6);   // "bb\n"

    // 在第 1 行中间（"aa" 的第二个 a 之后，字节偏移 2）插入 "XY"（无换行）
    const char* ins = "XY";
    pt.Insert(2, reinterpret_cast<const unsigned char*>(ins), 2);
    idx.NotifyEdit(2, 2, 0);

    // 尺寸更新
    CHECK(pt.Size() == std::strlen(text) + 2);
    // 行数不变
    CHECK(idx.GetLineCount() == 3);
    // 第 0 行行首不变；后续行首整体 +2
    CHECK(idx.GetLineStart(0) == 0);
    CHECK(idx.GetLineStart(1) == 5);   // 3 + 2
    CHECK(idx.GetLineStart(2) == 8);   // 6 + 2
    // 偏移→行仍正确
    CHECK(idx.ByteOffsetToRow(0) == 0);
    CHECK(idx.ByteOffsetToRow(3) == 0);   // 落于第 0 行（插入后 "aaXY\n"）
    CHECK(idx.ByteOffsetToRow(6) == 1);
    CHECK(idx.ByteOffsetToRow(9) == 2);

    // 行首插入：光标在行首（ofs == 某行行首）时该行行首不应平移
    PieceTable pt2;
    pt2.SetOriginal(reinterpret_cast<const unsigned char*>(text), std::strlen(text));
    CLineIndex idx2;
    idx2.Build(
        [&pt2](uint64_t ofs, unsigned char* dst, uint64_t maxLen) -> uint64_t {
            return pt2.ReadRange(ofs, dst, maxLen);
        },
        pt2.Size(), Encoding::Utf8);

    const char* ins2 = "Z";
    pt2.Insert(3, reinterpret_cast<const unsigned char*>(ins2), 1);  // 在行首(第1行)插入
    idx2.NotifyEdit(3, 1, 0);

    CHECK(idx2.GetLineStart(1) == 3);   // 第 1 行行首不变（新内容成为该行开头）
    CHECK(idx2.GetLineStart(2) == 7);   // 6 + 1
}

// 行结构变化的增量更新：NotifyEditRange / NotifyEdit(deltaLines≠0) 必须与
// 全量 Build 的结果一致（大文件编辑卡顿修复的关键路径）
static void TestLineIndexIncrementalLines()
{
    // 对照函数：对 PieceTable 当前内容全量重建的期望值
    auto rebuild = [](PieceTable& pt, CLineIndex& idx) {
        idx.Build(
            [&pt](uint64_t ofs, unsigned char* dst, uint64_t maxLen) -> uint64_t {
                return pt.ReadRange(ofs, dst, maxLen);
            },
            pt.Size(), Encoding::Utf8);
    };

    // ---- 用例 1：行中插入换行（回车）----
    {
        const char* text = "alpha\nbeta\ngamma\ndelta";
        PieceTable pt;
        pt.SetOriginal(reinterpret_cast<const unsigned char*>(text), std::strlen(text));
        CLineIndex idx;
        rebuild(pt, idx);
        CHECK(idx.GetLineCount() == 4);

        const char* nl = "\r\n";
        pt.Insert(7, reinterpret_cast<const unsigned char*>(nl), 2);  // "be|ta" 行中回车
        idx.NotifyEditRange(7, 7, 2, 1);

        CLineIndex expect;
        rebuild(pt, expect);
        CHECK(idx.GetLineCount() == expect.GetLineCount());
        CHECK(idx.GetLineCount() == 5);
        for (uint64_t r = 0; r < idx.GetLineCount(); ++r)
        {
            CHECK(idx.GetLineStart(r) == expect.GetLineStart(r));
            CHECK(idx.GetLineLength(r) == expect.GetLineLength(r));
        }
        CHECK(idx.ByteOffsetToRow(0) == 0);
        CHECK(idx.ByteOffsetToRow(7) == 1);   // 插入的 \r 在新行 1
        CHECK(idx.ByteOffsetToRow(9) == 2);   // "ta" 在新行 2
        CHECK(idx.ByteOffsetToRow(11) == 2);
    }

    // ---- 用例 2：行首插入换行（关键帧恰好位于编辑点）----
    {
        const char* text = "aa\nbb\ncc\ndd";
        PieceTable pt;
        pt.SetOriginal(reinterpret_cast<const unsigned char*>(text), std::strlen(text));
        CLineIndex idx;
        rebuild(pt, idx);

        const char* nl = "\r\n";
        pt.Insert(3, reinterpret_cast<const unsigned char*>(nl), 2);  // 行首(第1行)回车
        idx.NotifyEditRange(3, 3, 2, 1);

        CLineIndex expect;
        rebuild(pt, expect);
        CHECK(idx.GetLineCount() == expect.GetLineCount());
        for (uint64_t r = 0; r < idx.GetLineCount(); ++r)
            CHECK(idx.GetLineStart(r) == expect.GetLineStart(r));
        CHECK(idx.ByteOffsetToRow(3) == 1);
        CHECK(idx.ByteOffsetToRow(5) == 2);
    }

    // ---- 用例 3：跨行删除（合并两行）----
    {
        const char* text = "aa\nbb\ncc\ndd\nee";
        PieceTable pt;
        pt.SetOriginal(reinterpret_cast<const unsigned char*>(text), std::strlen(text));
        CLineIndex idx;
        rebuild(pt, idx);
        CHECK(idx.GetLineCount() == 5);

        // 删除 [2, 5) = "\nbb" → "aa" 与 "cc" 合并
        pt.Erase(2, 3);
        idx.NotifyEditRange(2, 5, -3, -1);

        CLineIndex expect;
        rebuild(pt, expect);
        CHECK(idx.GetLineCount() == expect.GetLineCount());
        CHECK(idx.GetLineCount() == 4);
        for (uint64_t r = 0; r < idx.GetLineCount(); ++r)
        {
            CHECK(idx.GetLineStart(r) == expect.GetLineStart(r));
            CHECK(idx.GetLineLength(r) == expect.GetLineLength(r));
        }
    }

    // ---- 用例 4：连续多次编辑（关键帧漂移累积）----
    {
        const char* text = "1\n2\n3\n4\n5\n6\n7\n8";
        PieceTable pt;
        pt.SetOriginal(reinterpret_cast<const unsigned char*>(text), std::strlen(text));
        CLineIndex idx;
        rebuild(pt, idx);

        // 固定在 "1\n" 之后连续回车 50 次：每次净 +1 行，考验增量累积
        for (int i = 0; i < 50; ++i)
        {
            const char* nl = "\r\n";
            pt.Insert(2, reinterpret_cast<const unsigned char*>(nl), 2);
            idx.NotifyEditRange(2, 2, 2, 1);
        }
        CLineIndex expect;
        rebuild(pt, expect);
        CHECK(idx.GetLineCount() == expect.GetLineCount());
        for (uint64_t r = 0; r < idx.GetLineCount(); ++r)
        {
            CHECK(idx.GetLineStart(r) == expect.GetLineStart(r));
            CHECK(idx.GetLineLength(r) == expect.GetLineLength(r));
        }
    }
}

int main()
{
    std::printf("== core_tests ==\n");

    TestPieceTableInsertDelete();
    TestPieceTableUndoRedo();
    TestPieceTableManyUndo();
    TestEncodingDetection();
    TestLineIndex();
    TestLineIndexOnPieceTable();
    TestLineIndexUtf16();
    TestLineIndexIncrementalEdit();
    TestLineIndexIncrementalLines();

    if (g_failures == 0)
    {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }

    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}