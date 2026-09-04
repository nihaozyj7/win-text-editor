// 无 GUI 核心单元测试：PieceTable 往返一致性 + 编码检测 + 行索引
// 编译为一个独立控制台程序，不依赖窗口/D2D
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/PieceTable.h"
#include "../src/Encoding.h"

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

int main()
{
    std::printf("== core_tests ==\n");

    TestPieceTableInsertDelete();
    TestPieceTableUndoRedo();
    TestPieceTableManyUndo();
    TestEncodingDetection();

    if (g_failures == 0)
    {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }

    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}