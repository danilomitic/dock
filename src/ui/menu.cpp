#include "ui/menu.h"

#include <d2d1_1helper.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "core/log.h"
#include "ui/popup.h"

namespace dx {
namespace {

D2D1_COLOR_F ToD2D(winrt::Windows::UI::Color c) { return D2D1::ColorF(c.R / 255.0f, c.G / 255.0f, c.B / 255.0f, c.A / 255.0f); }

struct Level {
    GlassPopup popup;
    const std::vector<MenuItem>* items = nullptr;
    std::vector<float> top;
    std::vector<float> height;
    int hover = -1;
    int parentRow = -1;  // row in the parent level that opened this one
    int w = 0, h = 0;
    wuc::SpriteVisual sprite{nullptr};
};

class Session {
public:
    Session(Graphics& gfx, bool dark, double scale) : gfx_(gfx), dark_(dark), s_(static_cast<float>(scale)) {
        accent_ = ToD2D(gfx_.Accent());
        if (SUCCEEDED(gfx_.DWrite()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                      DWRITE_FONT_STRETCH_NORMAL, 13.5f * s_, L"", font_.put()))) {
            font_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            font_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }

    UINT Run(const std::vector<MenuItem>& items, POINT anchor, MenuSide side) {
        if (!font_ || items.empty()) return 0;
        Open(items, anchor, side, -1);
        if (levels_.empty()) return 0;
        const HWND root = levels_[0]->popup.Hwnd();
        SetCapture(root);
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
        if (GetCapture() == root) ReleaseCapture();
        levels_.clear();
        return result_;
    }

private:
    float Px(float dip) const { return std::round(dip * s_); }
    float PadY() const { return Px(5); }
    float RowH() const { return Px(23); }
    float SepH() const { return Px(11); }
    float TextX() const { return Px(24); }

    float TextWidth(const std::wstring& text) const {
        winrt::com_ptr<IDWriteTextLayout> layout;
        if (FAILED(gfx_.DWrite()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), font_.get(), 4000, RowH(), layout.put())))
            return 0;
        DWRITE_TEXT_METRICS tm{};
        layout->GetMetrics(&tm);
        return tm.widthIncludingTrailingWhitespace;
    }

    void Measure(Level& l) const {
        float y = PadY();
        float textW = 0;
        bool anySub = false;
        l.top.clear();
        l.height.clear();
        for (const MenuItem& it : *l.items) {
            const float h = it.separator ? SepH() : RowH();
            l.top.push_back(y);
            l.height.push_back(h);
            y += h;
            if (!it.separator) textW = std::max(textW, TextWidth(it.text));
            anySub = anySub || !it.sub.empty();
        }
        const float right = Px(anySub ? 34.0f : 20.0f);
        l.w = static_cast<int>(std::ceil(std::max(Px(170), TextX() + textW + right)));
        l.h = static_cast<int>(std::ceil(y + PadY()));
    }

