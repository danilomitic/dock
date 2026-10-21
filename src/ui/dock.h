#pragma once
#include <windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "anim/spring.h"
#include "core/config.h"
#include "render/graphics.h"
#include "shell/apps.h"
#include "shell/icons.h"
#include "ui/layout.h"

namespace dx {

constexpr UINT WM_APP_TICK = WM_APP + 1;
constexpr UINT WM_APP_ICON = WM_APP + 2;

// Wakes the UI thread once per compositor frame, only while something animates.
// When nothing moves, the thread sleeps on an event: 0 % CPU.
class FrameClock {
public:
    FrameClock() = default;
    FrameClock(const FrameClock&) = delete;
    FrameClock& operator=(const FrameClock&) = delete;
    ~FrameClock() { Shutdown(); }

    void Init(HWND target);
    void Shutdown();
    void Run();   // start ticking
    void Halt();  // stop ticking
    void TickHandled() { pending_ = false; }
    [[nodiscard]] bool Running() const { return running_; }

private:
    void Loop();

    std::thread thread_;
    HANDLE wake_ = nullptr;
    HWND target_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
    std::atomic<bool> pending_{false};
};

struct DockItem {
    int id = 0;
    bool pinned = false;
    std::wstring path;   // launch target (.exe / .lnk), empty for Store apps
    std::wstring aumid;
    std::wstring matchExe;    // lower-case exe path used to match windows
    std::wstring matchAumid;  // lower-case AUMID used to match windows
    std::wstring iconSource;
    std::wstring name;
    std::vector<HWND> windows;  // Z order, most recent first

    bool removing = false;
    Spring presence;
    Spring dot;

    // Bounce
    bool bouncing = false;
    bool stopBounce = false;
    bool attention = false;
    double bounceTime = 0;
    int bounceCycles = 0;
    double bounceOffset = 0;

    // Visuals
    wuc::ContainerVisual root{nullptr};
    wuc::SpriteVisual icon{nullptr};
    wuc::SpriteVisual dim{nullptr};
    wuc::SpriteVisual dotVisual{nullptr};
    bool hasIcon = false;

    // Last layout
    double cx = 0;
    double size = 0;
    double top = 0;
};

class Dock {
public:
    Dock() = default;
    Dock(const Dock&) = delete;
    Dock& operator=(const Dock&) = delete;
    ~Dock();

    bool Create(HINSTANCE instance);

private:
    struct Metrics {
        double scale = 1;
        double icon = 48, maxIcon = 84, gap = 6, padTop = 5, padBottom = 10, padSide = 6;
        double radius = 16, marginBottom = 6, labelH = 26, labelGap = 10, dot = 4, bounce = 24;
        double shelfH = 0, shelfTop = 0, baseline = 0;
        int winW = 0, winH = 0;
    };

    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    // Setup
    void ComputeMetrics();
    void PositionWindow();
    void BuildChrome();
    void BuildShadow();
    void ApplyTheme();
    void Rebuild();

    // Items
    void LoadPinned();
    DockItem& AddItem(bool pinned, std::wstring path, std::wstring aumid, bool animate);
    void CreateItemVisuals(DockItem& it);
    void RequestIcon(DockItem& it);
    void SetIconBitmap(DockItem& it, IWICBitmap* bmp);
    void Rescan();
    void ScheduleRescan(bool alsoLate);
    void SavePinned();
    DockItem* FindItem(int id);
    int IndexOf(int id) const;

    // Interaction
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnLeftDown();
    void OnLeftUp();
    void OnRightUp(int x, int y);
    void Activate(DockItem& it);
    void StartBounce(DockItem& it, bool attention);
    void ShowContextMenu(DockItem* it, POINT screen);
    int HitTest(double x) const;

    // Frame
    void StartAnimating();
    void Tick();
    bool StepAnimations(double dt);
    void ApplyLayout();
    void UpdateLabel();
    void UpdateRegion();

    HWND hwnd_ = nullptr;
    UINT shellHookMsg_ = 0;
    UINT taskbarCreatedMsg_ = 0;
    HWINEVENTHOOK cloakHook_ = nullptr;

    Config cfg_;
    Metrics m_;
    Graphics gfx_;
    FrameClock clock_;
    IconLoader icons_;
    LARGE_INTEGER lastTick_{};
    LARGE_INTEGER qpcFreq_{};
    LARGE_INTEGER animStart_{};
    int animFrames_ = 0;

    std::vector<std::unique_ptr<DockItem>> items_;
    int nextId_ = 1;
    bool initialized_ = false;  // first scan done (later additions animate in)

    // Interaction state
    bool hovering_ = false;
    bool trackingLeave_ = false;
    double mouseX_ = 0, mouseY_ = 0;
    int hoverId_ = 0;
    int pressedId_ = 0;
    Spring magnify_;
    LayoutResult layout_;
    double shelfLeft_ = 0, shelfRight_ = 0;
    RECT region_{};

    // Visual tree
    wuc::SpriteVisual shadow_{nullptr};
    wuc::ContainerVisual shelf_{nullptr};
    wuc::CompositionRoundedRectangleGeometry shelfGeom_{nullptr};
    wuc::SpriteVisual backdrop_{nullptr};
    wuc::SpriteVisual tint_{nullptr};
    wuc::ShapeVisual border_{nullptr};
    wuc::CompositionRoundedRectangleGeometry borderGeom_{nullptr};
    wuc::CompositionSpriteShape borderShape_{nullptr};
    wuc::ContainerVisual dots_{nullptr};
    wuc::ContainerVisual iconLayer_{nullptr};
    wuc::SpriteVisual label_{nullptr};
    double shadowExtent_ = 0;
    int labelItemId_ = 0;
    double labelW_ = 0;
    winrt::com_ptr<IDWriteTextFormat> labelFormat_;
};

}  // namespace dx
