#include "ui/dock.h"

#include <d2d1_1helper.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>

#include "core/log.h"
#include "core/strings.h"
#include "ui/expose.h"
#include "ui/menu.h"

namespace dx {
namespace {

Dock* g_dock = nullptr;

constexpr UINT_PTR kTimerRescan = 1;
constexpr UINT_PTR kTimerRescanLate = 2;
constexpr UINT_PTR kTimerShow = 3;
constexpr UINT_PTR kTimerHide = 4;
constexpr UINT_PTR kTimerHold = 5;

constexpr double kLaunchBouncePeriod = 0.62;
constexpr double kAttentionBouncePeriod = 0.5;
constexpr double kMaxLaunchBounce = 10.0;
constexpr double kPoofDuration = 0.45;
constexpr int kPuffs = 7;
constexpr UINT kHoldMs = 500;

// Menu commands
enum : UINT {
    kCmdKeep = 1, kCmdShow, kCmdClose, kCmdForceQuit, kCmdHide, kCmdOpen, kCmdExpose,
    kCmdMagnify, kCmdDark, kCmdAutoHide, kCmdPosLeft, kCmdPosBottom, kCmdPosRight, kCmdHideTaskbar, kCmdQuitDock,
    kCmdWindow = 100
};

float F(double v) { return static_cast<float>(v); }

bool EndsWith(const std::wstring& s, const wchar_t* suffix) { return Lower(s).ends_with(suffix); }

UINT Ms(double seconds) { return static_cast<UINT>(std::max(1.0, seconds * 1000)); }

MenuItem Item(const wchar_t* text, UINT id, bool checked = false) {
    MenuItem m;
    m.text = text;
    m.id = id;
    m.checked = checked;
    return m;
}

}  // namespace

// ---------------------------------------------------------------- FrameClock

void FrameClock::Init(HWND target) {
    target_ = target;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread([this] { Loop(); });
}

void FrameClock::Shutdown() {
    if (!thread_.joinable()) return;
    quit_ = true;
    running_ = false;
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
    wake_ = nullptr;
}

void FrameClock::Run() {
    running_ = true;
    SetEvent(wake_);
}

void FrameClock::Halt() { running_ = false; }

void FrameClock::Loop() {
    using WaitForClock = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);
    HMODULE dcomp = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto waitForClock =
        dcomp ? reinterpret_cast<WaitForClock>(GetProcAddress(dcomp, "DCompositionWaitForCompositorClock")) : nullptr;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (!quit_) {
        WaitForSingleObject(wake_, INFINITE);
        while (running_ && !quit_) {
            // Block until the compositor's next frame (matches the monitor refresh rate).
            const DWORD r = waitForClock ? waitForClock(0, nullptr, 100) : WAIT_FAILED;
            if (r == WAIT_FAILED && FAILED(DwmFlush())) Sleep(8);
            if (!pending_.exchange(true)) PostMessageW(target_, WM_APP_TICK, 0, 0);
        }
    }
    if (dcomp) FreeLibrary(dcomp);
}

// ---------------------------------------------------------------- Dock setup

Dock::~Dock() {
    Log(L"shutdown: start");
    if (cloakHook_) UnhookWinEvent(cloakHook_);
    if (foregroundHook_) UnhookWinEvent(foregroundHook_);
    if (hwnd_) RevokeDragDrop(hwnd_);
    if (edgeWnd_) RevokeDragDrop(edgeWnd_);
    icons_.Stop();
    clock_.Shutdown();
    RestoreTaskbar();
    appBar_.Remove();
    ghost_.Destroy();
    if (edgeWnd_) DestroyWindow(edgeWnd_);
    if (hwnd_) {
        DeregisterShellHookWindow(hwnd_);
        DestroyWindow(hwnd_);
    }
    Log(L"shutdown: done");
    g_dock = nullptr;
}

bool Dock::Create(HINSTANCE instance) {
    g_dock = this;
    QueryPerformanceFrequency(&qpcFreq_);
    cfg_ = LoadConfig();
    EnableFileLog(cfg_.logging);
    SetLanguage(cfg_.language);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = instance;
    wc.lpszClassName = L"DockX.Window";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                            wc.lpszClassName, L"DockX", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    const BOOL on = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_HOSTBACKDROPBRUSH, &on, sizeof(on));
    DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &on, sizeof(on));
    DwmSetWindowAttribute(hwnd_, DWMWA_EXCLUDED_FROM_PEEK, &on, sizeof(on));

    if (!gfx_.Init(hwnd_)) return false;
    CreateEdgeTrigger(instance);

    magnify_.damping = 1.0;
    hide_.damping = 1.0;
    hide_.response = 0.35;

    appBar_.Register(hwnd_, WM_APP_BAR);
    ApplyTaskbarSetting();
    ComputeMetrics();
    PositionWindow(true);
    BuildChrome();
    BuildShadow();
    ApplyTheme();

    icons_.Start(hwnd_, WM_APP_ICON);
    clock_.Init(hwnd_);

    LoadPinned();
    Rescan();

    fullscreen_ = cfg_.hideInFullscreen && IsFullscreenOnPrimary(hwnd_);
    if (AutoHideActive()) {
        hide_.target = 1;
        hide_.Snap();
    }
    ApplyLayout();
    UpdateRegion();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    if (hide_.target == 1) {
        PlaceEdgeTrigger();
        ShowWindow(edgeWnd_, SW_SHOWNOACTIVATE);
    }

    shellHookMsg_ = RegisterWindowMessageW(L"SHELLHOOK");
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    RegisterShellHookWindow(hwnd_);
    cloakHook_ = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, WinEventProc, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    RegisterDropTargets();
    StartWatchdog(hwnd_);
    Log(L"DockX started: %d items, scale %.2f, edge %S", static_cast<int>(items_.size()), m_.scale, cfg_.position.c_str());
    return true;
}

void Dock::CreateEdgeTrigger(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = EdgeProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"DockX.Edge";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    // Invisible (no content) but it still receives the mouse: the auto-hide trigger.
    edgeWnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                               wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
}

void Dock::RegisterDropTargets() {
    DropTarget::Callbacks cb;
    cb.enter = [this](const std::vector<std::wstring>& files, POINT p) {
        dropFiles_ = files;
        return DropOver(p);
    };
    cb.over = [this](POINT p) { return DropOver(p); };
    cb.leave = [this] { DropLeave(); };
    cb.drop = [this](const std::vector<std::wstring>& files, POINT p) { return DropFiles(files, p); };
    dropTarget_ = winrt::make_self<DropTarget>(hwnd_, cb);
    if (FAILED(RegisterDragDrop(hwnd_, dropTarget_.get()))) Log(L"RegisterDragDrop failed");

    // Dragging a file to the screen edge reveals an auto-hidden dock.
    DropTarget::Callbacks edge;
    edge.enter = [this](const std::vector<std::wstring>&, POINT) {
        OnEdgeHover();
        return static_cast<DWORD>(DROPEFFECT_NONE);
    };
    edge.over = [this](POINT) {
        OnEdgeHover();
        return static_cast<DWORD>(DROPEFFECT_NONE);
    };
    edgeDropTarget_ = winrt::make_self<DropTarget>(edgeWnd_, edge);
    RegisterDragDrop(edgeWnd_, edgeDropTarget_.get());
}

void Dock::ComputeMetrics() {
    edge_ = ParseEdge(cfg_.position.c_str());
    const HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);

    Metrics& m = m_;
    m.scale = dpiX / 96.0;
    const double s = m.scale;
    m.icon = std::round(cfg_.iconSize * s);
    m.maxIcon = cfg_.magnification ? std::max(m.icon, std::round(cfg_.magnifiedSize * s)) : m.icon;
    m.gap = cfg_.spacing * s;
    m.padSide = cfg_.shelfPadding * s;
    m.padFar = std::round(std::max(4.0 * s, m.icon * 0.1));
    m.padNear = m.padFar + std::round(6 * s);
    m.dot = std::round(4 * s);
    m.margin = std::round(6 * s);
    m.labelH = std::round(24 * s);
    m.labelMaxW = std::round(240 * s);
    m.labelGap = std::round(8 * s);
    m.bounce = m.icon * 0.55;
    m.shelfH = m.padFar + m.icon + m.padNear;
    m.radius = std::min(cfg_.cornerRadius * s, m.shelfH / 2);
    m.iconNear = m.margin + m.padNear;

    const double labelDepth = edge_ == Edge::Bottom ? m.labelH : m.labelMaxW;
    const double iconReach = std::max(m.maxIcon, m.icon + 1.5 * m.bounce);
    m.depth = std::ceil(m.iconNear + iconReach + m.labelGap + labelDepth + 4 * s);
}

