#include <windows.h>
#include <winrt/base.h>

#include "core/log.h"
#include "ui/dock.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // One dock at a time.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DockX.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    int code = 0;
    {
        dx::Dock dock;
        if (!dock.Create(instance)) {
            dx::Log(L"DockX failed to start");
            MessageBoxW(nullptr, L"DockX ne može da se pokrene.", L"DockX", MB_ICONERROR);
            code = 1;
        } else {
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return code;
}
