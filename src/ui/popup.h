#pragma once
#include <windows.h>

#include <functional>

#include "render/graphics.h"

namespace dx {

// A borderless topmost window drawn with the shared compositor.
// With `glass`, it gets the macOS look: blurred translucent background,
// rounded corners, a hairline border and a soft shadow.
class GlassPopup {
public:
    // Return true in `handled` to stop default processing.
    using Handler = std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool& handled)>;

    GlassPopup() = default;
    GlassPopup(const GlassPopup&) = delete;
    GlassPopup& operator=(const GlassPopup&) = delete;
    ~GlassPopup() { Destroy(); }

    bool Create(Graphics* gfx, bool activatable, bool glass, Handler handler);
    void Destroy();

    // Colors, corner radius and shadow. Call before Place.
    void Style(bool dark, float radius, float scale);
    // The glass rectangle in screen pixels (the window is a bit larger, for the shadow).
    void Place(int x, int y, int w, int h);
    void MoveTo(int x, int y);  // same size, new position (cheap; used while dragging)
    void Show(bool activate);
    void Hide();

    [[nodiscard]] HWND Hwnd() const { return hwnd_; }
    [[nodiscard]] const wuc::ContainerVisual& Content() const { return content_; }
    [[nodiscard]] int Margin() const { return margin_; }
    [[nodiscard]] RECT GlassRect() const { return {x_, y_, x_ + w_, y_ + h_}; }
    [[nodiscard]] bool Contains(POINT screen) const { const RECT r = GlassRect(); return PtInRect(&r, screen) != FALSE; }

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);

    Graphics* gfx_ = nullptr;
    HWND hwnd_ = nullptr;
    Handler handler_;
    bool glass_ = true;
    bool activatable_ = false;

    wuc::ContainerVisual root_{nullptr};
    wuc::ContainerVisual box_{nullptr};
    wuc::ContainerVisual content_{nullptr};
    wuc::SpriteVisual shadow_{nullptr};
    wuc::SpriteVisual backdrop_{nullptr};
    wuc::SpriteVisual tint_{nullptr};
    wuc::ShapeVisual border_{nullptr};
    wuc::CompositionRoundedRectangleGeometry clipGeom_{nullptr};
    wuc::CompositionRoundedRectangleGeometry borderGeom_{nullptr};
    wuc::CompositionSpriteShape borderShape_{nullptr};

    float radius_ = 10;
    float scale_ = 1;
    float shadowExtent_ = 0;
    int margin_ = 0;
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
};

}  // namespace dx