void Dock::PositionWindow(bool force) {
    const HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const int thickness = (cfg_.reserveSpace && !cfg_.autoHide) ? static_cast<int>(std::ceil(m_.margin + m_.shelfH)) : 0;
    const RECT bar = appBar_.Place(edge_, thickness, mi.rcMonitor, force);
    const bool barOk = (bar.left | bar.top | bar.right | bar.bottom) != 0;  // false if the AppBar is not registered
    GetMonitorInfoW(mon, &mi);  // the work area may have changed
    const RECT& work = mi.rcWork;

    const int depth = static_cast<int>(m_.depth);
    int x = 0, y = 0, w = 0, h = 0;
    switch (edge_) {
        case Edge::Left:
            x = barOk ? bar.left : work.left;
            y = work.top;
            w = depth;
            h = work.bottom - work.top;
            break;
        case Edge::Right:
            x = (barOk ? bar.right : work.right) - depth;
            y = work.top;
            w = depth;
            h = work.bottom - work.top;
            break;
        case Edge::Bottom:
        default:
            x = work.left;
            y = (barOk ? bar.bottom : work.bottom) - depth;
            w = work.right - work.left;
            h = depth;
            break;
    }
    m_.winX = x;
    m_.winY = y;
    m_.winW = w;
    m_.winH = h;
    m_.axis = edge_ == Edge::Bottom ? w : h;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    region_ = {-1, -1, -1, -1};
    regionEmpty_ = false;
}

void Dock::BuildChrome() {
    const auto& c = gfx_.Comp();
    const auto root = gfx_.Root();

    shadow_ = c.CreateSpriteVisual();
    shelf_ = c.CreateContainerVisual();
    shelfGeom_ = c.CreateRoundedRectangleGeometry();
    shelf_.Clip(c.CreateGeometricClip(shelfGeom_));
    backdrop_ = c.CreateSpriteVisual();
    backdrop_.RelativeSizeAdjustment({1.0f, 1.0f});
    backdrop_.Brush(c.CreateHostBackdropBrush());
    tint_ = c.CreateSpriteVisual();
    tint_.RelativeSizeAdjustment({1.0f, 1.0f});
    shelf_.Children().InsertAtTop(backdrop_);
    shelf_.Children().InsertAtTop(tint_);

    border_ = c.CreateShapeVisual();
    borderGeom_ = c.CreateRoundedRectangleGeometry();
    borderShape_ = c.CreateSpriteShape(borderGeom_);
    border_.Shapes().Append(borderShape_);

    dots_ = c.CreateContainerVisual();
    iconLayer_ = c.CreateContainerVisual();
    label_ = c.CreateSpriteVisual();
    label_.IsVisible(false);

    root.Children().InsertAtTop(shadow_);
    root.Children().InsertAtTop(shelf_);
    root.Children().InsertAtTop(border_);
    root.Children().InsertAtTop(dots_);
    root.Children().InsertAtTop(iconLayer_);
    root.Children().InsertAtTop(label_);
}

void Dock::BuildShadow() {
    if (!cfg_.shadow) {
        shadow_.IsVisible(false);
        shadowExtent_ = 0;
        return;
    }
    const ShadowBrush sb = gfx_.CreateShadow(F(m_.radius), F(5 * m_.scale));
    shadow_.Brush(sb.brush);
    shadow_.Opacity(F(cfg_.shadowOpacity));
    shadow_.IsVisible(true);
    shadowExtent_ = sb.extent;
}

void Dock::ApplyTheme() {
    const auto& c = gfx_.Comp();
    const bool dark = Dark();
    const uint32_t tint = cfg_.glassTint.value_or(dark ? 0x282828u : 0xF4F4F4u);
    const double opacity = cfg_.blur ? cfg_.glassOpacity : std::max(0.88, cfg_.glassOpacity);
    tint_.Brush(c.CreateColorBrush(Rgba(static_cast<uint8_t>(tint >> 16), static_cast<uint8_t>(tint >> 8),
                                        static_cast<uint8_t>(tint), static_cast<uint8_t>(opacity * 255))));
    backdrop_.IsVisible(cfg_.blur);
    borderShape_.StrokeBrush(c.CreateColorBrush(dark ? Rgba(255, 255, 255, 46) : Rgba(255, 255, 255, 150)));
    borderShape_.StrokeThickness(F(m_.scale));
    border_.IsVisible(cfg_.border);

    const auto dotBrush = c.CreateColorBrush(dark ? Rgba(255, 255, 255, 210) : Rgba(0, 0, 0, 170));
    for (auto& it : items_) it->dotVisual.Brush(dotBrush);

    labelFormat_ = nullptr;
    if (SUCCEEDED(gfx_.DWrite()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                  F(13 * m_.scale), L"", labelFormat_.put()))) {
        // Long names end in "…" instead of wrapping.
        labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        winrt::com_ptr<IDWriteInlineObject> ellipsis;
        gfx_.DWrite()->CreateEllipsisTrimmingSign(labelFormat_.get(), ellipsis.put());
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        labelFormat_->SetTrimming(&trimming, ellipsis.get());
    }
    labelItemId_ = 0;
    label_.IsVisible(false);
}

void Dock::Rebuild(bool forceBar) {
    ComputeMetrics();
    PositionWindow(forceBar);
    BuildShadow();
    ApplyTheme();
    for (auto& it : items_) RequestIcon(*it);
    if (IsWindowVisible(edgeWnd_)) PlaceEdgeTrigger();
    ghostSize_ = 0;  // re-created at the new size on the next drag
    ApplyLayout();
    UpdateRegion();
}

// ---------------------------------------------------------------- Items

void Dock::LoadPinned() {
    if (!cfg_.pinnedInitialized) {
        cfg_.pinned = DefaultPinnedApps();
        cfg_.pinnedInitialized = true;
        SaveConfig(cfg_);
    }
    for (const auto& p : cfg_.pinned) AddItem(true, p.path, p.aumid, false);
}

DockItem& Dock::AddItem(bool pinned, std::wstring path, std::wstring aumid, bool animate) {
    auto item = std::make_unique<DockItem>();
    DockItem& it = *item;
    it.id = nextId_++;
    it.pinned = pinned;
    it.path = std::move(path);
    it.aumid = std::move(aumid);

    if (EndsWith(it.path, L".lnk")) {
        const ShortcutInfo sc = ReadShortcut(it.path);
        it.matchExe = Lower(sc.target);
        if (it.aumid.empty()) it.aumid = sc.aumid;
    } else if (EndsWith(it.path, L".exe")) {
        it.matchExe = Lower(it.path);
    }
    it.matchAumid = Lower(it.aumid);
    it.iconSource = !it.path.empty() ? it.path : L"shell:AppsFolder\\" + it.aumid;

    it.presence.damping = 1.0;
    it.presence.response = 0.35;
    it.presence.target = 1;
    it.presence.value = animate ? 0 : 1;
    it.dot.response = 0.25;
    it.shiftU.response = 0.3;
    it.shiftV.response = 0.3;

    CreateItemVisuals(it);
    items_.push_back(std::move(item));
    RequestIcon(it);
    return it;
}

void Dock::CreateItemVisuals(DockItem& it) {
    const auto& c = gfx_.Comp();
    it.root = c.CreateContainerVisual();
    it.icon = c.CreateSpriteVisual();
    it.icon.RelativeSizeAdjustment({1.0f, 1.0f});
    it.dim = c.CreateSpriteVisual();
    it.dim.RelativeSizeAdjustment({1.0f, 1.0f});
    it.dim.Opacity(0);
    it.root.Children().InsertAtTop(it.icon);
    it.root.Children().InsertAtTop(it.dim);
    iconLayer_.Children().InsertAtTop(it.root);

    const float d = F(m_.dot);
    it.dotVisual = c.CreateSpriteVisual();
    it.dotVisual.Size({d, d});
    const auto ellipse = c.CreateEllipseGeometry();
    ellipse.Center({d / 2, d / 2});
    ellipse.Radius({d / 2, d / 2});
    it.dotVisual.Clip(c.CreateGeometricClip(ellipse));
    it.dotVisual.Brush(c.CreateColorBrush(Dark() ? Rgba(255, 255, 255, 210) : Rgba(0, 0, 0, 170)));
    it.dotVisual.Opacity(0);
    dots_.Children().InsertAtTop(it.dotVisual);
}

