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
#include "shell/screen.h"
#include "ui/layout.h"

namespace dx {

constexpr UINT WM_APP_TICK = WM_APP + 1;
constexpr UINT WM_APP_ICON = WM_APP + 2;
constexpr UINT WM_APP_BAR = WM_APP + 3;

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

    // Last layout, in dock space (u along the edge, v away from it)
    double cu = 0;
    double size = 0;
    double reach = 0;  // v of the icon side farthest from the screen edge
};

class Dock {
public:
    Dock() = default;
    Dock(const Dock&) = delete;
    Dock& operator=(const Dock&) = delete;
    ~Dock();

    bool Create(HINSTANCE instance);

private:
    // Sizes in physical pixels. "near" = toward the screen edge, "far" = away from it.
    struct Metrics {
        double scale = 1;
        double icon = 48, maxIcon = 84, gap = 6, padFar = 5, padNear = 10, padSide = 6;
        double radius = 16, margin = 6, labelH = 24, labelMaxW = 240, labelGap = 8, dot = 4, bounce = 24;
        double shelfH = 0;
        double iconNear = 0;  // v where icons start
        double depth = 0;     // window size across the edge
        double axis = 0;      // window size along the edge
        int winX = 0, winY = 0, winW = 0, winH = 0;
    };

    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EdgeProc(HWND, UINT, WPARAM, LPARAM);
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    // Setup
    void ComputeMetrics();
    void PositionWindow(bool force);
    void BuildChrome();
    void BuildShadow();
    void ApplyTheme();
    void Rebuild(bool forceBar);
    void CreateEdgeTrigger(HINSTANCE instance);
    void PlaceEdgeTrigger();

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
    int HitTest(double u) const;

    // Hiding
    [[nodiscard]] bool AutoHideActive() const;
    void SetHidden(bool hidden);
    void OnEdgeHover();
    void UpdateFullscreen();
    void ApplyTaskbarSetting();
    void RestoreTaskbar();

    // Frame
    void StartAnimating();
    void Tick();
    bool StepAnimations(double dt);
    void ApplyLayout();
    void UpdateLabel();
    void UpdateRegion();
    [[nodiscard]] WinRect Map(double u, double v, double du, double dv) const;
    void RestShelfSpan(double& left, double& right) const;

    HWND hwnd_ = nullptr;
    HWND edgeWnd_ = nullptr;
    UINT shellHookMsg_ = 0;
    UINT taskbarCreatedMsg_ = 0;
    HWINEVENTHOOK cloakHook_ = nullptr;
    HWINEVENTHOOK foregroundHook_ = nullptr;

    Config cfg_;
    Metrics m_;
    Edge edge_ = Edge::Bottom;
    Graphics gfx_;
    FrameClock clock_;
    IconLoader icons_;
    AppBar appBar_;
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
    bool menuOpen_ = false;
    bool fullscreen_ = false;
    bool showPending_ = false;
    double mouseU_ = 0, mouseV_ = 0;
    int hoverId_ = 0;
    int pressedId_ = 0;
    Spring magnify_;
    Spring hide_;  // 0 = shown, 1 = hidden off the edge
    LayoutResult layout_;
    double shelfLeft_ = 0, shelfRight_ = 0;
    RECT region_{};
    bool regionEmpty_ = false;

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
