#include "ui/expose.h"

#include <d2d1_1helper.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "core/strings.h"
#include "shell/apps.h"
#include "ui/popup.h"

namespace dx {
namespace {

struct Cell {
    HWND hwnd = nullptr;
    HTHUMBNAIL thumb = nullptr;
    D2D1_RECT_F rect{};   // whole cell, glass-local
    D2D1_RECT_F image{};  // thumbnail area, glass-local
    std::wstring title;
    bool minimized = false;
};

class Session {
public:
    Session(Graphics& gfx, bool dark, double scale) : gfx_(gfx), dark_(dark), s_(static_cast<float>(scale)) {}

    ~Session() {
        for (Cell& c : cells_)
            if (c.thumb) DwmUnregisterThumbnail(c.thumb);
    }

    HWND Run(const std::vector<HWND>& windows, RECT icon, Edge edge) {
        if (windows.empty()) return nullptr;
        if (!popup_.Create(&gfx_, true, true, [this](HWND h, UINT m, WPARAM w, LPARAM l, bool& handled) {
                return Handle(h, m, w, l, handled);
            }))
            return nullptr;
        popup_.Style(dark_, Px(14), s_);
        gfx_.DWrite()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, 12.5f * s_, L"", font_.put());
        if (font_) {
            font_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            font_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            font_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            winrt::com_ptr<IDWriteInlineObject> ellipsis;
            gfx_.DWrite()->CreateEllipsisTrimmingSign(font_.get(), ellipsis.put());
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            font_->SetTrimming(&trimming, ellipsis.get());
        }

        // Grid size: up to 4 columns, shrunk to fit the screen.
        MONITORINFO mi{sizeof(mi)};
        const POINT center{(icon.left + icon.right) / 2, (icon.top + icon.bottom) / 2};
        GetMonitorInfoW(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& work = mi.rcWork;
        const int n = static_cast<int>(windows.size());
        const float pad = Px(14), titleH = Px(26);
        float cw = Px(250), ch = Px(156);
        const int cols = std::min(n, 4);
        const int rows = (n + cols - 1) / cols;
        const float maxW = (work.right - work.left) * 0.9f, maxH = (work.bottom - work.top) * 0.75f;
        const float fit = std::min({1.0f, (maxW - pad) / (cols * (cw + pad)), (maxH - pad) / (rows * (ch + titleH + pad))});
        cw = std::floor(cw * fit);
        ch = std::floor(ch * fit);
        const int w = static_cast<int>(std::ceil(cols * (cw + pad) + pad));
        const int h = static_cast<int>(std::ceil(rows * (ch + titleH + pad) + pad));

        int x = center.x - w / 2, y = icon.top - static_cast<int>(Px(12)) - h;
        if (edge == Edge::Left) { x = icon.right + static_cast<int>(Px(12)); y = center.y - h / 2; }
        if (edge == Edge::Right) { x = icon.left - static_cast<int>(Px(12)) - w; y = center.y - h / 2; }
        x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(std::max(work.left, work.right - w)));
        y = std::clamp(y, static_cast<int>(work.top), static_cast<int>(std::max(work.top, work.bottom - h)));
        popup_.Place(x, y, w, h);
        w_ = w;
        h_ = h;

        const float m = static_cast<float>(popup_.Margin());
        for (int i = 0; i < n; ++i) {
            Cell c;
            c.hwnd = windows[i];
            c.title = WindowTitle(c.hwnd);
            c.minimized = IsIconic(c.hwnd) != FALSE;
            const float cx = pad + (i % cols) * (cw + pad);
            const float cy = pad + (i / cols) * (ch + titleH + pad);
            c.rect = D2D1::RectF(cx, cy, cx + cw, cy + ch + titleH);
            c.image = D2D1::RectF(cx, cy, cx + cw, cy + ch);
            if (!c.minimized && SUCCEEDED(DwmRegisterThumbnail(popup_.Hwnd(), c.hwnd, &c.thumb))) {
                SIZE src{};
                DwmQueryThumbnailSourceSize(c.thumb, &src);
                const float inset = Px(6);
                const float bw = cw - 2 * inset, bh = ch - 2 * inset;
                const float k = (src.cx > 0 && src.cy > 0) ? std::min({bw / src.cx, bh / src.cy, 1.0f}) : 1.0f;
                const float tw = src.cx * k, th = src.cy * k;
                const float tx = cx + (cw - tw) / 2, ty = cy + (ch - th) / 2;
                DWM_THUMBNAIL_PROPERTIES p{};
                p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
                p.rcDestination = {static_cast<LONG>(m + tx), static_cast<LONG>(m + ty), static_cast<LONG>(m + tx + tw),
                                   static_cast<LONG>(m + ty + th)};
                p.fVisible = TRUE;
                p.opacity = 255;
                p.fSourceClientAreaOnly = FALSE;
                DwmUpdateThumbnailProperties(c.thumb, &p);
            }
            cells_.push_back(std::move(c));
        }
        Draw();
        popup_.Show(true);
        SetCapture(popup_.Hwnd());