void Dock::RequestIcon(DockItem& it) {
    const auto cached = LoadCachedIcon(gfx_.Wic(), it.iconSource);
    if (cached) SetIconBitmap(it, cached.get());
    icons_.Request({it.id, it.iconSource, it.matchExe.empty() ? it.path : it.matchExe, !cached});
}

void Dock::SetIconBitmap(DockItem& it, IWICBitmap* bmp) {
    const int px = static_cast<int>(std::ceil(m_.maxIcon));
    const auto surface = gfx_.CreateSurface(px, px);
    gfx_.Draw(surface, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        if (FAILED(dc->CreateBitmapFromWicBitmap(bmp, nullptr, bitmap.put()))) return;
        const D2D1_RECT_F dest = D2D1::RectF(0, 0, F(px), F(px));
        dc->DrawBitmap(bitmap.get(), &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    });
    const auto& c = gfx_.Comp();
    const auto brush = c.CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::Fill);
    brush.BitmapInterpolationMode(wuc::CompositionBitmapInterpolationMode::Linear);
    it.icon.Brush(brush);

    // Press feedback: a black layer masked by the icon's own shape.
    const auto maskSource = c.CreateSurfaceBrush(surface);
    maskSource.Stretch(wuc::CompositionStretch::Fill);
    const auto mask = c.CreateMaskBrush();
    mask.Source(c.CreateColorBrush(Rgba(0, 0, 0)));
    mask.Mask(maskSource);
    it.dim.Brush(mask);
    it.hasIcon = true;
}

DockItem* Dock::FindItem(int id) {
    for (auto& it : items_)
        if (it->id == id) return it.get();
    return nullptr;
}

int Dock::IndexOf(int id) const {
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i]->id == id) return static_cast<int>(i);
    return -1;
}

// Moves an item in the list; the others glide to their new places from where they are now.
void Dock::Reorder(int from, int to) {
    if (from == to || from < 0 || to < 0) return;
    std::unordered_map<int, double> before;
    for (const auto& it : items_) before[it->id] = it->cu;
    auto moved = std::move(items_[from]);
    items_.erase(items_.begin() + from);
    items_.insert(items_.begin() + to, std::move(moved));
    const LayoutResult after = ComputeCurrentLayout();
    for (size_t i = 0; i < items_.size() && i < after.center.size(); ++i) {
        DockItem& it = *items_[i];
        if (dragging_ && it.id == pressedId_) continue;
        it.shiftU.value = before[it.id] - after.center[i];
        it.shiftU.target = 0;
    }
    StartAnimating();
}

void Dock::PinAt(const std::wstring& path, double u) {
    for (const auto& it : items_)
        if (it->pinned && Lower(it->path) == Lower(path)) return;  // already in the dock
    int index = static_cast<int>(items_.size());
    for (size_t i = 0; i < layout_.center.size() && i < items_.size(); ++i)
        if (u < layout_.center[i]) {
            index = static_cast<int>(i);
            break;
        }
    AddItem(true, path, L"", true);
    auto added = std::move(items_.back());
    items_.pop_back();
    items_.insert(items_.begin() + index, std::move(added));
    SavePinned();
    ScheduleRescan(false);
    StartAnimating();
}

void Dock::Rescan() {
    const auto windows = EnumAppWindows(GetCurrentProcessId());
    for (auto& it : items_) it->windows.clear();

    for (const AppWindow& w : windows) {
        if (w.hwnd == edgeWnd_) continue;
        const std::wstring aumid = Lower(w.aumid);
        const std::wstring exe = Lower(w.exe);
        DockItem* target = nullptr;
        if (!aumid.empty())
            for (auto& it : items_)
                if (it->matchAumid == aumid) { target = it.get(); break; }
        if (!target && !w.packaged && !exe.empty())
            for (auto& it : items_)
                if (it->matchExe == exe) { target = it.get(); break; }
        if (!target) {
            target = w.packaged ? &AddItem(false, L"", w.aumid, initialized_) : &AddItem(false, w.exe, w.aumid, initialized_);
        }
        target->windows.push_back(w.hwnd);
    }

    for (auto& it : items_) {
        const bool running = !it->windows.empty();
        it->dot.target = running ? 1 : 0;
        if (!initialized_) it->dot.Snap();
        if (dragging_ && it->id == pressedId_) continue;  // the drag decides about this one
        if (running) {
            if (it->removing) {
                it->removing = false;
                it->presence.target = 1;
            }
            if (it->bouncing && !it->attention) it->stopBounce = true;
        } else if (!it->pinned && !it->removing) {
            it->removing = true;
            it->presence.target = 0;
        }
    }
    initialized_ = true;
    StartAnimating();
}

void Dock::ScheduleRescan(bool alsoLate) {
    SetTimer(hwnd_, kTimerRescan, 40, nullptr);
    if (alsoLate) SetTimer(hwnd_, kTimerRescanLate, 900, nullptr);
}

void Dock::SavePinned() {
    cfg_.pinned.clear();
    for (const auto& it : items_)
        if (it->pinned && !it->removing) cfg_.pinned.push_back({it->path, it->aumid});
    cfg_.pinnedInitialized = true;
    SaveConfig(cfg_);
}

// ---------------------------------------------------------------- Interaction

int Dock::HitTest(double u) const {
    const size_t n = std::min(items_.size(), layout_.slotLeft.size());
    for (size_t i = 0; i < n; ++i)
        if (u >= layout_.slotLeft[i] && u < layout_.slotRight[i]) return static_cast<int>(i);
    return -1;
}

void Dock::OnMouseMove(int x, int y) {
    mouseX_ = x;
    mouseY_ = y;
    ToDock(edge_, x, y, m_.winW, m_.winH, mouseU_, mouseV_);
    if (!trackingLeave_) {
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&t);
        trackingLeave_ = true;
    }
    if (dragging_) {
        DragMove();
        return;
    }
    if (pressedId_ && std::hypot(x - pressX_, y - pressY_) > 6 * m_.scale) {
        BeginDrag();
        DragMove();
        return;
    }

    const double u = mouseU_, v = mouseV_;
    ApplyLayout();  // hit-test against the layout for this cursor position
    const int idx = HitTest(u);
    if (!hovering_) {
        const bool inShelf = hide_.target == 0 && u >= shelfLeft_ && u <= shelfRight_ && v >= 0 && v <= m_.margin + m_.shelfH;
        if (!inShelf) return;
        Log(L"enter u=%.0f v=%.0f", u, v);
        hovering_ = true;
        KillTimer(hwnd_, kTimerHide);
        magnify_.target = cfg_.magnification ? 1 : 0;
        UpdateRegion();
    } else if (!pressedId_) {
        // Stay "inside" up to the far side of the (magnified) icon under the cursor.
        const double reach = std::max(idx >= 0 ? items_[idx]->reach : 0.0, m_.margin + m_.shelfH);
        const bool inside = u >= shelfLeft_ - 2 && u <= shelfRight_ + 2 && v <= reach + 2 * m_.scale;
        if (!inside) {
            Log(L"leave (outside) u=%.0f v=%.0f shelf=%.0f..%.0f reach=%.0f", u, v, shelfLeft_, shelfRight_, reach);
            OnMouseLeave();
            return;
        }
    }
    hoverId_ = idx >= 0 ? items_[idx]->id : 0;
    ApplyLayout();
    StartAnimating();
}

void Dock::OnMouseLeave() {
    if (!hovering_) return;
    hovering_ = false;
    hoverId_ = 0;
    magnify_.target = 0;
    if (pressedId_ && !dragging_) {
        if (DockItem* it = FindItem(pressedId_)) it->dim.Opacity(0);
        pressedId_ = 0;
        KillTimer(hwnd_, kTimerHold);
        ReleaseCapture();
    }
    if (AutoHideActive() && !menuOpen_) SetTimer(hwnd_, kTimerHide, Ms(cfg_.hideDelay), nullptr);
    UpdateLabel();
    StartAnimating();
}

