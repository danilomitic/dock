#include <windows.h>
#include <unknwn.h>  // before C++/WinRT, so it can implement classic COM interfaces
#include <shellapi.h>
#include <winrt/base.h>

#include <string>

#include "core/log.h"
#include "shell/screen.h"
#include "ui/dock.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // "DockX.exe --watchdog <pid> <hwnd>": restore the taskbar if the dock dies.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc == 4 && std::wstring(argv[1]) == L"--watchdog") {
        const auto pid = static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));
        const auto hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(std::wcstoull(argv[3], nullptr, 10)));
        LocalFree(argv);
        return dx::RunWatchdog(pid, hwnd);
    }
    if (argv) LocalFree(argv);

    // One dock at a time.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DockX.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    const bool ole = SUCCEEDED(OleInitialize(nullptr));  // drag & drop of files
    int code = 0;
    {
        dx::Dock dock;
        if (!dock.Create(instance)) {
            dx::Log(L"DockX failed to start");
            MessageBoxW(nullptr, L"DockX could not start.", L"DockX", MB_ICONERROR);
            code = 1;
        } else {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    if (ole) OleUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return code;
}
