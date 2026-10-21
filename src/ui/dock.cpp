#include "ui/dock.h"

#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "core/strings.h"

namespace dx {
namespace {

Dock* g_dock = nullptr;

constexpr UINT_PTR kTimerRescan = 1;
constexpr UINT_PTR kTimerRescanLate = 2;

constexpr double kLaunchBouncePeriod = 0.62;
constexpr double kAttentionBouncePeriod = 0.5;
constexpr double kMaxLaunchBounce = 10.0;

float F(double v) { return static_cast<float>(v); }

bool EndsWith(const std::wstring& s, const wchar_t* suffix) { return Lower(s).ends_with(suffix); }

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
    if (cloakHook_) UnhookWinEvent(cloakHook_);
    icons_.Stop();
    clock_.Shutdown();
    if (hwnd_) {
        DeregisterShellHookWindow(hwnd_);
        DestroyWindow(hwnd_);
    }
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

    magnify_.damping = 1.0;
    ComputeMetrics();
    PositionWindow();
    BuildChrome();
    BuildShadow();
    ApplyTheme();

    icons_.Start(hwnd_, WM_APP_ICON);
    clock_.Init(hwnd_);

    LoadPinned();
    Rescan();
    ApplyLayout();
    UpdateRegion();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    shellHookMsg_ = RegisterWindowMessageW(L"SHELLHOOK");
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    RegisterShellHookWindow(hwnd_);
    cloakHook_ = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, WinEventProc, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    Log(L"DockX started: %d items, scale %.2f", static_cast<int>(items_.size()), m_.scale);
    return true;
}

void Dock::ComputeMetrics() {
    const HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);

    Metrics& m = m_;
    m.scale = dpiX / 96.0;
    const double s = m.scale;
    m.icon = std::round(cfg_.iconSize * s);
    m.maxIcon = cfg_.magnification ? std::max(m.icon, std::round(cfg_.magnifiedSize * s)) : m.icon;
    m.gap = cfg_.spacing * s;
    m.padSide = cfg_.shelfPadding * s;
    m.padTop = std::round(std::max(4.0 * s, m.icon * 0.1));
    m.padBottom = m.padTop + std::round(6 * s);
    m.dot = std::round(4 * s);
    m.marginBottom = std::round(6 * s);
    m.labelH = std::round(24 * s);
    m.labelGap = std::round(8 * s);
    m.bounce = m.icon * 0.55;
    m.shelfH = m.padTop + m.icon + m.padBottom;
    m.radius = std::min(cfg_.cornerRadius * s, m.shelfH / 2);

    const double grow = m.maxIcon - m.icon;
    const double bounceExtra = std::max(0.0, 1.5 * m.bounce - grow);
    m.winW = mi.rcWork.right - mi.rcWork.left;
    m.winH = static_cast<int>(std::ceil(m.marginBottom + m.shelfH + grow + bounceExtra + m.labelGap + m.labelH + 4 * s));
    m.shelfTop = m.winH - m.marginBottom - m.shelfH;
    m.baseline = m.shelfTop + m.padTop + m.icon;
}

void Dock::PositionWindow() {
    const HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    SetWindowPos(hwnd_, HWND_TOPMOST, mi.rcWork.left, mi.rcWork.bottom - m_.winH, m_.winW, m_.winH,
                 SWP_NOACTIVATE);
    region_ = {};
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

// A blurred rounded rectangle drawn once, stretched with a nine-grid brush.
void Dock::BuildShadow() {
    if (!cfg_.shadow) {
        shadow_.IsVisible(false);
        shadowExtent_ = 0;
        return;
    }
    const float sigma = F(5 * m_.scale);
    const float ext = 3 * sigma;
    const float rad = F(m_.radius);
    const float inset = ext + rad;
    const int n = static_cast<int>(std::ceil(2 * inset + 4));
    const auto surface = gfx_.CreateSurface(n, n);
    gfx_.Draw(surface, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1Bitmap1> shape;
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(n, n), nullptr, 0, props, shape.put()))) return;
        winrt::com_ptr<ID2D1Image> target;
        dc->GetTarget(target.put());
        D2D1_MATRIX_3X2_F transform;
        dc->GetTransform(&transform);
        dc->SetTarget(shape.get());
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        winrt::com_ptr<ID2D1SolidColorBrush> black;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), black.put());
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ext, ext, n - ext, n - ext), rad, rad), black.get());
        dc->SetTarget(target.get());
        dc->SetTransform(transform);
        winrt::com_ptr<ID2D1Effect> fx;
        if (FAILED(dc->CreateEffect(CLSID_D2D1Shadow, fx.put()))) return;
        fx->SetInput(0, shape.get());
        fx->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, sigma);
        dc->DrawImage(fx.get());
    });
    const auto& c = gfx_.Comp();
    const auto nine = c.CreateNineGridBrush();
    nine.Source(c.CreateSurfaceBrush(surface));
    nine.SetInsets(inset);
    shadow_.Brush(nine);
    shadow_.Opacity(F(cfg_.shadowOpacity));
    shadow_.IsVisible(true);
    shadowExtent_ = ext;
}