// Clicks use the cursor position right now, not the last mouse-move we processed.
void Dock::RefreshHover() {
    POINT p{};
    GetCursorPos(&p);
    ScreenToClient(hwnd_, &p);
    OnMouseMove(p.x, p.y);
    ApplyLayout();
    const int idx = hovering_ ? HitTest(mouseU_) : -1;
    hoverId_ = idx >= 0 ? items_[idx]->id : 0;
}

void Dock::OnLeftDown() {
    RefreshHover();
    if (!hoverId_ || dragging_) return;
    pressedId_ = hoverId_;
    pressX_ = mouseX_;
    pressY_ = mouseY_;
    if (DockItem* it = FindItem(pressedId_)) it->dim.Opacity(0.3f);
    SetCapture(hwnd_);
    SetTimer(hwnd_, kTimerHold, kHoldMs, nullptr);
    UpdateLabel();
}

void Dock::OnLeftUp() {
    KillTimer(hwnd_, kTimerHold);
    if (dragging_) {
        EndDrag();
        return;
    }
    if (!pressedId_) return;
    const int id = pressedId_;
    pressedId_ = 0;
    ReleaseCapture();
    DockItem* it = FindItem(id);
    if (!it) return;
    it->dim.Opacity(0);
    if (id == hoverId_) Activate(*it);
    UpdateLabel();
}

void Dock::OnRightUp() {
    if (dragging_) return;
    RefreshHover();
    if (!hovering_) return;
    // The strip under the icons (where the dots are) counts as the dock itself.
    const bool onIcon = hoverId_ && mouseV_ >= m_.iconNear - 2 * m_.scale;
    try {
        if (onIcon) ShowItemMenu(hoverId_);
        else ShowDockMenu();
    } catch (const winrt::hresult_error& e) {
        menuOpen_ = false;
        Log(L"menu failed: 0x%08X %s", static_cast<unsigned>(e.code().value), e.message().c_str());
    }
}

// Click and hold: App Exposé for a running app, the menu otherwise.
void Dock::OnHoldTimer() {
    if (!pressedId_ || dragging_) return;
    const int id = pressedId_;
    pressedId_ = 0;
    ReleaseCapture();
    DockItem* it = FindItem(id);
    if (!it) return;
    it->dim.Opacity(0);
    if (!it->windows.empty()) ShowExpose(id);
    else ShowItemMenu(id);
}

void Dock::Activate(DockItem& it) {
    if (!it.windows.empty()) {
        const HWND w = it.windows.front();
        if (GetForegroundWindow() == w && !IsIconic(w)) return;  // macOS: clicking the active app does nothing
        ActivateWindow(w);
        return;
    }
    LaunchApp(it.path, it.aumid);
    if (cfg_.launchBounce) StartBounce(it, false);
    ScheduleRescan(true);
}

void Dock::StartBounce(DockItem& it, bool attention) {
    if (it.bouncing) return;
    it.bouncing = true;
    it.attention = attention;
    it.stopBounce = false;
    it.bounceTime = 0;
    it.bounceCycles = 0;
    UpdateRegion();
    StartAnimating();
}

// ---------------------------------------------------------------- Menus & App Exposé

POINT Dock::ItemAnchor(const DockItem& it) const {
    const WinRect r = Map(it.cu, it.reach + 6 * m_.scale, 0, 0);
    return {m_.winX + static_cast<LONG>(r.x), m_.winY + static_cast<LONG>(r.y)};
}

RECT Dock::ItemScreenRect(const DockItem& it) const {
    const WinRect r = Map(it.cu - it.size / 2, it.reach - it.size, it.size, it.size);
    return {m_.winX + static_cast<LONG>(r.x), m_.winY + static_cast<LONG>(r.y), m_.winX + static_cast<LONG>(r.x + r.w),
            m_.winY + static_cast<LONG>(r.y + r.h)};
}

void Dock::BeforePopup() {
    menuOpen_ = true;
    KillTimer(hwnd_, kTimerHide);
    OnMouseLeave();
}

void Dock::AfterPopup() {
    menuOpen_ = false;
    if (AutoHideActive() && !hovering_) SetTimer(hwnd_, kTimerHide, Ms(cfg_.hideDelay), nullptr);
}

void Dock::ShowItemMenu(int itemId) {
    DockItem* it = FindItem(itemId);
    if (!it) return;
    const bool shift = GetKeyState(VK_SHIFT) < 0;
    const MenuSide side = edge_ == Edge::Left ? MenuSide::Right : edge_ == Edge::Right ? MenuSide::Left : MenuSide::Above;

    std::vector<MenuItem> items;
    std::vector<std::wstring> titles;
    for (size_t i = 0; i < it->windows.size() && i < 20; ++i) {
        std::wstring title = WindowTitle(it->windows[i]);
        titles.push_back(title.empty() ? it->name : title);
        items.push_back(Item(titles.back().c_str(), kCmdWindow + static_cast<UINT>(i)));
    }
    if (!items.empty()) items.push_back(MenuItem::Separator());
    MenuItem options = Item(Tr(Text::Options), 0);
    options.sub.push_back(Item(Tr(Text::KeepInDock), kCmdKeep, it->pinned));
    if (!it->path.empty()) options.sub.push_back(Item(Tr(Text::ShowInExplorer), kCmdShow));
    items.push_back(options);
    if (!it->windows.empty()) items.push_back(Item(Tr(Text::ShowAllWindows), kCmdExpose));
    items.push_back(MenuItem::Separator());
    if (!it->windows.empty()) {
        items.push_back(Item(Tr(Text::Hide), kCmdHide));
        items.push_back(shift ? Item(Tr(Text::ForceQuit), kCmdForceQuit) : Item(Tr(Text::Quit), kCmdClose));
    } else {
        items.push_back(Item(Tr(Text::Open), kCmdOpen));
    }

    const POINT anchor = ItemAnchor(*it);
    BeforePopup();
    const UINT cmd = MacMenu::Run(gfx_, items, anchor, side, Dark(), m_.scale);
    AfterPopup();

    it = FindItem(itemId);  // the item list may have changed while the menu was open
    if (!it || !cmd) return;
    switch (cmd) {
        case kCmdKeep:
            it->pinned = !it->pinned;
            if (!it->pinned && it->windows.empty()) {
                it->removing = true;
                it->presence.target = 0;
            }
            SavePinned();
            StartAnimating();
            break;
        case kCmdShow: ShowInExplorer(it->path); break;
        case kCmdExpose: ShowExpose(itemId); break;
        case kCmdHide: MinimizeWindows(it->windows); break;
        case kCmdClose: CloseWindows(it->windows); break;
        case kCmdForceQuit: ForceQuitWindows(it->windows); break;
        case kCmdOpen: Activate(*it); break;
        default:
            if (cmd >= kCmdWindow && cmd - kCmdWindow < it->windows.size()) ActivateWindow(it->windows[cmd - kCmdWindow]);
            break;
    }
}

