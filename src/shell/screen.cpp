#include "shell/screen.h"

#include <shellapi.h>

#include <string>

#include "core/config.h"
#include "core/log.h"
#include "ui/minimize.h"

namespace dx {
namespace {

UINT ToAbe(Edge e) {
    switch (e) {
        case Edge::Left: return ABE_LEFT;
        case Edge::Right: return ABE_RIGHT;
        case Edge::Bottom:
        default: return ABE_BOTTOM;
    }
}

void SetThickness(Edge e, RECT& rc, int thickness) {
    switch (e) {
        case Edge::Left: rc.right = rc.left + thickness; break;
        case Edge::Right: rc.left = rc.right - thickness; break;
        case Edge::Bottom:
        default: rc.top = rc.bottom - thickness; break;
    }
}

}  // namespace

// ---------------------------------------------------------------- AppBar

void AppBar::Register(HWND hwnd, UINT callbackMsg) {
    hwnd_ = hwnd;
    msg_ = callbackMsg;
    if (registered_) return;
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = hwnd_;
    abd.uCallbackMessage = msg_;
    registered_ = SHAppBarMessage(ABM_NEW, &abd) != FALSE;
    lastThickness_ = -1;
    if (!registered_) Log(L"ABM_NEW failed");
}

void AppBar::Remove() {
    if (!registered_) return;
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = hwnd_;
    SHAppBarMessage(ABM_REMOVE, &abd);
    registered_ = false;
}

void AppBar::WindowPosChanged() {
    if (!registered_) return;
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = hwnd_;
    SHAppBarMessage(ABM_WINDOWPOSCHANGED, &abd);
}

RECT AppBar::Place(Edge edge, int thickness, const RECT& monitor, bool force) {
    if (!registered_) Register(hwnd_, msg_);
    if (!registered_) return RECT{};  // caller falls back to the work area
    if (!force && edge == lastEdge_ && thickness == lastThickness_) return last_;

    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = hwnd_;
    abd.uEdge = ToAbe(edge);
    abd.rc = monitor;
    SetThickness(edge, abd.rc, thickness);
    // The shell moves the rectangle off other bars (like the taskbar)...
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    // ...then we restore our thickness from the adjusted outer side and claim it.
    SetThickness(edge, abd.rc, thickness);
    SHAppBarMessage(ABM_SETPOS, &abd);
    last_ = abd.rc;
    lastEdge_ = edge;
    lastThickness_ = thickness;
    return last_;
}

// ---------------------------------------------------------------- Taskbar

UINT GetTaskbarState() {
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    return static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
}

void SetTaskbarState(UINT state) {
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    abd.lParam = state;
    SHAppBarMessage(ABM_SETSTATE, &abd);
}

// ---------------------------------------------------------------- Fullscreen

bool IsFullscreenOnPrimary(HWND self) {
    const HWND fg = GetForegroundWindow();
    if (!fg || fg == self) return false;
    wchar_t cls[64]{};
    GetClassNameW(fg, cls, ARRAYSIZE(cls));
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
        if (wcscmp(cls, shell) == 0) return false;
    const HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) != primary) return false;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(primary, &mi);
    RECT wr{};
    GetWindowRect(fg, &wr);
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right &&
           wr.bottom >= mi.rcMonitor.bottom;
}

// ---------------------------------------------------------------- Watchdog

void StartWatchdog(HWND dock) {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --watchdog " + std::to_wstring(GetCurrentProcessId()) + L" " +
                       std::to_wstring(reinterpret_cast<uintptr_t>(dock));
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        Log(L"Watchdog failed to start (%lu)", GetLastError());
    }
}

int RunWatchdog(DWORD pid, HWND dock) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return 0;
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
    WaitForSingleObject(process, INFINITE);
    CloseHandle(process);

    // The dock is gone. Release its reserved space (harmless if it already did).
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = dock;
    SHAppBarMessage(ABM_REMOVE, &abd);

    // Put the taskbar and the Windows minimize animation back if the dock changed them and could not undo it.
    Config cfg = LoadConfig();
    bool save = false;
    if (cfg.taskbarHiddenByDock) {
        SetTaskbarState(static_cast<UINT>(cfg.taskbarOriginalState));
        cfg.taskbarHiddenByDock = false;
        save = true;
    }
    if (cfg.systemMinAnimChanged) {
        SetSystemMinimizeAnimation(true);
        cfg.systemMinAnimChanged = false;
        save = true;
    }
    if (save) SaveConfig(cfg);
    return 0;
}

}  // namespace dx
