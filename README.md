# DockX

**A native macOS-style Dock for Windows 11** — glass, magnification, bouncing icons, Stacks, App Exposé and the
Genie effect — written in C++20 with Win32 and Windows.UI.Composition. No Electron, no .NET, no injection into
other apps.

| | |
| --- | --- |
| Idle CPU | **0 %** — nothing runs until you move the mouse over the dock or a window changes |
| Animations | Your monitor's refresh rate (tested at **144 fps**) |
| Memory | about 65 MB (the Windows compositor accounts for most of it) |
| Windows | Windows 11 (22H2 or newer recommended) |

## Features

**Looks like macOS**
- Blurred translucent glass shelf with rounded corners, a light edge and a soft shadow
- Dark and light themes, glass color and density, icon size, spacing, corner radius — all adjustable
- Running-app dots, app names above icons, separators between sections

**Moves like macOS**
- Magnification that follows the cursor (spring physics, interruptible at any moment)
- Launch bounce until the app's window appears, attention bounce when an app flashes
- **Genie** or **Scale** minimize effect: the window bends into a funnel and pours into its dock icon, and comes back out of it
- Smooth glide when icons are added, removed or reordered

**Works like macOS**
- Pinned apps, then running and recently used apps after a separator, then Stacks and the Recycle Bin
- Click to open or switch; click and hold a running app for **App Exposé** (live previews of all its windows)
- Right-click menus in the macOS style: windows list, Options › Keep in Dock / Show in Explorer, Show All Windows,
  Hide, Quit (hold **Shift** for Force Quit)
- Drag icons to reorder; drag one away and let go to remove it (*poof*)
- Drop an app, shortcut or folder on the dock to add it; drop files on an app to open them with it
- **Stacks**: folders (Downloads by default) open as a fan, grid or list with thumbnails; press and hold to peek,
  let go over a file to open it; drag files out, or drop files in
- **Recycle Bin**: shows empty/full, drop files to recycle them (never a permanent delete), right-click to empty
- Bottom, left or right of the screen; reserves screen space like the taskbar (optional)
- Auto-hide with an invisible edge trigger; hides automatically when an app is fullscreen (games, videos)
- A **hide button** at the end of the dock and **Ctrl+Alt+D** slide the dock down / keep it shown
- A **Windows button** at the start of the dock: click for the Start menu, right-click for the Win+X menu
- Optionally auto-hides the Windows taskbar while it runs

**Safe**
- A tiny watchdog process restores the taskbar, the screen space and the Windows minimize animation if DockX
  crashes or is killed
- Runs as a normal user: no admin rights, no drivers, no code injected into other processes

## DockX Settings

A separate settings app in the style of macOS System Settings (`DockX.Settings.exe`, also in the Start menu as
**DockX Settings**, or right-click the dock › **Dock Settings…**). Every change shows on the dock immediately.

- **Appearance** — theme, icon size, spacing, padding, corner radius, glass blur / color / density, edge, shadow,
  dots, names
- **Animations** — magnification (size, reach, speed), minimize effect (Genie / Scale / Windows), bounces,
  overall animation speed
- **Behavior** — position, reserve screen space, auto-hide and its delays, hide in fullscreen, hide the taskbar,
  start with Windows, recent apps, Recycle Bin, Stack view and sorting
- **Advanced** — language (English / Serbian), log file, presets (macOS, Minimal, Lite, Large), reset, export /
  import, restart the dock

## Using it

| To | Do this |
| --- | --- |
| Start DockX | Start menu › **DockX** (it also starts with Windows) |
| Quit DockX | **DockX Settings** › **Quit Dock** (bottom left), or right-click the strip under the icons › **Quit DockX** |
| Hide the dock / bring it back | Click the chevron button at the end of the dock, or press **Ctrl+Alt+D**; when hidden, move the mouse to the screen edge |
| Change settings | Right-click the strip under the icons › **Dock Settings…** |
| Add an app | Drag it (or its shortcut) from the desktop / Explorer onto the dock |
| Remove an app | Drag it away from the dock and let go |
| See all windows of an app | Click and hold its icon |
| Peek into a folder | Click and hold its Stack |

Settings are stored in `%APPDATA%\DockX\config.json`; icons are cached in `%LOCALAPPDATA%\DockX\cache`.

## Build

Needs the Visual Studio 2022 Build Tools (C++ workload + Windows 11 SDK) and CMake.

```bat
build.cmd
build\DockX.exe
```

`build.cmd` builds `DockX.exe`, `DockX.Settings.exe` and the unit tests, and runs the tests.

## How it works

- **Rendering:** one Windows.UI.Composition visual tree per window. The system compositor draws and blurs
  everything (the glass uses the host backdrop brush); DockX only updates positions while something moves.
- **Frame clock:** a background thread waits on the compositor clock and wakes the UI thread once per frame —
  only while a spring is still moving. Then it sleeps on an event (0 % CPU).
- **Physics:** critically damped springs (damping ratio + response, Apple style) for magnification, hiding,
  reordering and appearing icons.
- **Apps:** shell hooks and window events (no polling) track windows; icons are extracted on a background thread
  and cached as PNG.
- **Genie:** the window is shown with live DWM thumbnails cut into strips, each moved along the genie curve.
- **Screen space:** the dock registers as an AppBar, so Windows knows to keep maximized windows above it.

```
src/core      config (JSON), strings (English / Serbian), logging
src/anim      springs
src/render    Direct3D / Direct2D devices, composition, shadows
src/shell     window tracking, icons, AppBar & taskbar, files & Recycle Bin, drag & drop
src/ui        the dock, layout math, menus, App Exposé, Stacks, minimize effects
settings      the DockX Settings app
tests         unit tests for the layout, springs, genie shape and config
```

## Status

| Milestone | State |
| --- | --- |
| 1. Skeleton: glass shelf, config, single instance, DPI | done |
| 2. Apps: pinned + running, icon cache, launch / switch, Store apps | done |
| 3. Springs, magnification, labels, press feedback, bounces | done |
| 4. Screen space, auto-hide, fullscreen, left / right, taskbar hiding + watchdog | done |
| 5. Drag to reorder / remove, file drops, macOS menus, App Exposé | done |
| 6. Separators, recent apps, Stacks, Recycle Bin | done |
| 7. Genie and Scale minimize effects | done |
| 8. DockX Settings app | done |
| 9. Polish: performance overlay, global hotkey, lower memory, notification badges | next |
