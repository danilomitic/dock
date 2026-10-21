# Prompt: macOS-style Dock for Windows 11 (native, ultra-optimized)

> Copy everything below this line into Claude Code (or another coding AI) inside an empty folder.
> Replace `DockX` with the name you want.

---

## Role

You are a senior Windows systems engineer and design engineer. You write native C++ for Windows 11 and you care about Apple-level motion and polish. Build **DockX**: a native dock for Windows 11 that looks and behaves exactly like the macOS Dock (Sonoma/Sequoia style), but where nearly every visual and behavioral property can be customized.

Work in milestones (see the end). After each milestone the app must build, run and pass its checks before you continue. Never leave the user's taskbar or windows in a broken state.

## Skills to load before you start

If these skills are installed, load them and follow them:

| Skill | Use it for |
| --- | --- |
| `cpp-pro` | All C++ code: C++20, RAII, no raw `new`/`delete`, sanitizers, CMake. On MSVC use `/W4 /permissive- /WX` instead of `-Wall -Wextra`, and run `/fsanitize=address` builds in debug. |
| `apple-design` | Springs (damping + response), interruptible motion, velocity handoff, rubber-banding, glass materials, reduced motion. Translate the web examples to the D3D/DirectComposition renderer. |
| `emil-design-eng` | Polish details: press feedback, never animate from scale 0, no animation on keyboard actions, fast exits. |
| `review-animations` | Review the finished motion (milestones 3 and 7) before you call them done. |

There is no public skill for Win32/DirectComposition: the Windows-specific rules are in this prompt.

## Hard requirements (non-negotiable)

| Area | Requirement |
| --- | --- |
| RAM | **≤ 100 MB** private working set in the worst case (all features on, 40 icons, 4K screen). Target **≤ 40 MB** when idle. |
| CPU idle | **0.0 %** when nothing is happening. No polling timers, no render loop while idle. Wake only on OS events or mouse input over the dock. |
| Frame rate | Animations run at the **monitor's refresh rate** (60/120/144/165/240 Hz), synced to the compositor clock. Zero dropped frames on a mid-range PC. |
| Weak PCs | Must run on Windows 11 with 4 GB RAM and integrated graphics. Ship a **Lite mode** (see Settings). |
| Startup | Dock visible in **< 300 ms** after launch (icons may stream in from cache). |
| Input latency | Hover/press feedback within **1 frame**. |
| Privileges | Runs as a normal user, no admin, no driver, no DLL injection into other processes. |
| Safety | If DockX crashes or is killed, the Windows taskbar and all windows must be in a usable state (restore taskbar auto-hide state, restore any reserved screen space). |

## Tech stack

- **C++20, MSVC, CMake.** No .NET, no Electron, no Qt, no web view in the dock process.
- **Win32** for windows and messages, **DirectComposition + Direct3D 11 + Direct2D/DirectWrite** for rendering. Everything that moves is a composition visual or drawn in one D2D pass. Animate transforms/opacity only.
- **WIC** for image decoding, **IShellItemImageFactory** for high-resolution app icons (256 px), **WinRT (C++/WinRT)** only where needed (Windows.Graphics.Capture, notifications).
- **Settings app = a separate executable** (`DockX.Settings.exe`, WinUI 3 or Win32+D2D). Start it only when the user opens settings, and close it fully afterwards, so the dock process stays small. The two talk through the config file plus a named event or `WM_COPYDATA` for live preview.
- Dependencies: as few as possible. A small JSON library (e.g. nlohmann/json, or hand-written) is fine. Static CRT is fine.
- Per-monitor DPI awareness v2. Everything is DPI-correct and sharp at 100–300 % scaling.

## Performance architecture (how to reach 0 % idle CPU)

