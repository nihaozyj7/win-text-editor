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