void Dock::ShowDockMenu() {
    const MenuSide side = edge_ == Edge::Left ? MenuSide::Right : edge_ == Edge::Right ? MenuSide::Left : MenuSide::Above;
    const WinRect r = Map(mouseU_, m_.margin + m_.shelfH + 6 * m_.scale, 0, 0);
    const POINT anchor{m_.winX + static_cast<LONG>(r.x), m_.winY + static_cast<LONG>(r.y)};

    std::vector<MenuItem> items;
    items.push_back(Item(Tr(Text::AutoHide), kCmdAutoHide, cfg_.autoHide));
    items.push_back(Item(Tr(Text::Magnification), kCmdMagnify, cfg_.magnification));
    MenuItem pos = Item(Tr(Text::Position), 0);
    pos.sub.push_back(Item(Tr(Text::PositionLeft), kCmdPosLeft, edge_ == Edge::Left));
    pos.sub.push_back(Item(Tr(Text::PositionBottom), kCmdPosBottom, edge_ == Edge::Bottom));
    pos.sub.push_back(Item(Tr(Text::PositionRight), kCmdPosRight, edge_ == Edge::Right));
    items.push_back(pos);
    items.push_back(MenuItem::Separator());
    items.push_back(Item(Tr(Text::HideTaskbar), kCmdHideTaskbar, cfg_.hideTaskbar));
    items.push_back(Item(Tr(Text::DarkTheme), kCmdDark, Dark()));
    items.push_back(MenuItem::Separator());
    items.push_back(Item(Tr(Text::QuitDock), kCmdQuitDock));

    BeforePopup();
    const UINT cmd = MacMenu::Run(gfx_, items, anchor, side, Dark(), m_.scale);
    AfterPopup();

    auto setPosition = [this](const char* p) {
        if (cfg_.position == p) return;
        cfg_.position = p;
        SaveConfig(cfg_);
        Rebuild(true);
    };
    switch (cmd) {
        case kCmdMagnify:
            cfg_.magnification = !cfg_.magnification;
            SaveConfig(cfg_);
            Rebuild(false);
            break;
        case kCmdDark:
            cfg_.theme = Dark() ? "light" : "dark";
            SaveConfig(cfg_);
            ApplyTheme();
            break;
        case kCmdAutoHide:
            cfg_.autoHide = !cfg_.autoHide;
            SaveConfig(cfg_);
            PositionWindow(true);  // reserve or release screen space
            SetHidden(AutoHideActive());
            ApplyLayout();
            break;
        case kCmdPosLeft: setPosition("left"); break;
        case kCmdPosBottom: setPosition("bottom"); break;
        case kCmdPosRight: setPosition("right"); break;
        case kCmdHideTaskbar:
            cfg_.hideTaskbar = !cfg_.hideTaskbar;
            SaveConfig(cfg_);
            ApplyTaskbarSetting();
            break;
        case kCmdQuitDock:
            DestroyWindow(hwnd_);
            break;
        default: break;
    }
}

void Dock::ShowExpose(int itemId) {
    DockItem* it = FindItem(itemId);
    if (!it || it->windows.empty()) return;
    const std::vector<HWND> windows = it->windows;
    const RECT icon = ItemScreenRect(*it);
    BeforePopup();
    const HWND chosen = Expose::Run(gfx_, windows, icon, edge_, Dark(), m_.scale);
    AfterPopup();
    if (chosen && IsWindow(chosen)) ActivateWindow(chosen);
}

// ---------------------------------------------------------------- Dragging icons

void Dock::EnsureGhost() {
    const int size = static_cast<int>(std::ceil(std::max(2.3 * m_.maxIcon, m_.maxIcon + 2 * (m_.labelGap + m_.labelH)) + 8 * m_.scale));
    if (ghost_.Hwnd() && size == ghostSize_) return;
    ghost_.Destroy();
    puffs_.clear();
    ghostSize_ = size;
    ghost_.Create(&gfx_, false, false, nullptr);
    ghost_.Style(Dark(), 0, F(m_.scale));
    ghost_.Place(-10000, -10000, ghostSize_, ghostSize_);

    // The poof cloud: soft white puffs drawn once.
    const auto& c = gfx_.Comp();
    const int puffPx = static_cast<int>(std::ceil(64 * m_.scale));
    const auto surface = gfx_.CreateSurface(puffPx, puffPx);
    gfx_.Draw(surface, [&](ID2D1DeviceContext* dc) {
        const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(1, 1, 1, 0.95f)},
                                            {0.55f, D2D1::ColorF(0.93f, 0.93f, 0.95f, 0.75f)},
                                            {1.0f, D2D1::ColorF(0.9f, 0.9f, 0.92f, 0.0f)}};
        winrt::com_ptr<ID2D1GradientStopCollection> collection;
        dc->CreateGradientStopCollection(stops, 3, collection.put());
        winrt::com_ptr<ID2D1RadialGradientBrush> brush;
        const float r = puffPx / 2.0f;
        dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(r, r), {}, r, r), collection.get(),
                                      brush.put());
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r, r), r, r), brush.get());
    });
    const auto puffBrush = c.CreateSurfaceBrush(surface);
    for (int i = 0; i <= kPuffs; ++i) {
        auto p = c.CreateSpriteVisual();
        p.Brush(puffBrush);
        p.IsVisible(false);
        ghost_.Content().Children().InsertAtTop(p);
        puffs_.push_back(p);
    }
    ghostIcon_ = c.CreateSpriteVisual();
    ghost_.Content().Children().InsertAtTop(ghostIcon_);
    ghostLabel_ = c.CreateSpriteVisual();
    ghostLabel_.IsVisible(false);
    ghost_.Content().Children().InsertAtTop(ghostLabel_);
}

void Dock::BeginDrag() {
    DockItem* it = FindItem(pressedId_);
    if (!it) return;
    KillTimer(hwnd_, kTimerHold);
    dragging_ = true;
    dragOutside_ = false;
    hovering_ = true;
    it->dim.Opacity(0);

    // Keep the cursor at the same spot inside the icon (direct manipulation).
    const WinRect ir = Map(it->cu - it->size / 2, m_.iconNear + it->bounceOffset + it->shiftV.value, it->size, it->size);
    grabX_ = pressX_ - ir.x;
    grabY_ = pressY_ - ir.y;
    dragSize_ = it->size;

    EnsureGhost();
    poofTime_ = -1;
    for (auto& p : puffs_) p.IsVisible(false);
    const float g = static_cast<float>(ghostSize_), s = F(dragSize_);
    ghostIcon_.Brush(it->icon.Brush());
    ghostIcon_.Size({s, s});
    ghostIcon_.Offset({(g - s) / 2, (g - s) / 2, 0});
    ghostIcon_.Opacity(1);
    ghostIcon_.IsVisible(true);
    float lw = 0;
    ghostLabel_.Brush(MakeBubble(Tr(Text::Remove), lw));
    ghostLabel_.Size({lw, F(m_.labelH)});
    ghostLabel_.Offset({(g - lw) / 2, (g - s) / 2 - F(m_.labelGap + m_.labelH), 0});
    ghostLabel_.IsVisible(false);
    PlaceGhost();
    ghost_.Show(false);
    UpdateLabel();
    UpdateRegion();
}

void Dock::PlaceGhost() {
    POINT p{static_cast<LONG>(mouseX_ - grabX_), static_cast<LONG>(mouseY_ - grabY_)};  // icon top-left, client
    ClientToScreen(hwnd_, &p);
    const int off = static_cast<int>((ghostSize_ - dragSize_) / 2);
    ghost_.MoveTo(p.x - off, p.y - off);
}

void Dock::DragMove() {
    DockItem* it = FindItem(pressedId_);
    if (!it) return;
    PlaceGhost();
    const double u = mouseU_, v = mouseV_;
    const bool outside = v > m_.margin + m_.shelfH + 0.75 * m_.icon || v < -0.5 * m_.icon ||
                         u < shelfLeft_ - m_.icon || u > shelfRight_ + m_.icon;
    if (outside != dragOutside_) {
        dragOutside_ = outside;
        magnify_.target = (!outside && cfg_.magnification) ? 1 : 0;
        // Outside: the gap closes (unless the app is running, then it stays in the dock anyway).
        it->presence.target = (outside && it->windows.empty()) ? 0 : 1;
        ghostLabel_.IsVisible(outside);
    }
    if (!outside) {
        const int target = HitTest(u);
        const int from = IndexOf(it->id);
        if (target >= 0 && target != from) Reorder(from, target);
    }
    ApplyLayout();
    StartAnimating();
}

