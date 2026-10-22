#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "core/config.h"

namespace dx {

// One top-level app window, as the taskbar would show it.
struct AppWindow {
    HWND hwnd = nullptr;
    std::wstring exe;    // full process path
    std::wstring aumid;  // AppUserModelID, if any
    bool packaged = false;  // Store / MSIX app
};

// Visible app windows in Z order (most recently active first).
std::vector<AppWindow> EnumAppWindows(DWORD selfPid);

struct ShortcutInfo {
    std::wstring target;
    std::wstring aumid;
};
ShortcutInfo ReadShortcut(const std::wstring& lnkPath);

// Launches without blocking the UI thread.
void LaunchApp(const std::wstring& path, const std::wstring& aumid);
// Opens files with an app (files dropped on its dock icon).
void OpenFilesWith(const std::wstring& exe, const std::wstring& aumid, std::vector<std::wstring> files);
void MinimizeWindows(const std::vector<HWND>& windows);
void CloseWindows(const std::vector<HWND>& windows);
void ForceQuitWindows(const std::vector<HWND>& windows);
void ShowInExplorer(const std::wstring& path);
void ActivateWindow(HWND hwnd);
std::wstring WindowTitle(HWND hwnd);

// "shell:AppsFolder\<aumid>" exists?
bool IsAppsFolderItem(const std::wstring& aumid);

std::vector<PinnedItem> DefaultPinnedApps();

// Start DockX when you sign in (HKCU\...\Run).
void SetStartWithWindows(bool on);
// "DockX" in the Start menu and on the desktop.
void CreateAppShortcuts();
// "DockX podešavanja" in the Start menu, if the settings app is next to DockX.exe.
void EnsureSettingsShortcut();
void OpenSettingsApp();

}  // namespace dx
