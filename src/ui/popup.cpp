#include "ui/popup.h"

#include <d2d1_1helper.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>

namespace dx {

bool GlassPopup::Create(Graphics* gfx, bool activatable, bool glass, Handler handler) {
    gfx_ = gfx;
    glass_ = glass;
    activatable_ = activatable;
    handler_ = std::move(handler);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DockX.Popup";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }
    DWORD ex = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    if (!activatable) ex |= WS_EX_NOACTIVATE;
    hwnd_ = CreateWindowExW(ex, L"DockX.Popup", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    const BOOL on = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_HOSTBACKDROPBRUSH, &on, sizeof(on));
    DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &on, sizeof(on));

    const auto& c = gfx_->Comp();
    root_ = gfx_->AttachWindow(hwnd_);
    shadow_ = c.CreateSpriteVisual();
    box_ = c.CreateContainerVisual();
    content_ = c.CreateContainerVisual();
    content_.RelativeSizeAdjustment({1.0f, 1.0f});
    if (glass_) {
        clipGeom_ = c.CreateRoundedRectangleGeometry();
        box_.Clip(c.CreateGeometricClip(clipGeom_));
        backdrop_ = c.CreateSpriteVisual();
        backdrop_.RelativeSizeAdjustment({1.0f, 1.0f});
        backdrop_.Brush(c.CreateHostBackdropBrush());
        tint_ = c.CreateSpriteVisual();
        tint_.RelativeSizeAdjustment({1.0f, 1.0f});
        box_.Children().InsertAtTop(backdrop_);
        box_.Children().InsertAtTop(tint_);
        border_ = c.CreateShapeVisual();
        borderGeom_ = c.CreateRoundedRectangleGeometry();
        borderShape_ = c.CreateSpriteShape(borderGeom_);
        border_.Shapes().Append(borderShape_);
    }
    box_.Children().InsertAtTop(content_);
    root_.Children().InsertAtTop(shadow_);
    root_.Children().InsertAtTop(box_);
    if (border_) root_.Children().InsertAtTop(border_);
    return true;
}

void GlassPopup::Destroy() {
    if (!hwnd_) return;
    if (gfx_) gfx_->DetachWindow(hwnd_);
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    root_ = nullptr;
}

void GlassPopup::Style(bool dark, float radius, float scale) {
    radius_ = radius;
    scale_ = scale;
    if (!glass_) {
        shadow_.IsVisible(false);
        shadowExtent_ = 0;
        margin_ = 0;
        return;
    }
    const auto& c = gfx_->Comp();
    tint_.Brush(c.CreateColorBrush(dark ? Rgba(36, 36, 38, 175) : Rgba(246, 246, 246, 190)));
    borderShape_.StrokeBrush(c.CreateColorBrush(dark ? Rgba(255, 255, 255, 40) : Rgba(0, 0, 0, 30)));
    borderShape_.StrokeThickness(scale);
    const ShadowBrush sb = gfx_->CreateShadow(radius, 9 * scale);
    shadow_.Brush(sb.brush);
    shadow_.Opacity(dark ? 0.55f : 0.30f);
    shadowExtent_ = sb.extent;
    margin_ = static_cast<int>(std::ceil(sb.extent + 4 * scale));
}

void GlassPopup::SetBeak(Edge dockEdge, POINT target) {
    beak_ = true;
    beakEdge_ = dockEdge;
    beakTarget_ = target;
}