void Dock::EndDrag() {
    DockItem* it = FindItem(pressedId_);
    dragging_ = false;
    pressedId_ = 0;
    ReleaseCapture();
    if (!it) {
        ghost_.Hide();
        return;
    }
    // Where the dragged icon is now, in dock space.
    double gu = 0, gv = 0;
    ToDock(edge_, mouseX_ - grabX_ + dragSize_ / 2, mouseY_ - grabY_ + dragSize_ / 2, m_.winW, m_.winH, gu, gv);
    const double ghostNear = gv - dragSize_ / 2;
    const int idx = IndexOf(it->id);

    auto flyBack = [&] {
        // The icon glides from where it was dropped into its slot.
        if (idx >= 0 && idx < static_cast<int>(layout_.center.size())) {
            it->shiftU.value = gu - layout_.center[idx];
            it->shiftV.value = ghostNear - m_.iconNear;
        }
        ghost_.Hide();
    };

    if (dragOutside_) {
        it->pinned = false;
        if (it->windows.empty()) {
            it->removing = true;
            it->presence.target = 0;
            StartPoof();
        } else {
            it->presence.target = 1;
            flyBack();
        }
    } else {
        it->pinned = true;  // dragging an app into the dock keeps it there
        flyBack();
    }
    dragOutside_ = false;
    SavePinned();
    magnify_.target = cfg_.magnification ? 1 : 0;
    OnMouseMove(mouseX_, mouseY_);  // decides whether we are still hovering
    StartAnimating();
}

void Dock::StartPoof() {
    poofTime_ = 0;
    ghostIcon_.IsVisible(false);
    ghostLabel_.IsVisible(false);
    for (auto& p : puffs_) p.IsVisible(true);
    UpdatePoof(0);
    StartAnimating();
}

// The macOS "poof": a little cloud that bursts outward and fades.
void Dock::UpdatePoof(double t) {
    const double g = ghostSize_ / 2.0;
    const double ease = 1 - std::pow(1 - t, 3);
    for (int i = 0; i <= kPuffs; ++i) {
        const bool center = i == kPuffs;
        const double angle = 2 * std::numbers::pi * i / kPuffs + 0.4;
        const double dist = center ? 0 : (0.12 + 0.5 * ease) * dragSize_;
        const double size = dragSize_ * (center ? 0.55 + 0.45 * ease : (i % 2 ? 0.38 : 0.48) + 0.3 * ease);
        const double x = g + std::cos(angle) * dist - size / 2;
        const double y = g + std::sin(angle) * dist - size / 2;
        puffs_[i].Size({F(size), F(size)});
        puffs_[i].Offset({F(x), F(y), 0});
        puffs_[i].Opacity(F(0.9 * std::pow(1 - t, 1.6)));
    }
}

// ---------------------------------------------------------------- Files dropped from Explorer