    void Draw(Level& l) const {
        const auto surface = gfx_.CreateSurface(l.w, l.h);
        gfx_.Draw(surface, [&](ID2D1DeviceContext* dc) {
            dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            winrt::com_ptr<ID2D1SolidColorBrush> text, textDisabled, textOnAccent, accent, sep;
            dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.86f), text.put());
            dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(1, 1, 1, 0.35f) : D2D1::ColorF(0, 0, 0, 0.3f), textDisabled.put());
            dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), textOnAccent.put());
            dc->CreateSolidColorBrush(accent_, accent.put());
            dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.1f), sep.put());
            const float w = static_cast<float>(l.w);
            for (size_t i = 0; i < l.items->size(); ++i) {
                const MenuItem& it = (*l.items)[i];
                const float top = l.top[i], h = l.height[i];
                if (it.separator) {
                    const float y = std::round(top + h / 2) + 0.5f;
                    dc->DrawLine(D2D1::Point2F(Px(10), y), D2D1::Point2F(w - Px(10), y), sep.get(), 1.0f);
                    continue;
                }
                const bool hot = static_cast<int>(i) == l.hover && !it.disabled;
                if (hot) {
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(Px(5), top, w - Px(5), top + h), Px(4), Px(4)), accent.get());
                }
                ID2D1SolidColorBrush* brush = it.disabled ? textDisabled.get() : hot ? textOnAccent.get() : text.get();
                if (it.checked) {
                    static const std::wstring kCheck = L"✓";
                    dc->DrawTextW(kCheck.c_str(), static_cast<UINT32>(kCheck.size()), font_.get(),
                                  D2D1::RectF(Px(8), top, TextX(), top + h), brush);
                }
                dc->DrawTextW(it.text.c_str(), static_cast<UINT32>(it.text.size()), font_.get(),
                              D2D1::RectF(TextX(), top, w - Px(20), top + h), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                if (!it.sub.empty()) {
                    static const std::wstring kArrow = L"›";
                    dc->DrawTextW(kArrow.c_str(), static_cast<UINT32>(kArrow.size()), font_.get(),
                                  D2D1::RectF(w - Px(20), top, w - Px(6), top + h), brush);
                }
            }
        });
        if (!l.sprite) {
            l.sprite = gfx_.Comp().CreateSpriteVisual();
            l.popup.Content().Children().InsertAtTop(l.sprite);
        }
        l.sprite.Size({static_cast<float>(l.w), static_cast<float>(l.h)});
        l.sprite.Brush(gfx_.Comp().CreateSurfaceBrush(surface));
    }

    void Open(const std::vector<MenuItem>& items, POINT anchor, MenuSide side, int parentRow) {
        auto level = std::make_unique<Level>();
        Level& l = *level;
        l.items = &items;
        l.parentRow = parentRow;
        const bool isRoot = levels_.empty();
        if (!l.popup.Create(&gfx_, isRoot, true, [this](HWND h, UINT m, WPARAM w, LPARAM lp, bool& handled) {
                return Handle(h, m, w, lp, handled);
            }))
            return;
        l.popup.Style(dark_, Px(9), s_);
        Measure(l);

        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& work = mi.rcWork;
        int x = anchor.x, y = anchor.y;
        switch (side) {
            case MenuSide::Above: x = anchor.x - l.w / 2; y = anchor.y - l.h; break;
            case MenuSide::Right: y = anchor.y - l.h / 2; break;
            case MenuSide::Left: x = anchor.x - l.w; y = anchor.y - l.h / 2; break;
            case MenuSide::AtCursor:
                if (y + l.h > work.bottom) y = anchor.y - l.h;
                if (x + l.w > work.right) x = anchor.x - l.w;
                break;
        }
        if (!isRoot) {
            // Submenu: next to the parent row, flipped to the left if there is no room.
            const Level& parent = *levels_.back();
            const RECT pr = parent.popup.GlassRect();
            x = pr.right + static_cast<int>(Px(1));
            y = pr.top + static_cast<int>(parent.top[parentRow] - PadY());
            if (x + l.w > work.right) x = pr.left - l.w - static_cast<int>(Px(1));
        }
        x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(std::max(work.left, work.right - l.w)));
        y = std::clamp(y, static_cast<int>(work.top), static_cast<int>(std::max(work.top, work.bottom - l.h)));
        l.popup.Place(x, y, l.w, l.h);
        Draw(l);
        levels_.push_back(std::move(level));
        levels_.back()->popup.Show(isRoot);
        if (isRoot)
            Log(L"menu: open at %d,%d size %dx%d, foreground=%d", x, y, l.w, l.h,
                GetForegroundWindow() == levels_.back()->popup.Hwnd() ? 1 : 0);
    }

    void CloseFrom(size_t depth) {
        if (depth < levels_.size()) levels_.resize(depth);
    }

    void Close(UINT result, const wchar_t* why) {
        Log(L"menu: close (%s) -> %u", why, result);
        result_ = result;
        open_ = false;
    }

    // Deepest menu under the point, and the row under it (-1 for padding / separators).
    bool Find(POINT p, size_t& depth, int& row) const {
        for (size_t d = levels_.size(); d-- > 0;) {
            const Level& l = *levels_[d];
            if (!l.popup.Contains(p)) continue;
            depth = d;
            row = -1;
            const float y = static_cast<float>(p.y - l.popup.GlassRect().top);
            for (size_t i = 0; i < l.items->size(); ++i)
                if (y >= l.top[i] && y < l.top[i] + l.height[i] && !(*l.items)[i].separator) row = static_cast<int>(i);
            return true;
        }
        return false;
    }

    void SetHover(Level& l, int row) {
        if (row >= 0 && (*l.items)[row].disabled) row = -1;
        if (l.hover == row) return;
        l.hover = row;
        Draw(l);
    }

    void OpenSub(size_t depth, int row) {
        const MenuItem& it = (*levels_[depth]->items)[row];
        if (it.sub.empty() || it.disabled) return;
        if (levels_.size() > depth + 1 && levels_[depth + 1]->parentRow == row) return;
        CloseFrom(depth + 1);
        Open(it.sub, {}, MenuSide::Right, row);
    }

    void OnMove(POINT p) {
        size_t depth = 0;
        int row = -1;
        if (!Find(p, depth, row)) {
            Level& deepest = *levels_.back();
            SetHover(deepest, -1);
            return;
        }
        if (levels_.size() > depth + 1 && levels_[depth + 1]->parentRow != row && row >= 0) CloseFrom(depth + 1);
        SetHover(*levels_[depth], row);
        if (row >= 0) OpenSub(depth, row);
    }

    void Choose(size_t depth, int row) {
        if (row < 0) return;
        const MenuItem& it = (*levels_[depth]->items)[row];
        if (it.separator || it.disabled) return;
        if (!it.sub.empty()) {
            OpenSub(depth, row);
            return;
        }
        Close(it.id, L"chosen");
    }

    void MoveHover(Level& l, int dir) {
        const int n = static_cast<int>(l.items->size());
        int r = l.hover;
        for (int k = 0; k < n; ++k) {
            r = (r < 0) ? (dir > 0 ? 0 : n - 1) : (r + dir + n) % n;
            const MenuItem& it = (*l.items)[r];
            if (!it.separator && !it.disabled) break;
        }
        SetHover(l, r);
    }

    void OnKey(WPARAM key) {
        Level& l = *levels_.back();
        const size_t depth = levels_.size() - 1;
        switch (key) {
            case VK_ESCAPE:
                if (depth > 0) CloseFrom(depth);
                else Close(0, L"escape");
                break;
            case VK_UP: MoveHover(l, -1); break;
            case VK_DOWN: MoveHover(l, +1); break;
            case VK_RIGHT:
                if (l.hover >= 0 && !(*l.items)[l.hover].sub.empty()) {
                    OpenSub(depth, l.hover);
                    MoveHover(*levels_.back(), +1);
                }
                break;
            case VK_LEFT:
                if (depth > 0) CloseFrom(depth);
                break;
            case VK_RETURN:
            case VK_SPACE:
                Choose(depth, l.hover);
                if (levels_.size() > depth + 1) MoveHover(*levels_.back(), +1);
                break;
            default: break;
        }
    }

    LRESULT Handle(HWND, UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
        if (levels_.empty()) return 0;
        POINT p{};
        switch (msg) {
            case WM_MOUSEMOVE:
                GetCursorPos(&p);
                OnMove(p);
                handled = true;
                return 0;
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN: {
                GetCursorPos(&p);
                size_t d = 0;
                int r = -1;
                if (!Find(p, d, r)) Close(0, L"click outside");
                handled = true;
                return 0;
            }
            case WM_LBUTTONUP:
            case WM_RBUTTONUP: {
                GetCursorPos(&p);
                size_t d = 0;
                int r = -1;
                if (Find(p, d, r)) Choose(d, r);
                handled = true;
                return 0;
            }
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                OnKey(wp);
                handled = true;
                return 0;
            case WM_CAPTURECHANGED:
                if (reinterpret_cast<HWND>(lp) != levels_[0]->popup.Hwnd()) Close(0, L"capture lost");
                return 0;
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE) Close(0, L"deactivated");
                return 0;
            default:
                return 0;
        }
    }

    Graphics& gfx_;
    bool dark_;
    float s_;
    D2D1_COLOR_F accent_{};
    winrt::com_ptr<IDWriteTextFormat> font_;
    std::vector<std::unique_ptr<Level>> levels_;
    bool open_ = true;
    UINT result_ = 0;
};

}  // namespace

UINT MacMenu::Run(Graphics& gfx, const std::vector<MenuItem>& items, POINT anchor, MenuSide side, bool dark, double scale) {
    Session session(gfx, dark, scale);
    return session.Run(items, anchor, side);
}

}  // namespace dx