1. **Event-driven only.**
   - Window tracking: `SetWinEventHook` (out-of-context) for `EVENT_OBJECT_CREATE/DESTROY/SHOW/HIDE/NAMECHANGE`, `EVENT_SYSTEM_FOREGROUND`, `EVENT_SYSTEM_MINIMIZESTART/END`, plus `RegisterShellHookWindow` (`HSHELL_WINDOWCREATED`, `HSHELL_WINDOWDESTROYED`, `HSHELL_FLASH`, `HSHELL_RUDEAPPACTIVATED`).
   - Listen for `TaskbarCreated` (Explorer restarts), `WM_DISPLAYCHANGE`, `WM_DPICHANGED`, `WM_SETTINGCHANGE`, and power/battery notifications.
   - Folders (Stacks, Recycle Bin): `ReadDirectoryChangesW` / `SHChangeNotifyRegister`, never polling.
2. **Render loop only while something animates.** Use a frame loop synced to the compositor (`DCompositionWaitForCompositorClock` or a waitable swap chain / `DwmFlush`). When every spring has settled (|velocity| and |offset| below a threshold), stop the loop and block in `MsgWaitForMultipleObjectsEx`.
3. **Auto-hide without polling:** a 1–2 px invisible trigger window at the screen edge wakes the dock on mouse enter. Never poll the cursor position.
4. **Icon pipeline:** extract icons on a background thread pool, scale them once to the needed sizes (normal size and magnified size × DPI), pack them into a GPU texture atlas, and cache them on disk (`%LOCALAPPDATA%\DockX\cache`) keyed by path + timestamp. Free CPU-side bitmaps after upload.
5. **Memory hygiene:** one D3D device, one atlas, no per-frame allocations, string interning for paths/AUMIDs. After 30 s idle, trim caches and call `SetProcessWorkingSetSizeEx` / `EmptyWorkingSet` (optional setting).
6. **Battery:** on battery saver, cap the frame rate at 60 Hz and switch to Lite effects automatically (can be turned off).
7. Add a built-in **performance overlay** (hidden setting, `Ctrl+Alt+Shift+P`) that shows FPS, frame time, RAM and CPU, so the budgets above can be checked.

## Look (default = identical to macOS)

- A floating rounded **glass shelf**: blurred, translucent background, a 1 px light inner border on top (light catching the glass), soft shadow underneath. Default sizes match macOS (icon 48 pt, shelf padding and radius like Sonoma).
- **Blur:** use the Windows 11 Acrylic/Mica backdrop via `DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE)` where possible. For the custom rounded shape and tint, fall back to `SetWindowCompositionAttribute` (ACCENT_ENABLE_ACRYLICBLURBEHIND) behind a feature check. If blur is unavailable (or Lite mode is on), draw a solid tinted shelf. Never crash because of a missing API.
- **Running indicator:** small dot under running apps (mac style).
- **Separator** line between pinned apps and the right side (recent apps, Stacks, Recycle Bin).
- **App name label** above the hovered icon: a small rounded glass bubble with the app name, like macOS.
- **Light and Dark themes**, chosen **manually** in settings (they do not follow Windows automatically).
- Icons are always sharp: they are drawn from the 256 px source at the current magnified size, never upscaled from the small size.

## Motion (Apple physics)

All motion uses **springs** (damping ratio + response, Apple style), never fixed-duration tweens, so everything can be interrupted and started from the current on-screen value. Every value below is the default and is adjustable in settings.

| Interaction | Behavior | Default spring |
| --- | --- | --- |
| **Magnification** | Icons near the cursor grow with a smooth bell-shaped falloff over about 3 icons each side. Neighbors move aside so nothing overlaps, and the shelf widens. This must feel exactly like macOS. | damping 1.0, response 0.12 (follows the cursor closely) |
| Magnification exit | The mouse leaves and icons return to rest. | damping 1.0, response 0.25 |
| **Launch bounce** | The icon jumps up and down (mac launch bounce) until the app's first window appears, max ~10 s. | parabolic jump, ~0.6 s cycle |
| **Attention bounce** | Driven by `HSHELL_FLASH`: 3 bounces higher than the launch bounce, then stays still with the dot until the app is focused. | — |
| **Show/hide (auto-hide)** | The dock slides from its edge. Delay before showing: 0 ms (option: 0–1 s). | damping 1.0, response 0.35 |
| **Reorder by drag** | The dragged icon follows the cursor 1:1 from the point you grabbed it, other icons slide aside. Dropping uses the release velocity. | damping 1.0, response 0.3 |
| **Remove** | Drag an icon out of the dock and hold briefly: a "Remove" label appears, release → mac "poof" cloud. | — |
| **Minimize effect** | **Genie** (default), **Scale**, or **None**. | — |
| **Press** | Icon darkens slightly on mouse-down (instant), and acts on mouse-up. | — |

