// 程序入口：初始化 COM/D2D、创建主窗口、运行消息循环
#include <windows.h>
#include <commctrl.h>
#include "CEditorWindow.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
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
    {
        CEditorWindow window;
        if (window.Create(hInstance, nCmdShow))
        {
            // 命令行第一参数作为要打开的文件路径
            int argc = 0;
            LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argv)
            {
                if (argc > 1)
                    window.OpenFile(argv[1]);
                LocalFree(argv);
            }
            exitCode = window.Run();
        }
        else
        {
            exitCode = 1;
        }
    }

    CoUninitialize();
    return exitCode;
}