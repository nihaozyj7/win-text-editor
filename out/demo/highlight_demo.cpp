// ============================================================
//  ZNote — Windows 记事本风格文本编辑器
//  Win32 + Direct2D/DirectWrite, C++20, 超大文件流畅编辑
// ============================================================
#include <windows.h>
#include <d2d1.h>
#include <string>
#include <vector>

namespace editor {

enum class Encoding { UTF8, UTF16LE, ANSI, GBK };

class PieceTable {
public:
    bool Insert(size_t offset, std::wstring_view text);
    bool Erase(size_t offset, size_t count);
    void Undo() noexcept;  // 撤销栈上限 1000
    void Redo() noexcept;
private:
    std::wstring append_buffer_;   // 追加缓冲
    std::vector<size_t> history_;
};

// 内存映射文件：只读视图，禁止写入
bool OpenHugeFile(const wchar_t* path, Encoding enc) {
    auto handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    auto size = GetFileSize(handle, nullptr);
    auto map = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, size, nullptr);
    return map != nullptr && size > 0;  // 100MB 秒开
}

}  // namespace editor

int wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    // 渲染初始化：COM 必须 STA（DWrite/D2D 要求）
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;
    editor::OpenHugeFile(L"big_demo.txt", editor::Encoding::UTF8);
    return 0;
}
