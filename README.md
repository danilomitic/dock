# DockX

A native macOS-style Dock for Windows 11, written in C++20 with Win32 and Windows.UI.Composition.

## Status

| Milestone | State |
| --- | --- |
| 1. Skeleton: glass shelf, config, single instance, DPI | done |
| 2. Apps: pinned + running apps, icons with disk cache, launch/focus, Store apps | done |
| 3. Springs + magnification, labels, press feedback, launch/attention bounce | done |
| 4. Mac behavior: reserved space, auto-hide, fullscreen, left/right, taskbar hiding + crash watchdog | done |
| 5. Interaction: drag reorder, drag out to remove ("poof"), drop files on apps, drop apps to pin, mac-style menus, App Exposé | done |
| 6. Right side: separator, Stacks, Recycle Bin, recent apps | next |
| 7–9 | planned |

Measured on a 1920×1080 / 144 Hz PC: **0 % CPU when idle**, animations at **144 fps**, about **66 MB** private memory.

## Build

Needs Visual Studio 2022 Build Tools (C++ workload, Windows 11 SDK) and CMake.

```bat
build.cmd
build\DockX.exe
```

Using it:

- Click an app to open or switch to it. Click and hold a running app for **App Exposé** (live previews of its windows).
- Drag icons to reorder them. Drag one away from the dock and let go to remove it (*poof*).
- Drop files on an app to open them with it. Drop an `.exe`, shortcut or folder on the dock to add it.
- Right-click an icon for its menu (windows, Options, Show All Windows, Hide, Quit — hold Shift for Force Quit).

Settings live in `%APPDATA%\DockX\config.json`. Right-click the strip under the icons for the dock menu:
auto-hide, magnification, position (left / bottom / right), hide the Windows taskbar, theme, quit.

If DockX is killed or crashes, a small watchdog process (`DockX.exe --watchdog`) gives the
reserved screen space back and restores the taskbar.