**Genie effect implementation:** capture the window with `Windows.Graphics.Capture` (or `DwmRegisterThumbnail` for the Scale effect) into a texture, warp it with a D3D11 mesh along the genie curve into the dock icon, and minimize the real window with the system animation turned off for that window (`DWMWA_TRANSITIONS_FORCEDISABLED`). Restore plays the same path in reverse (same path in and out). If capture fails for a window, fall back to Scale.

**Reduce motion** setting: replace movement with short cross-fades and turn off bounces and magnification.

## Behavior (like macOS)

- **Position:** bottom, left or right (chosen in settings). **Main monitor only.** Moves correctly when the main monitor or resolution changes.
- **Screen space:** when auto-hide is off, the dock reserves its space like macOS (register as an AppBar with `SHAppBarMessage` so maximized windows stop above it). When auto-hide is on, nothing is reserved.
- **Windows taskbar:** DockX does **not** replace it. Settings offer "Auto-hide Windows taskbar while DockX runs". Save the original taskbar state and restore it on exit and on crash (watchdog or restore-on-next-start).
- **Fullscreen apps** (games, video, presentations): the dock hides, like macOS. Detect with `SHQueryUserNotificationState` and an AppBar `ABN_FULLSCREENAPP`.
- **Pinned + running apps (mac model):** pinned apps stay on the left in the user's order. Running apps that are not pinned appear after them. Optional "Show recent apps" section (last 3) after the separator.
- **App identity:** group windows by **AppUserModelID**, falling back to the executable path. Support Win32 apps, UWP/Store apps (launch through `shell:AppsFolder\<AUMID>` / `IApplicationActivationManager`), and `.lnk` shortcuts. Skip tool windows, cloaked windows (`DWMWA_CLOAKED`), and owned popups.
- **Click a running app:** focus its last active window. Click again while it is focused: nothing (mac default; option: minimize).
- **App Exposé (instead of hover thumbnails, like macOS):** right-click (or click-and-hold) on a running app shows its windows. Use live `DwmRegisterThumbnail` previews in a glass panel above the dock. Click a preview to focus that window.
- **Right-click menu** (custom-drawn, mac style, not the Win32 menu): window list · Options → Keep in Dock / Open at Login / Show in Explorer · Hide · Quit (Shift turns it into Force Quit). Separate dock menu on the shelf: Turn Hiding On/Off, Magnification On/Off, Position, Dock Settings…
- **Drag & drop:**
  - Drag an app, shortcut or folder from Explorer onto the dock → pin it.
  - Drag files onto an app icon → open them with that app (icon highlights while hovering).
  - Drag files onto the Recycle Bin → delete them (move to the Recycle Bin, never permanent delete).
- **Stacks:** folders on the right side (default: Downloads). Click → **Fan**, **Grid** or **List** view (mac style), sort by name/date added/kind. Auto-updates when the folder changes.
- **Recycle Bin** at the end of the dock. The icon is empty or full based on `SHQueryRecycleBin`. Right-click → Empty Recycle Bin, after a confirmation dialog.
- **Badges:** red number badges on icons when the count can be known: notification counts through `UserNotificationListener` (ask the user for permission, off until granted). Flashing windows always count as "attention". If there is no data for an app, show nothing. Never show fake badges.
- **Keyboard:** a global hotkey to show/hide the dock (default `Ctrl+Alt+D`, can be changed). Hotkey actions are **not animated**: they act instantly.
- Single instance (named mutex). Starts with Windows (`HKCU\...\Run`, setting on by default). Elevated windows: handle UIPI gracefully (they show up, but some actions may not work. No crashes).

## Settings (everything customizable)

A clean settings window grouped like macOS System Settings → Desktop & Dock, with an **Advanced** section. Every change previews live on the dock. Settings are saved in `%APPDATA%\DockX\config.json` (written atomically). Include **Export/Import** and **Reset to macOS defaults**. Ship 4–5 presets (macOS Sonoma, Big Sur, Mojave 3D shelf, Minimal, Lite).