        MSG msg;
        while (open_) {
            const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
            if (r <= 0) {
                if (r == 0) PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        open_ = false;
        if (GetCapture() == popup_.Hwnd()) ReleaseCapture();
        return result_;
    }

private:
    float Px(float dip) const { return std::round(dip * s_); }

    void Draw() {
        const auto surface = gfx_.CreateSurface(w_, h_);
        const winrt::Windows::UI::Color a = gfx_.Accent();
        gfx_.Draw(surface, [&](ID2D1DeviceContext* dc) {
            dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            winrt::com_ptr<ID2D1SolidColorBrush> text, accentFill, accentEdge, plate;
            dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(1, 1, 1, 0.9f) : D2D1::ColorF(0, 0, 0, 0.85f), text.put());
            dc->CreateSolidColorBrush(D2D1::ColorF(a.R / 255.f, a.G / 255.f, a.B / 255.f, 0.28f), accentFill.put());
            dc->CreateSolidColorBrush(D2D1::ColorF(a.R / 255.f, a.G / 255.f, a.B / 255.f, 1.0f), accentEdge.put());
            dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(0, 0, 0, 0.05f), plate.put());
            for (size_t i = 0; i < cells_.size(); ++i) {
                const Cell& c = cells_[i];
                const auto box = D2D1::RoundedRect(D2D1::RectF(c.rect.left - Px(4), c.rect.top - Px(4), c.rect.right + Px(4),
                                                               c.rect.bottom + Px(2)),
                                                   Px(9), Px(9));
                if (static_cast<int>(i) == hover_) {
                    dc->FillRoundedRectangle(box, accentFill.get());
                    dc->DrawRoundedRectangle(box, accentEdge.get(), Px(2));
                }
                if (c.minimized || !c.thumb) {
                    dc->FillRoundedRectangle(D2D1::RoundedRect(c.image, Px(6), Px(6)), plate.get());
                    const wchar_t* label = Tr(Text::Minimized);
                    if (font_)
                        dc->DrawTextW(label, static_cast<UINT32>(wcslen(label)), font_.get(), c.image, text.get());
                }
                if (font_)
                    dc->DrawTextW(c.title.c_str(), static_cast<UINT32>(c.title.size()), font_.get(),
                                  D2D1::RectF(c.rect.left, c.image.bottom, c.rect.right, c.rect.bottom), text.get(),
                                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        });
        if (!sprite_) {
            sprite_ = gfx_.Comp().CreateSpriteVisual();
            popup_.Content().Children().InsertAtTop(sprite_);
        }
        sprite_.Size({static_cast<float>(w_), static_cast<float>(h_)});
        sprite_.Brush(gfx_.Comp().CreateSurfaceBrush(surface));
    }

    int CellAt(POINT screen) const {
        const RECT g = popup_.GlassRect();
        const float x = static_cast<float>(screen.x - g.left), y = static_cast<float>(screen.y - g.top);
        for (size_t i = 0; i < cells_.size(); ++i) {
            const auto& r = cells_[i].rect;
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return static_cast<int>(i);
        }
        return -1;
    }

    void SetHover(int i) {
        if (i == hover_) return;
        hover_ = i;
        Draw();
    }

    LRESULT Handle(HWND, UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
        POINT p{};
        switch (msg) {
            case WM_MOUSEMOVE:
                GetCursorPos(&p);
                SetHover(CellAt(p));
                handled = true;
                return 0;
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
                GetCursorPos(&p);
                if (!popup_.Contains(p)) open_ = false;
                handled = true;
                return 0;
            case WM_LBUTTONUP: {
                GetCursorPos(&p);
                const int i = CellAt(p);
                if (i >= 0) {
                    result_ = cells_[i].hwnd;
                    open_ = false;
                }
                handled = true;
                return 0;
            }
            case WM_KEYDOWN: {
                const int n = static_cast<int>(cells_.size());
                if (wp == VK_ESCAPE) open_ = false;
                else if (wp == VK_RETURN && hover_ >= 0) { result_ = cells_[hover_].hwnd; open_ = false; }
                else if (wp == VK_RIGHT || wp == VK_TAB) SetHover((hover_ + 1) % n);
                else if (wp == VK_LEFT) SetHover(hover_ <= 0 ? n - 1 : hover_ - 1);
                handled = true;
                return 0;
            }
            case WM_CAPTURECHANGED:
                if (reinterpret_cast<HWND>(lp) != popup_.Hwnd()) open_ = false;
                return 0;
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE) open_ = false;
                return 0;
            default:
                return 0;
        }
    }

    Graphics& gfx_;
    bool dark_;
    float s_;
    GlassPopup popup_;
    wuc::SpriteVisual sprite_{nullptr};
    winrt::com_ptr<IDWriteTextFormat> font_;
    std::vector<Cell> cells_;
    int w_ = 0, h_ = 0;
    int hover_ = -1;
    bool open_ = true;
    HWND result_ = nullptr;
};

}  // namespace

HWND Expose::Run(Graphics& gfx, const std::vector<HWND>& windows, RECT icon, Edge edge, bool dark, double scale) {
    Session session(gfx, dark, scale);
    return session.Run(windows, icon, edge);
}

}  // namespace dx
