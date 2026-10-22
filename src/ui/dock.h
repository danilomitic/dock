#pragma once
#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "anim/spring.h"
#include "core/config.h"
#include "render/graphics.h"
#include "shell/apps.h"
#include "shell/droptarget.h"
#include "shell/icons.h"
#include "shell/screen.h"
#include "ui/layout.h"
#include "ui/popup.h"

namespace dx {

constexpr UINT WM_APP_TICK = WM_APP + 1;
constexpr UINT WM_APP_ICON = WM_APP + 2;
constexpr UINT WM_APP_BAR = WM_APP + 3;
constexpr UINT WM_APP_SHELL = WM_APP + 4;  // Recycle Bin changed

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
    enum class Kind { App, Stack, Trash };

    int id = 0;
    Kind kind = Kind::App;
    bool pinned = false;
    ULONGLONG lastUsed = 0;  // for the recent-apps section
    std::wstring path;   // launch target (.exe / .lnk / folder), empty for Store apps
    std::wstring aumid;
    std::wstring matchExe;    // lower-case exe path used to match windows
    std::wstring matchAumid;  // lower-case AUMID used to match windows
    std::wstring iconSource;
    std::wstring name;
    std::vector<HWND> windows;  // Z order, most recent first

    bool removing = false;
    Spring presence;
    Spring dot;
    Spring shiftU;  // smooth moves after a reorder or a drop (offset from the layout)
    Spring shiftV;

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
    void RegisterDropTargets();

    // Items
    void LoadPinned();
    DockItem& AddItem(bool pinned, std::wstring path, std::wstring aumid, bool animate,
                      DockItem::Kind kind = DockItem::Kind::App);
    int Section(const DockItem& it) const;  // 0 pinned apps, 1 other apps, 2 stacks + trash
    void ChangeOrder(const std::function<void()>& change);  // reorders with a smooth glide
    void NormalizeOrder();
    void UpdateTrash();
    void TrimRecent();
    void SaveRecent();
    void SaveStacks();
    void ShowStack(int itemId, bool held = false);
    bool AreApps(const std::vector<std::wstring>& files) const;
    void CreateItemVisuals(DockItem& it);
    void RequestIcon(DockItem& it);
    void SetIconBitmap(DockItem& it, IWICBitmap* bmp);
    void Rescan();
    void ScheduleRescan(bool alsoLate);
    void SavePinned();
    DockItem* FindItem(int id);
    int IndexOf(int id) const;
    void Reorder(int from, int to);
    void PinAt(const std::wstring& path, double u);

    // Interaction
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void RefreshHover();
    void OnLeftDown();
    void OnLeftUp();
    void OnRightUp();
    void OnHoldTimer();
    void Activate(DockItem& it);
    void StartBounce(DockItem& it, bool attention);
    int HitTest(double u, bool* separator = nullptr) const;  // item index, -1 if none
    [[nodiscard]] bool Dark() const { return cfg_.theme != "light"; }

    // Menus and App Exposé
    void BeforePopup();
    void AfterPopup();
    void ShowItemMenu(int itemId);
    void ShowDockMenu();
    void ShowExpose(int itemId);
    POINT ItemAnchor(const DockItem& it) const;
    RECT ItemScreenRect(const DockItem& it) const;

    // Dragging icons
    void EnsureGhost();
    void BeginDrag();
    void DragMove();
    void EndDrag();
    void PlaceGhost();
    void StartPoof();
    void UpdatePoof(double t);

    // Files dropped from Explorer
    DWORD DropOver(POINT screen);
    void DropLeave();
    DWORD DropFiles(const std::vector<std::wstring>& files, POINT screen);
    void SetDropHighlight(int itemId);
    bool CanPin(const std::vector<std::wstring>& files) const;

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
    LayoutResult ComputeCurrentLayout(std::vector<int>* entryItem = nullptr, std::vector<int>* itemEntry = nullptr,
                                      bool rest = false) const;
    void ApplyLayout();
    wuc::CompositionSurfaceBrush MakeBubble(const std::wstring& text, float& width) const;
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
    int mouseX_ = 0, mouseY_ = 0;      // client pixels
    double mouseU_ = 0, mouseV_ = 0;   // dock space
    int hoverId_ = 0;
    int pressedId_ = 0;
    int pressX_ = 0, pressY_ = 0;
    Spring magnify_;
    Spring hide_;  // 0 = shown, 1 = hidden off the edge
    LayoutResult layout_;
    std::vector<int> entryItem_;  // layout slot -> item index (-1 = separator)
    std::vector<int> itemEntry_;  // item index -> layout slot
    double shelfLeft_ = 0, shelfRight_ = 0;
    bool dragWasPinned_ = false;
    bool trashFull_ = false;
    ULONG shellNotify_ = 0;
    std::vector<wuc::SpriteVisual> separators_;
    RECT region_{};
    bool regionEmpty_ = false;

    // Icon drag
    bool dragging_ = false;
    bool dragOutside_ = false;
    double grabX_ = 0, grabY_ = 0;  // cursor offset inside the dragged icon
    double dragSize_ = 0;
    int ghostSize_ = 0;
    GlassPopup ghost_;
    wuc::SpriteVisual ghostIcon_{nullptr};
    wuc::SpriteVisual ghostLabel_{nullptr};
    std::vector<wuc::SpriteVisual> puffs_;
    double poofTime_ = -1;

    // File drop
    winrt::com_ptr<DropTarget> dropTarget_;
    winrt::com_ptr<DropTarget> edgeDropTarget_;
    std::vector<std::wstring> dropFiles_;
    int dropHighlightId_ = 0;

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