**Appearance**
- Theme: Light / Dark (manual)
- Glass: blur on/off, blur strength, **glass tint color + opacity** (color picker), saturation, noise/grain amount
- Shelf: corner radius, padding, height, border on/off + color/opacity, shadow on/off + size/opacity/offset
- Style: flat glass (Sonoma) / 3D shelf (Mojave)
- Icon size, spacing between icons, icon shadow on/off + strength
- Running indicator: on/off, shape (dot/line), size, color
- Labels: on/off, font size, style
- Separator: on/off, color

**Animation**
- Magnification: on/off, max size, range (number of icons affected), spring response
- Launch bounce on/off, attention bounce on/off, bounce height
- Minimize effect: Genie / Scale / None, speed
- Auto-hide: on/off, show delay, hide delay, slide speed
- Global animation speed multiplier (0.5×–2×)
- Reduce motion

**Behavior**
- Position: bottom/left/right · reserve screen space on/off
- Auto-hide Windows taskbar on/off
- Hide in fullscreen apps on/off
- Show recent apps on/off and how many
- Minimize windows into the app icon / into the right side (mac option)
- Click on focused app: nothing / minimize
- Global hotkey
- Start with Windows

**Performance**
- Frame-rate limit: monitor refresh / 120 / 60
- **Lite mode** (no blur, Scale instead of Genie, 60 Hz, smaller cache)
- Auto Lite on battery saver
- Trim memory when idle
- Show performance overlay

**Language:** Serbian (Latin) and English, switchable. All strings in resource files.

## Code quality

- Clear modules: `core/` (config, logging), `shell/` (window tracking, app identity, AppBar, taskbar, Recycle Bin, Stacks), `render/` (device, atlas, composition tree, genie mesh), `anim/` (spring solver), `ui/` (dock layout, menus, labels, Exposé), `settings/` (separate exe).
- Spring solver: an analytic damped harmonic oscillator using damping ratio + response, with the velocity carried over when the target changes.
- Layout and magnification math are pure functions with unit tests.
- RAII for every handle and COM object. No leaks: check with a 24-hour soak test (RAM must stay flat).
- Logging to a small rotating file, off by default.
- No telemetry, no network access.

## Milestones (build in this order)

1. **Skeleton:** CMake project, single-instance check, DPI awareness, transparent layered DirectComposition window at the bottom of the main monitor, empty glass shelf, config load/save. Check: 0 % CPU idle, < 20 MB RAM.
2. **Apps:** pinned apps from config, icon extraction + atlas + disk cache, click to launch/focus, running detection with dots, Store app support.
3. **Magnification + springs:** spring solver, bell-curve magnification, labels, press feedback, render loop that stops when idle. Check FPS = refresh rate, then 0 % CPU after settling.
4. **Mac behavior:** AppBar space, auto-hide with edge trigger window, fullscreen detection, taskbar auto-hide with safe restore, position left/right, Explorer restart handling.
5. **Interaction:** drag reorder, drag-out "poof" remove, drop to pin, drop files onto apps, right-click menus, App Exposé.
6. **Right side:** separator, Stacks (fan/grid/list), Recycle Bin, recent apps.
7. **Animations:** launch/attention bounce, Scale and Genie minimize.
8. **Settings app:** every option above, live preview, presets, import/export, Serbian + English.
9. **Polish & performance:** badges, Lite mode, battery mode, performance overlay, 24-hour soak test, test on a weak PC profile (60 Hz, integrated GPU, 4 GB RAM).

At the end of each milestone, report: what works, measured RAM (idle/peak), idle CPU, FPS during magnification, and known issues.

## Definition of done

- Looks and feels like the macOS Dock with default settings: someone who uses a Mac would not notice a difference in magnification, bounce, or genie motion.
- All settings above work and preview live.
- Idle CPU 0 %, RAM ≤ 100 MB worst case (≤ 40 MB idle), animations at full refresh rate.
- Killing the process at any moment leaves Windows fully usable.
