// 程序入口：初始化 COM/D2D、创建主窗口、运行消息循环
#include <windows.h>
#include <commctrl.h>
#include "CEditorWindow.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    // 高分屏/系统缩放：Per-Monitor V2（Win10 1703+），每块显示器独立 DPI，
    // 窗口收到 WM_DPICHANGED 实时跟随；老系统回退系统级感知
    {
        using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        auto setCtx = u32
            ? reinterpret_cast<SetCtxFn>(GetProcAddress(u32, "SetProcessDpiAwarenessContext"))
            : nullptr;
        if (setCtx)
        {
            if (!setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
                setCtx(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
        }
        else
        {
            // Vista+ 兜底：至少保证不模糊
            using SetAwareFn = BOOL(WINAPI*)();
            auto setAware = reinterpret_cast<SetAwareFn>(
                GetProcAddress(u32, "SetProcessDPIAware"));
            if (setAware)
                setAware();
        }
    }

    // 初始化公共控件（状态栏需要）
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    // COM 以 STA 初始化（DWrite/D2D 需要）
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
        return 1;

    int exitCode = 0;

    // 命令行第一参数作为要打开的文件路径
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    LPCWSTR fileArg = (argv && argc > 1) ? argv[1] : nullptr;

    // 该文件已被其他实例打开 → 激活已有窗口并直接退出，不重复打开。
    // 必须在创建窗口前判定，避免新空窗口闪现
    if (fileArg && CEditorWindow::ActivateExistingForFile(fileArg))
    {
        if (argv)
            LocalFree(argv);
        CoUninitialize();
        return 0;
    }

    {
        CEditorWindow window;
        if (window.Create(hInstance, nCmdShow))
        {
            if (fileArg)
                window.OpenFile(fileArg);
            exitCode = window.Run();
        }
        else
        {
            exitCode = 1;
        }
    }

    if (argv)
        LocalFree(argv);
    CoUninitialize();
    return exitCode;
}