bool Dock::CanPin(const std::vector<std::wstring>& files) const {
    if (files.size() != 1) return false;
    const std::wstring& f = files[0];
    const DWORD attrs = GetFileAttributesW(f.c_str());
    return EndsWith(f, L".exe") || EndsWith(f, L".lnk") ||
           (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
}

void Dock::SetDropHighlight(int itemId) {
    if (itemId == dropHighlightId_) return;
    if (DockItem* old = FindItem(dropHighlightId_)) old->dim.Opacity(0);
    dropHighlightId_ = itemId;
    if (DockItem* it = FindItem(itemId)) it->dim.Opacity(0.25f);
}

DWORD Dock::DropOver(POINT screen) {
    POINT c = screen;
    ScreenToClient(hwnd_, &c);
    mouseX_ = c.x;
    mouseY_ = c.y;
    ToDock(edge_, c.x, c.y, m_.winW, m_.winH, mouseU_, mouseV_);
    if (!hovering_) {
        hovering_ = true;
        KillTimer(hwnd_, kTimerHide);
        magnify_.target = cfg_.magnification ? 1 : 0;
        UpdateRegion();
    }
    ApplyLayout();
    StartAnimating();
    const int idx = mouseV_ <= m_.margin + m_.shelfH + m_.maxIcon ? HitTest(mouseU_) : -1;
    DockItem* it = idx >= 0 ? items_[idx].get() : nullptr;
    SetDropHighlight(it ? it->id : 0);
    hoverId_ = it ? it->id : 0;
    UpdateLabel();
    if (it) return DROPEFFECT_COPY;                    // open the files with this app
    if (CanPin(dropFiles_)) return DROPEFFECT_LINK;    // add the app/folder to the dock
    return DROPEFFECT_NONE;
}

void Dock::DropLeave() {
    SetDropHighlight(0);
    dropFiles_.clear();
    OnMouseLeave();
}

DWORD Dock::DropFiles(const std::vector<std::wstring>& files, POINT screen) {
    dropFiles_ = files;
    const DWORD effect = DropOver(screen);
    if (effect == DROPEFFECT_COPY) {
        if (DockItem* it = FindItem(dropHighlightId_)) {
            const std::wstring exe = !it->matchExe.empty() ? it->matchExe : it->path;
            OpenFilesWith(exe, it->path.empty() ? it->aumid : L"", files);
            if (it->windows.empty() && cfg_.launchBounce) StartBounce(*it, false);
            ScheduleRescan(true);
        }
    } else if (effect == DROPEFFECT_LINK) {
        PinAt(files[0], mouseU_);
    }
    DropLeave();
    return effect;
}

// ---------------------------------------------------------------- Hiding

bool Dock::AutoHideActive() const { return cfg_.autoHide || fullscreen_; }

void Dock::SetHidden(bool hidden) {
    const double target = hidden ? 1 : 0;
    if (hide_.target == target) return;
    hide_.target = target;
    if (!hidden) {
        ShowWindow(edgeWnd_, SW_HIDE);
    } else {
        KillTimer(hwnd_, kTimerHide);
    }
    UpdateLabel();
    UpdateRegion();
    StartAnimating();
}

void Dock::OnEdgeHover() {
    if (!AutoHideActive() || hide_.target == 0 || showPending_) return;
    if (cfg_.showDelay > 0) {
        showPending_ = true;
        SetTimer(hwnd_, kTimerShow, Ms(cfg_.showDelay), nullptr);
        return;
    }
    SetHidden(false);
    // Hide again if the mouse never actually moves onto the dock.
    SetTimer(hwnd_, kTimerHide, Ms(std::max(cfg_.hideDelay, 0.8)), nullptr);
}

void Dock::UpdateFullscreen() {
    const bool fs = cfg_.hideInFullscreen && IsFullscreenOnPrimary(hwnd_);
    if (fs == fullscreen_) return;
    fullscreen_ = fs;
    Log(L"fullscreen: %d", fs ? 1 : 0);
    if (AutoHideActive()) {
        if (!hovering_) SetHidden(true);
    } else {
        SetHidden(false);
    }
}

void Dock::ApplyTaskbarSetting() {
    if (!cfg_.hideTaskbar) {
        RestoreTaskbar();
        return;
    }
    if (cfg_.taskbarHiddenByDock) return;  // already hidden by us (e.g. after a crash)
    const UINT state = GetTaskbarState();
    if (state & ABS_AUTOHIDE) return;      // the user already auto-hides it
    cfg_.taskbarOriginalState = static_cast<int>(state);
    cfg_.taskbarHiddenByDock = true;
    SaveConfig(cfg_);  // saved first, so the watchdog can undo it if we crash
    SetTaskbarState(state | ABS_AUTOHIDE);
}

void Dock::RestoreTaskbar() {
    if (!cfg_.taskbarHiddenByDock) return;
    SetTaskbarState(static_cast<UINT>(cfg_.taskbarOriginalState));
    cfg_.taskbarHiddenByDock = false;
    SaveConfig(cfg_);
}

void Dock::RestShelfSpan(double& left, double& right) const {
    double total = 0;
    for (const auto& it : items_) total += (m_.icon + m_.gap) * std::clamp(it->presence.target, 0.0, 1.0);
    left = m_.axis / 2 - total / 2 + m_.gap / 2 - m_.padSide;
    right = m_.axis / 2 + total / 2 - m_.gap / 2 + m_.padSide;
}

void Dock::PlaceEdgeTrigger() {
    double l = 0, r = 0;
    RestShelfSpan(l, r);
    l = std::max(0.0, l - m_.icon);
    r = std::min(m_.axis, r + m_.icon);
    const WinRect w = Map(l, 0, r - l, 2);
    SetWindowPos(edgeWnd_, HWND_TOPMOST, m_.winX + static_cast<int>(w.x), m_.winY + static_cast<int>(w.y),
                 static_cast<int>(std::ceil(w.w)), static_cast<int>(std::ceil(w.h)), SWP_NOACTIVATE);
}

// ---------------------------------------------------------------- Frame

WinRect Dock::Map(double u, double v, double du, double dv) const {
    return ToWindow(edge_, {u, v, du, dv}, m_.winW, m_.winH);
}

void Dock::StartAnimating() {
    if (clock_.Running()) return;
    QueryPerformanceCounter(&lastTick_);
    animStart_ = lastTick_;
    animFrames_ = 0;
    clock_.Run();
}

void Dock::Tick() {
    if (!clock_.Running()) return;  // a tick that was already queued when the clock stopped
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = static_cast<double>(now.QuadPart - lastTick_.QuadPart) / static_cast<double>(qpcFreq_.QuadPart);
    lastTick_ = now;
    dt = std::clamp(dt, 0.0, 1.0 / 30) * cfg_.animationSpeed;
    const bool active = StepAnimations(dt);
    ApplyLayout();
    UpdateRegion();
    ++animFrames_;
    if (!active) {
        clock_.Halt();
        const double secs = static_cast<double>(now.QuadPart - animStart_.QuadPart) / static_cast<double>(qpcFreq_.QuadPart);
        if (secs > 0.2) Log(L"animation: %d frames in %.2f s = %.0f fps", animFrames_, secs, animFrames_ / secs);
        // Fully hidden: arm the screen-edge trigger.
        if (hide_.target == 1 && !IsWindowVisible(edgeWnd_)) {
            PlaceEdgeTrigger();
            ShowWindow(edgeWnd_, SW_SHOWNOACTIVATE);
        }
    }
}

bool Dock::StepAnimations(double dt) {
    bool active = false;
    auto step = [&](Spring& s, double eps) {
        s.Step(dt);
        if (s.Settled(eps)) s.Snap();
        else active = true;
    };
    magnify_.response = cfg_.magnifyResponse;
    step(magnify_, 0.001);
    step(hide_, 0.001);

    for (auto& p : items_) {
        DockItem& it = *p;
        step(it.presence, 0.002);
        step(it.dot, 0.002);
        step(it.shiftU, 0.05);
        step(it.shiftV, 0.05);
        if (it.bouncing) {
            active = true;
            const double period = it.attention ? kAttentionBouncePeriod : kLaunchBouncePeriod;
            const double height = m_.bounce * (it.attention ? 1.5 : 1.0);
            it.bounceTime += dt;
            const int cycle = static_cast<int>(it.bounceTime / period);
            if (cycle > it.bounceCycles) {
                it.bounceCycles = cycle;
                const bool done = it.attention ? cycle >= 3 : (it.stopBounce || it.bounceTime > kMaxLaunchBounce);
                if (done) {
                    it.bouncing = false;
                    it.bounceOffset = 0;
                    continue;
                }
            }
            const double phase = std::fmod(it.bounceTime, period) / period;
            it.bounceOffset = height * 4 * phase * (1 - phase);
        }
    }

    if (poofTime_ >= 0) {
        active = true;
        poofTime_ += dt;
        const double t = poofTime_ / kPoofDuration;
        if (t >= 1) {
            poofTime_ = -1;
            for (auto& p : puffs_) p.IsVisible(false);
            ghost_.Hide();
        } else {
            UpdatePoof(t);
        }
    }

    // Drop items that finished their exit animation.
    for (size_t i = 0; i < items_.size();) {
        DockItem& it = *items_[i];
        if (it.removing && it.presence.target == 0 && it.presence.value < 0.01) {
            iconLayer_.Children().Remove(it.root);
            dots_.Children().Remove(it.dotVisual);
            if (labelItemId_ == it.id) labelItemId_ = 0;
            if (dropHighlightId_ == it.id) dropHighlightId_ = 0;
            items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    return active;
}

LayoutResult Dock::ComputeCurrentLayout() const {
    std::vector<double> presence;
    presence.reserve(items_.size());
    for (const auto& it : items_) presence.push_back(it->presence.value);
    LayoutParams p;
    p.iconSize = m_.icon;
    p.gap = m_.gap;
    p.maxScale = m_.maxIcon / m_.icon;
    p.range = cfg_.magnificationRange;
    p.amount = magnify_.value;
    p.mouseX = mouseU_;
    p.centerX = m_.axis / 2.0;
    return ComputeLayout(presence, p);
}

void Dock::ApplyLayout() {
    layout_ = ComputeCurrentLayout();

    if (items_.empty()) {
        shelfLeft_ = m_.axis / 2.0 - 30 * m_.scale;
        shelfRight_ = m_.axis / 2.0 + 30 * m_.scale;
    } else {
        shelfLeft_ = layout_.start + m_.gap / 2 - m_.padSide;
        shelfRight_ = layout_.end - m_.gap / 2 + m_.padSide;
    }

    // Slide everything off the edge while auto-hidden.
    double ox = 0, oy = 0;
    EdgeOffset(edge_, -hide_.value * (m_.margin + m_.shelfH + shadowExtent_ + 4 * m_.scale), ox, oy);
    gfx_.Root().Offset({F(ox), F(oy), 0});

    const WinRect s = Map(shelfLeft_, m_.margin, shelfRight_ - shelfLeft_, m_.shelfH);
    const float l = F(s.x), t = F(s.y), w = F(s.w), h = F(s.h);
    const float r = F(m_.radius);
    shelf_.Offset({l, t, 0});
    shelf_.Size({w, h});
    shelfGeom_.Size({w, h});
    shelfGeom_.CornerRadius({r, r});
    border_.Offset({l, t, 0});
    border_.Size({w, h});
    borderGeom_.Offset({0.5f, 0.5f});
    borderGeom_.Size({w - 1, h - 1});
    borderGeom_.CornerRadius({std::max(0.0f, r - 0.5f), std::max(0.0f, r - 0.5f)});
    if (shadowExtent_ > 0) {
        const float e = F(shadowExtent_);
        shadow_.Offset({l - e, t - e + F(2 * m_.scale), 0});
        shadow_.Size({w + 2 * e, h + 2 * e});
    }

    const double d = m_.dot;
    for (size_t i = 0; i < items_.size(); ++i) {
        DockItem& it = *items_[i];
        const double pr = std::clamp(it.presence.value, 0.0, 1.0);
        it.size = layout_.size[i];
        it.cu = layout_.center[i] + it.shiftU.value;
        const double nearV = m_.iconNear + it.bounceOffset + it.shiftV.value;
        it.reach = nearV + it.size;
        const WinRect ir = Map(it.cu - it.size / 2, nearV, it.size, it.size);
        it.root.Offset({F(ir.x), F(ir.y), 0});
        it.root.Size({F(ir.w), F(ir.h)});
        it.root.Opacity((dragging_ && it.id == pressedId_) ? 0.0f : F(pr));  // the ghost shows it while dragging
        const WinRect dr = Map(layout_.center[i] - d / 2, m_.margin + m_.padNear / 2 - d / 2, d, d);
        it.dotVisual.Offset({F(dr.x), F(dr.y), 0});
        it.dotVisual.Opacity(cfg_.showDots ? F(it.dot.value * pr) : 0.0f);
    }
    UpdateLabel();
}

// A rounded label bubble with text (app names, "Remove").
wuc::CompositionSurfaceBrush Dock::MakeBubble(const std::wstring& text, float& width) const {
    const float s = F(m_.scale);
    const float padX = 11 * s;
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (!labelFormat_ || FAILED(gfx_.DWrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), labelFormat_.get(),
                                                                F(m_.labelMaxW) - 2 * padX, 400.0f, layout.put()))) {
        width = 0;
        return nullptr;
    }
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    width = std::ceil(std::min(tm.widthIncludingTrailingWhitespace, F(m_.labelMaxW) - 2 * padX) + 2 * padX);
    const float height = F(m_.labelH);
    const bool dark = Dark();
    const auto surface = gfx_.CreateSurface(static_cast<int>(width), static_cast<int>(height));
    gfx_.Draw(surface, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1SolidColorBrush> bg, edge, fg;
        dc->CreateSolidColorBrush(dark ? D2D1::ColorF(0.13f, 0.13f, 0.13f, 0.92f) : D2D1::ColorF(0.96f, 0.96f, 0.96f, 0.95f), bg.put());
        dc->CreateSolidColorBrush(dark ? D2D1::ColorF(1, 1, 1, 0.15f) : D2D1::ColorF(0, 0, 0, 0.12f), edge.put());
        dc->CreateSolidColorBrush(dark ? D2D1::ColorF(1, 1, 1, 0.95f) : D2D1::ColorF(0.08f, 0.08f, 0.08f, 1), fg.put());
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        const auto rr = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, width - 0.5f, height - 0.5f), 6 * s, 6 * s);
        dc->FillRoundedRectangle(rr, bg.get());
        dc->DrawRoundedRectangle(rr, edge.get(), 1.0f);
        dc->DrawTextLayout(D2D1::Point2F(padX, (height - tm.height) / 2), layout.get(), fg.get());
    });
    return gfx_.Comp().CreateSurfaceBrush(surface);
}