void GlassPopup::Place(int x, int y, int w, int h) {
    x_ = x;
    y_ = y;
    w_ = w;
    h_ = h;
    const float m = static_cast<float>(margin_);
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    const float bh = beak_ ? std::round(10 * scale_) : 0;  // beak length
    const float bw = std::round(22 * scale_);              // beak base width
    box_.Offset({m, m, 0});
    box_.Size({fw, fh});
    content_.RelativeSizeAdjustment({0, 0});
    content_.Size({fw, fh});
    RECT beakRect{};
    if (glass_) {
        // The glass also covers the beak, so it reaches past the body on every side.
        backdrop_.RelativeSizeAdjustment({0, 0});
        backdrop_.Offset({-bh, -bh, 0});
        backdrop_.Size({fw + 2 * bh, fh + 2 * bh});
        tint_.RelativeSizeAdjustment({0, 0});
        tint_.Offset({-bh, -bh, 0});
        tint_.Size({fw + 2 * bh, fh + 2 * bh});
        border_.Offset({m, m, 0});
        border_.Size({fw, fh});
        if (beak_) {
            // Body + pointer as one shape: clip, border and hit area.
            auto* factory = gfx_->D2DFactory();
            winrt::com_ptr<ID2D1RoundedRectangleGeometry> body;
            factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(0, 0, fw, fh), radius_, radius_), body.put());
            D2D1_POINT_2F p0{}, p1{}, tip{};
            if (beakEdge_ == Edge::Bottom) {
                const float bx = std::clamp(static_cast<float>(beakTarget_.x - x), radius_ + bw, fw - radius_ - bw);
                p0 = {bx - bw / 2, fh - 1};
                p1 = {bx + bw / 2, fh - 1};
                tip = {bx, fh + bh};
                beakRect = {margin_ + static_cast<LONG>(bx - bw / 2), margin_ + h - 1, margin_ + static_cast<LONG>(bx + bw / 2),
                            margin_ + h + static_cast<LONG>(bh)};
            } else {
                const float by = std::clamp(static_cast<float>(beakTarget_.y - y), radius_ + bw, fh - radius_ - bw);
                const bool left = beakEdge_ == Edge::Left;  // dock on the left: beak on our left side
                const float sideX = left ? 1.0f : fw - 1;
                p0 = {sideX, by - bw / 2};
                p1 = {sideX, by + bw / 2};
                tip = {left ? -bh : fw + bh, by};
                beakRect = {margin_ + static_cast<LONG>(left ? -bh : fw - 1), margin_ + static_cast<LONG>(by - bw / 2),
                            margin_ + static_cast<LONG>(left ? 1 : fw + bh), margin_ + static_cast<LONG>(by + bw / 2)};
            }
            winrt::com_ptr<ID2D1PathGeometry> triangle;
            factory->CreatePathGeometry(triangle.put());
            winrt::com_ptr<ID2D1GeometrySink> sink;
            triangle->Open(sink.put());
            sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(tip);
            sink->AddLine(p1);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            winrt::com_ptr<ID2D1PathGeometry> shape;
            factory->CreatePathGeometry(shape.put());
            winrt::com_ptr<ID2D1GeometrySink> shapeSink;
            shape->Open(shapeSink.put());
            body->CombineWithGeometry(triangle.get(), D2D1_COMBINE_MODE_UNION, nullptr, shapeSink.get());
            shapeSink->Close();
            box_.Clip(gfx_->Comp().CreateGeometricClip(gfx_->CreatePathGeometry(shape.get())));
            borderShape_.Geometry(gfx_->CreatePathGeometry(shape.get()));
            borderShape_.Offset({0.5f, 0.5f});
        } else {
            clipGeom_.Size({fw, fh});
            clipGeom_.CornerRadius({radius_, radius_});
            borderGeom_.Offset({0.5f, 0.5f});
            borderGeom_.Size({fw - 1, fh - 1});
            borderGeom_.CornerRadius({std::max(0.0f, radius_ - 0.5f), std::max(0.0f, radius_ - 0.5f)});
        }
        const float e = shadowExtent_;
        shadow_.Offset({m - e, m - e + 3 * scale_, 0});
        shadow_.Size({fw + 2 * e, fh + 2 * e});
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, x - margin_, y - margin_, w + 2 * margin_, h + 2 * margin_, SWP_NOACTIVATE);
    // Only the glass takes the mouse; the shadow margin lets clicks through.
    HRGN rgn = CreateRectRgn(margin_, margin_, margin_ + w, margin_ + h);
    if (beak_) {
        HRGN extra = CreateRectRgnIndirect(&beakRect);
        CombineRgn(rgn, rgn, extra, RGN_OR);
        DeleteObject(extra);
    }
    if (!SetWindowRgn(hwnd_, rgn, FALSE)) DeleteObject(rgn);
}

void GlassPopup::MoveTo(int x, int y) {
    x_ = x;
    y_ = y;
    SetWindowPos(hwnd_, HWND_TOPMOST, x - margin_, y - margin_, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

void GlassPopup::Show(bool activate) {
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | (activate ? 0 : SWP_NOACTIVATE));
    if (activate) {
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
    }
}

void GlassPopup::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

LRESULT CALLBACK GlassPopup::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    }
    auto* self = reinterpret_cast<GlassPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        if (msg == WM_MOUSEACTIVATE && !self->activatable_) return MA_NOACTIVATE;
        if (self->handler_) {
            bool handled = false;
            const LRESULT r = self->handler_(hwnd, msg, wp, lp, handled);
            if (handled) return r;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace dx
