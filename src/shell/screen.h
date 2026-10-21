#pragma once
#include <windows.h>

#include "ui/layout.h"

namespace dx {

// Registers the dock as a Windows "AppBar" so the shell can reserve screen
// space for it (maximized windows stop at the dock) and tell it when an app
// goes fullscreen.
class AppBar {
public:
    void Register(HWND hwnd, UINT callbackMsg);
    void Remove();
    void Reset() { registered_ = false; }  // after Explorer restarts
    void WindowPosChanged();

    // Reserves `thickness` px along `edge` of `monitor` (0 = reserve nothing)
    // and returns the bar rectangle. Its outer side is where the dock sits.
    RECT Place(Edge edge, int thickness, const RECT& monitor, bool force);

private:
    HWND hwnd_ = nullptr;
    UINT msg_ = 0;
    bool registered_ = false;
    Edge lastEdge_ = Edge::Bottom;
    int lastThickness_ = -1;
    RECT last_{};
};

// Taskbar "automatically hide" state (ABS_AUTOHIDE / ABS_ALWAYSONTOP bits).
UINT GetTaskbarState();
void SetTaskbarState(UINT state);

// True when the foreground window covers the whole primary monitor
// (games, videos, presentations, F11 browsers).
bool IsFullscreenOnPrimary(HWND self);

// A tiny copy of DockX that waits for the dock to exit and puts the taskbar
// and screen space back if the dock crashed or was killed.
void StartWatchdog(HWND dock);
int RunWatchdog(DWORD pid, HWND dock);

}  // namespace dx