void Dock::UpdateLabel() {
    const int want = (hovering_ && cfg_.showLabels && !pressedId_ && hide_.target == 0) ? hoverId_ : 0;
    if (want != labelItemId_) {
        labelItemId_ = want;
        DockItem* it = want ? FindItem(want) : nullptr;
        if (!it || !labelFormat_) {
            labelItemId_ = 0;
            label_.IsVisible(false);
            return;
        }
        float width = 0;
        const auto brush = MakeBubble(it->name.empty() ? L"…" : it->name, width);
        if (!brush) return;
        label_.Brush(brush);
        label_.Size({width, F(m_.labelH)});
        labelW_ = width;
        label_.IsVisible(true);
    }
    if (labelItemId_) {
        if (const DockItem* it = FindItem(labelItemId_)) {
            // Along the edge the label is as long as its text (bottom) or as tall as one line (sides).
            const double du = edge_ == Edge::Bottom ? labelW_ : m_.labelH;
            const double dv = edge_ == Edge::Bottom ? m_.labelH : labelW_;
            const double u = std::clamp(it->cu - du / 2, 2.0, m_.axis - du - 2);
            const WinRect r = Map(u, it->reach + m_.labelGap, du, dv);
            label_.Offset({F(std::round(r.x)), F(std::round(r.y)), 0});
        }
    }
}

// The window only takes mouse input where the dock is; elsewhere clicks go to
// the windows underneath. While hovering it covers the magnification area.
void Dock::UpdateRegion() {
    if (hide_.target == 1 && hide_.Settled(0.001)) {
        if (regionEmpty_) return;
        regionEmpty_ = true;
        region_ = {};
        SetWindowRgn(hwnd_, CreateRectRgn(0, 0, 0, 0), TRUE);
        return;
    }
    RECT want{};
    bool moving = !magnify_.Settled(0.002) || !hide_.Settled(0.001);
    for (const auto& it : items_) moving = moving || !it->shiftU.Settled(0.05) || !it->shiftV.Settled(0.05);
    if (hovering_ || dragging_ || moving) {
        want = {0, 0, m_.winW, m_.winH};
    } else {
        const double e = shadowExtent_ + 8 * m_.scale;
        double reach = m_.margin + m_.shelfH + shadowExtent_;
        for (const auto& it : items_)
            if (it->bouncing) reach = std::max(reach, m_.iconNear + m_.icon + 1.5 * m_.bounce + 2);
        const WinRect r = Map(shelfLeft_ - e, 0, shelfRight_ - shelfLeft_ + 2 * e, reach);
        want = {static_cast<LONG>(std::floor(r.x)), static_cast<LONG>(std::floor(r.y)),
                static_cast<LONG>(std::ceil(r.x + r.w)), static_cast<LONG>(std::ceil(r.y + r.h))};
        want.left = std::max(want.left, 0L);
        want.top = std::max(want.top, 0L);
        want.right = std::min(want.right, static_cast<LONG>(m_.winW));
        want.bottom = std::min(want.bottom, static_cast<LONG>(m_.winH));
    }
    if (!regionEmpty_ && EqualRect(&want, &region_)) return;
    regionEmpty_ = false;
    region_ = want;
    HRGN rgn = CreateRectRgnIndirect(&want);
    if (!SetWindowRgn(hwnd_, rgn, TRUE)) DeleteObject(rgn);
}

// ---------------------------------------------------------------- Messages

LRESULT CALLBACK Dock::WndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        static_cast<Dock*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<Dock*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->WndProc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Dock::EdgeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE:
            if (g_dock) g_dock->OnEdgeHover();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void CALLBACK Dock::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!g_dock) return;
    if (event == EVENT_SYSTEM_FOREGROUND) {
        g_dock->UpdateFullscreen();
        return;
    }
    if (hwnd && idObject == OBJID_WINDOW && idChild == CHILDID_SELF && GetAncestor(hwnd, GA_ROOT) == hwnd)
        g_dock->ScheduleRescan(false);
}

LRESULT Dock::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_TICK:
            clock_.TickHandled();
            Tick();
            return 0;
        case WM_APP_ICON: {
            std::unique_ptr<IconResult> r(reinterpret_cast<IconResult*>(lp));
            if (DockItem* it = FindItem(r->id)) {
                if (!r->name.empty()) it->name = r->name;
                if (r->bitmap) SetIconBitmap(*it, r->bitmap.get());
                if (labelItemId_ == it->id) {
                    labelItemId_ = 0;
                    UpdateLabel();
                }
            }
            return 0;
        }
        case WM_APP_BAR:
            switch (wp) {
                case ABN_FULLSCREENAPP:
                    UpdateFullscreen();
                    break;
                case ABN_POSCHANGED:
                case ABN_STATECHANGE:
                    PositionWindow(true);
                    if (IsWindowVisible(edgeWnd_)) PlaceEdgeTrigger();
                    ApplyLayout();
                    UpdateRegion();
                    break;
                default:
                    break;
            }
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSELEAVE:
            Log(L"WM_MOUSELEAVE (hovering=%d pressed=%d)", hovering_ ? 1 : 0, pressedId_);
            trackingLeave_ = false;
            if (!pressedId_) OnMouseLeave();
            return 0;
        case WM_LBUTTONDOWN:
            OnLeftDown();
            return 0;
        case WM_LBUTTONUP:
            OnLeftUp();
            return 0;
        case WM_RBUTTONUP:
            OnRightUp();
            return 0;
        case WM_CAPTURECHANGED:
            // Someone took the mouse away mid-drag (e.g. Alt+Tab): finish the drag where it is.
            if (dragging_ && reinterpret_cast<HWND>(lp) != hwnd_) EndDrag();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_WINDOWPOSCHANGED:
            appBar_.WindowPosChanged();
            break;
        case WM_TIMER:
            KillTimer(hwnd_, wp);
            switch (wp) {
                case kTimerShow:
                    showPending_ = false;
                    SetHidden(false);
                    SetTimer(hwnd_, kTimerHide, Ms(std::max(cfg_.hideDelay, 0.8)), nullptr);
                    break;
                case kTimerHide:
                    if (AutoHideActive() && !hovering_ && !menuOpen_ && !dragging_) SetHidden(true);
                    break;
                case kTimerHold:
                    OnHoldTimer();
                    break;
                default:
                    Rescan();
                    break;
            }
            return 0;
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            Rebuild(true);
            return 0;
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETWORKAREA) Rebuild(false);
            break;
        case WM_DESTROY:
            Log(L"WM_DESTROY");
            PostQuitMessage(0);
            return 0;
        default:
            if (shellHookMsg_ && msg == shellHookMsg_) {
                if (wp == HSHELL_FLASH) {
                    const HWND w = reinterpret_cast<HWND>(lp);
                    for (auto& it : items_)
                        if (cfg_.attentionBounce && std::find(it->windows.begin(), it->windows.end(), w) != it->windows.end())
                            StartBounce(*it, true);
                } else if (wp == HSHELL_WINDOWCREATED) {
                    ScheduleRescan(true);
                } else if (wp == HSHELL_WINDOWDESTROYED || wp == HSHELL_WINDOWACTIVATED || wp == HSHELL_RUDEAPPACTIVATED) {
                    ScheduleRescan(false);
                    if (wp != HSHELL_WINDOWDESTROYED) UpdateFullscreen();
                }
                return 0;
            }
            if (taskbarCreatedMsg_ && msg == taskbarCreatedMsg_) {
                // Explorer restarted: register everything again.
                appBar_.Reset();
                appBar_.Register(hwnd_, WM_APP_BAR);
                RegisterShellHookWindow(hwnd_);
                Rebuild(true);
                ScheduleRescan(true);
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace dx