void Dock::ApplyTheme() {
    const auto& c = gfx_.Comp();
    const bool dark = cfg_.theme != "light";
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
    gfx_.DWrite()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, F(13 * m_.scale), L"", labelFormat_.put());
    labelItemId_ = 0;
    label_.IsVisible(false);
}

void Dock::Rebuild() {
    ComputeMetrics();
    PositionWindow();
    BuildShadow();
    ApplyTheme();
    for (auto& it : items_) RequestIcon(*it);
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
    const bool dark = cfg_.theme != "light";
    it.dotVisual.Brush(c.CreateColorBrush(dark ? Rgba(255, 255, 255, 210) : Rgba(0, 0, 0, 170)));
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

void Dock::Rescan() {
    const auto windows = EnumAppWindows(GetCurrentProcessId());
    for (auto& it : items_) it->windows.clear();

    for (const AppWindow& w : windows) {
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
        if (it->pinned) cfg_.pinned.push_back({it->path, it->aumid});
    cfg_.pinnedInitialized = true;
    SaveConfig(cfg_);
}

// ---------------------------------------------------------------- Interaction

int Dock::HitTest(double x) const {
    const size_t n = std::min(items_.size(), layout_.slotLeft.size());
    for (size_t i = 0; i < n; ++i)
        if (x >= layout_.slotLeft[i] && x < layout_.slotRight[i]) return static_cast<int>(i);
    return -1;
}

void Dock::OnMouseMove(int x, int y) {
    mouseX_ = x;
    mouseY_ = y;
    if (!trackingLeave_) {
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&t);
        trackingLeave_ = true;
    }
    const int idx = HitTest(x);
    if (!hovering_) {
        const bool inShelf = x >= shelfLeft_ && x <= shelfRight_ && y >= m_.shelfTop && y <= m_.shelfTop + m_.shelfH;
        if (!inShelf) return;
        Log(L"hover enter %d,%d", x, y);
        hovering_ = true;
        magnify_.target = cfg_.magnification ? 1 : 0;
        UpdateRegion();
    } else {
        // Stay "inside" up to the top of the (magnified) icon under the cursor.
        const double top = idx >= 0 ? std::min(items_[idx]->top, m_.shelfTop) : m_.shelfTop;
        const bool inside = x >= shelfLeft_ - 2 && x <= shelfRight_ + 2 && y >= top - 2 * m_.scale;
        if (!inside) {
            Log(L"hover exit (outside) %d,%d top=%.1f shelf=%.1f..%.1f", x, y, top, shelfLeft_, shelfRight_);
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
    if (pressedId_) {
        if (DockItem* it = FindItem(pressedId_)) it->dim.Opacity(0);
        pressedId_ = 0;
        ReleaseCapture();
    }
    UpdateLabel();
    StartAnimating();
}

void Dock::OnLeftDown() {
    if (!hoverId_) return;
    pressedId_ = hoverId_;
    if (DockItem* it = FindItem(pressedId_)) it->dim.Opacity(0.3f);
    SetCapture(hwnd_);
    UpdateLabel();
}

void Dock::OnLeftUp() {
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

void Dock::OnRightUp(int x, int y) {
    if (!hovering_) return;
    POINT pt{x, y};
    ClientToScreen(hwnd_, &pt);
    ShowContextMenu(hoverId_ ? FindItem(hoverId_) : nullptr, pt);
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

void Dock::ShowContextMenu(DockItem* it, POINT pt) {
    enum : UINT { kKeep = 1, kShow, kClose, kMagnify, kDark, kQuitDock, kWindow = 100 };
    HMENU menu = CreatePopupMenu();
    const int id = it ? it->id : 0;
    if (it) {
        for (size_t i = 0; i < it->windows.size() && i < 20; ++i) {
            std::wstring title = WindowTitle(it->windows[i]);
            if (title.empty()) title = it->name;
            AppendMenuW(menu, MF_STRING, kWindow + i, title.c_str());
        }
        if (!it->windows.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (it->pinned ? MF_CHECKED : 0), kKeep, Tr(Text::KeepInDock));
        if (!it->path.empty()) AppendMenuW(menu, MF_STRING, kShow, Tr(Text::ShowInExplorer));
        if (!it->windows.empty()) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kClose, Tr(Text::Quit));
        }
    } else {
        AppendMenuW(menu, MF_STRING | (cfg_.magnification ? MF_CHECKED : 0), kMagnify, Tr(Text::Magnification));
        AppendMenuW(menu, MF_STRING | (cfg_.theme != "light" ? MF_CHECKED : 0), kDark, Tr(Text::DarkTheme));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kQuitDock, Tr(Text::QuitDock));
    }

    SetForegroundWindow(hwnd_);
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN | TPM_CENTERALIGN, pt.x, pt.y, hwnd_, nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    OnMouseLeave();

    it = id ? FindItem(id) : nullptr;  // the item list may have changed
    switch (cmd) {
        case 0: break;
        case kKeep:
            if (!it) break;
            it->pinned = !it->pinned;
            if (it->pinned) {
                // Pinned apps come first: move it right after the last pinned one.
                const int from = IndexOf(it->id);
                int to = 0;
                for (size_t i = 0; i < items_.size(); ++i)
                    if (items_[i]->pinned && static_cast<int>(i) != from) to = static_cast<int>(i) + 1;
                if (to > from) --to;
                auto moved = std::move(items_[from]);
                items_.erase(items_.begin() + from);
                items_.insert(items_.begin() + to, std::move(moved));
            } else if (it->windows.empty()) {
                it->removing = true;
                it->presence.target = 0;
            }
            SavePinned();
            StartAnimating();
            break;
        case kShow:
            if (it) ShowInExplorer(it->path);
            break;
        case kClose:
            if (it)
                for (HWND w : it->windows) PostMessageW(w, WM_CLOSE, 0, 0);
            break;
        case kMagnify:
            cfg_.magnification = !cfg_.magnification;
            SaveConfig(cfg_);
            Rebuild();
            break;
        case kDark:
            cfg_.theme = cfg_.theme == "light" ? "dark" : "light";
            SaveConfig(cfg_);
            ApplyTheme();
            break;
        case kQuitDock:
            DestroyWindow(hwnd_);
            break;
        default:
            if (it && cmd >= kWindow && cmd - kWindow < it->windows.size()) ActivateWindow(it->windows[cmd - kWindow]);
            break;
    }
}

// ---------------------------------------------------------------- Frame

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

    for (auto& p : items_) {
        DockItem& it = *p;
        step(it.presence, 0.002);
        step(it.dot, 0.002);
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

    // Drop items that finished their exit animation.
    for (size_t i = 0; i < items_.size();) {
        DockItem& it = *items_[i];
        if (it.removing && it.presence.target == 0 && it.presence.value < 0.01) {
            iconLayer_.Children().Remove(it.root);
            dots_.Children().Remove(it.dotVisual);
            if (labelItemId_ == it.id) labelItemId_ = 0;
            items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    return active;
}

void Dock::ApplyLayout() {
    std::vector<double> presence;
    presence.reserve(items_.size());
    for (const auto& it : items_) presence.push_back(it->presence.value);

    LayoutParams p;
    p.iconSize = m_.icon;
    p.gap = m_.gap;
    p.maxScale = m_.maxIcon / m_.icon;
    p.range = cfg_.magnificationRange;
    p.amount = magnify_.value;
    p.mouseX = mouseX_;
    p.centerX = m_.winW / 2.0;
    layout_ = ComputeLayout(presence, p);

    if (items_.empty()) {
        shelfLeft_ = m_.winW / 2.0 - 30 * m_.scale;
        shelfRight_ = m_.winW / 2.0 + 30 * m_.scale;
    } else {
        shelfLeft_ = layout_.start + m_.gap / 2 - m_.padSide;
        shelfRight_ = layout_.end - m_.gap / 2 + m_.padSide;
    }

    const float l = F(shelfLeft_), t = F(m_.shelfTop), w = F(shelfRight_ - shelfLeft_), h = F(m_.shelfH);
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

    const float d = F(m_.dot);
    for (size_t i = 0; i < items_.size(); ++i) {
        DockItem& it = *items_[i];
        const double pr = std::clamp(it.presence.value, 0.0, 1.0);
        it.size = layout_.size[i];
        it.cx = layout_.center[i];
        it.top = m_.baseline - it.size - it.bounceOffset;
        it.root.Offset({F(it.cx - it.size / 2), F(it.top), 0});
        it.root.Size({F(it.size), F(it.size)});
        it.root.Opacity(F(pr));
        it.dotVisual.Offset({F(it.cx) - d / 2, F(m_.baseline + m_.padBottom / 2) - d / 2, 0});
        it.dotVisual.Opacity(cfg_.showDots ? F(it.dot.value * pr) : 0.0f);
    }
    UpdateLabel();
}

void Dock::UpdateLabel() {
    const int want = (hovering_ && cfg_.showLabels && !pressedId_) ? hoverId_ : 0;
    if (want != labelItemId_) {
        labelItemId_ = want;
        DockItem* it = want ? FindItem(want) : nullptr;
        if (!it || !labelFormat_) {
            labelItemId_ = 0;
            label_.IsVisible(false);
            return;
        }
        const std::wstring text = it->name.empty() ? L"…" : it->name;
        winrt::com_ptr<IDWriteTextLayout> layout;
        if (FAILED(gfx_.DWrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), labelFormat_.get(),
                                                   4000.0f, 400.0f, layout.put())))
            return;
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        const float s = F(m_.scale);
        const float padX = 11 * s;
        const float width = std::ceil(tm.widthIncludingTrailingWhitespace + 2 * padX);
        const float height = F(m_.labelH);
        const bool dark = cfg_.theme != "light";
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
        label_.Brush(gfx_.Comp().CreateSurfaceBrush(surface));
        label_.Size({width, height});
        labelW_ = width;
        label_.IsVisible(true);
    }
    if (labelItemId_) {
        if (const DockItem* it = FindItem(labelItemId_)) {
            const double x = std::clamp(it->cx - labelW_ / 2, 2.0, m_.winW - labelW_ - 2);
            const double y = it->top - m_.labelGap - m_.labelH;
            label_.Offset({F(std::round(x)), F(std::round(y)), 0});
        }
    }
}

// The window only takes mouse input where the dock is; elsewhere clicks go to
// the windows underneath. While hovering it covers the magnification area.
void Dock::UpdateRegion() {
    RECT want{};
    if (hovering_ || !magnify_.Settled(0.002)) {
        want = {0, 0, m_.winW, m_.winH};
    } else {
        const double e = shadowExtent_ + 8 * m_.scale;
        want = {static_cast<LONG>(std::floor(shelfLeft_ - e)), static_cast<LONG>(std::floor(m_.shelfTop - shadowExtent_)),
                static_cast<LONG>(std::ceil(shelfRight_ + e)), m_.winH};
        for (const auto& it : items_) {
            if (!it->bouncing) continue;
            const double top = m_.baseline - m_.icon - m_.bounce * 1.5 - 2;
            want.top = std::min(want.top, static_cast<LONG>(std::floor(top)));
        }
        want.left = std::max(want.left, 0L);
        want.right = std::min(want.right, static_cast<LONG>(m_.winW));
        want.top = std::max(want.top, 0L);
    }
    if (EqualRect(&want, &region_)) return;
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

void CALLBACK Dock::WinEventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (g_dock && hwnd && idObject == OBJID_WINDOW && idChild == CHILDID_SELF && GetAncestor(hwnd, GA_ROOT) == hwnd)
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
        case WM_MOUSEMOVE:
            OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSELEAVE:
            Log(L"WM_MOUSELEAVE");
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
            OnRightUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_TIMER:
            KillTimer(hwnd_, wp);
            Rescan();
            return 0;
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            Rebuild();
            return 0;
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETWORKAREA) Rebuild();
            break;
        case WM_DESTROY:
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
                }
                return 0;
            }
            if (taskbarCreatedMsg_ && msg == taskbarCreatedMsg_) {
                RegisterShellHookWindow(hwnd_);
                Rebuild();
                ScheduleRescan(true);
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace dx
