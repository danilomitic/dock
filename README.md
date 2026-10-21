# DockX

A native macOS-style Dock for Windows 11, written in C++20 with Win32 and Windows.UI.Composition.

## Status

| Milestone | State |
| --- | --- |
| 1. Skeleton: glass shelf, config, single instance, DPI | done |
| 2. Apps: pinned + running apps, icons with disk cache, launch/focus, Store apps | done |
| 3. Springs + magnification, labels, press feedback, launch/attention bounce | done |
| 4. Mac behavior: reserved space, auto-hide, fullscreen, left/right, taskbar hiding + crash watchdog | done |
| 5. Interaction: drag reorder, drag out to remove, drop files, mac-style menus, App Exposé | next |
| 6–9 | planned |

Measured on a 1920×1080 / 144 Hz PC: **0 % CPU when idle**, animations at **144 fps**, about **66 MB** private memory.

## Build

Needs Visual Studio 2022 Build Tools (C++ workload, Windows 11 SDK) and CMake.

```bat
build.cmd
build\DockX.exe
```

Settings live in `%APPDATA%\DockX\config.json`. Right-click an empty part of the dock for options:
auto-hide, magnification, position (left / bottom / right), hide the Windows taskbar, theme, quit.

If DockX is killed or crashes, a small watchdog process (`DockX.exe --watchdog`) gives the
reserved screen space back and restores the taskbar.
