#include "ui/stack.h"

#include <d2d1_1helper.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

#include "core/log.h"
#include "core/strings.h"
#include "shell/files.h"
#include "shell/icons.h"
#include "ui/popup.h"

namespace dx {
namespace {

constexpr UINT kMsgIcon = WM_APP + 20;
constexpr int kFanMax = 10;

enum class Mode { Fan, Grid, List };

struct Cell {
    int entry = -1;          // index into entries, -1 = the "Open in Explorer" link
    D2D1_RECT_F rect{};      // hit area, glass-local pixels
    float iconPx = 0;
    wuc::ContainerVisual row{nullptr};  // fan only
    wuc::SpriteVisual icon{nullptr};
    wuc::SpriteVisual pill{nullptr};    // fan only
    wuc::CompositionSurfaceBrush pillNormal{nullptr};
    wuc::CompositionSurfaceBrush pillHot{nullptr};
};

D2D1_COLOR_F ToD2D(winrt::Windows::UI::Color c, float alpha) { return D2D1::ColorF(c.R / 255.f, c.G / 255.f, c.B / 255.f, alpha); }

class Session {
public:
    Session(Graphics& gfx, const StackOptions& o) : gfx_(gfx), o_(o), s_(static_cast<float>(o.scale)) {}

    ~Session() { loader_.Stop(); }

    void Run() {
        entries_ = ListFolder(o_.folder, o_.sort);
        const bool side = o_.edge != Edge::Bottom;
        mode_ = o_.view == "fan" ? Mode::Fan : o_.view == "grid" ? Mode::Grid : o_.view == "list" ? Mode::List
              : (entries_.size() <= 9 && !entries_.empty() ? Mode::Fan : Mode::Grid);
        if (mode_ == Mode::Fan && (side || entries_.empty())) mode_ = Mode::Grid;  // the fan only rises from a bottom dock
        CreateFonts();

        const bool glass = mode_ != Mode::Fan;
        if (!popup_.Create(&gfx_, true, glass, [this](HWND h, UINT m, WPARAM w, LPARAM l, bool& handled) {
                return Handle(h, m, w, l, handled);
            }))
            return;
        popup_.Style(o_.dark, Px(14), s_);
        loader_.Start(popup_.Hwnd(), kMsgIcon);

        if (mode_ == Mode::Fan) BuildFan();
        else BuildPanel();

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
        // Free icons that arrived after we stopped listening.
        loader_.Stop();
        MSG pending;
        while (PeekMessageW(&pending, popup_.Hwnd(), kMsgIcon, kMsgIcon, PM_REMOVE))
            delete reinterpret_cast<IconResult*>(pending.lParam);
    }

private:
    float Px(float dip) const { return std::round(dip * s_); }

    void CreateFonts() {
        auto* dw = gfx_.DWrite();
        winrt::com_ptr<IDWriteInlineObject> ellipsis;
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        auto make = [&](winrt::com_ptr<IDWriteTextFormat>& f, float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align,
                        bool wrap) {
            if (FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                            size * s_, L"", f.put())))
                return;
            f->SetTextAlignment(align);
            f->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_EMERGENCY_BREAK : DWRITE_WORD_WRAPPING_NO_WRAP);
            winrt::com_ptr<IDWriteInlineObject> sign;
            dw->CreateEllipsisTrimmingSign(f.get(), sign.put());
            f->SetTrimming(&trimming, sign.get());
        };
        make(fontPill_, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false);
        make(fontName_, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
        make(fontRow_, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        make(fontTitle_, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
        make(fontLink_, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING, false);
    }

    std::wstring LinkText(size_t shown) const {
        if (entries_.size() <= shown) return Tr(Text::OpenInExplorer);
        wchar_t buf[96];
        swprintf_s(buf, Tr(Text::MoreInExplorer), static_cast<int>(entries_.size() - shown));
        return buf;
    }

    float TextWidth(const std::wstring& t, IDWriteTextFormat* f, float max) const {
        winrt::com_ptr<IDWriteTextLayout> l;
        if (FAILED(gfx_.DWrite()->CreateTextLayout(t.c_str(), static_cast<UINT32>(t.size()), f, max, 100, l.put()))) return 0;
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        return std::min(max, m.widthIncludingTrailingWhitespace);
    }

    wuc::CompositionSurfaceBrush PillBrush(const std::wstring& text, float w, float h, bool hot) const {
        const auto surface = gfx_.CreateSurface(static_cast<int>(w), static_cast<int>(h));
        const auto accent = gfx_.Accent();
        gfx_.Draw(surface, [&](ID2D1DeviceContext* dc) {
            winrt::com_ptr<ID2D1SolidColorBrush> bg, fg;
            const D2D1_COLOR_F back = hot ? ToD2D(accent, 1.0f)
                                    : o_.dark ? D2D1::ColorF(0.12f, 0.12f, 0.13f, 0.9f) : D2D1::ColorF(0.97f, 0.97f, 0.97f, 0.93f);
            dc->CreateSolidColorBrush(back, bg.put());
            dc->CreateSolidColorBrush(hot || o_.dark ? D2D1::ColorF(1, 1, 1, 0.95f) : D2D1::ColorF(0, 0, 0, 0.85f), fg.put());
            dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, w, h), h / 2, h / 2), bg.get());
            dc->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fontPill_.get(), D2D1::RectF(Px(8), 0, w - Px(8), h), fg.get());
        });
        return gfx_.Comp().CreateSurfaceBrush(surface);
    }

    void RequestIcons() {
        for (size_t i = 0; i < cells_.size(); ++i) {
            const Cell& c = cells_[i];
            if (c.entry < 0) continue;
            IconRequest r;
            r.id = static_cast<int>(i);
            r.source = entries_[c.entry].path;
            r.cache = false;
            r.thumbnail = true;
            r.size = c.iconPx <= 32 ? 48 : 128;
            loader_.Request(std::move(r));
        }
    }

    // ---------------------------------------------------------------- Fan

    void BuildFan() {
        const auto& c = gfx_.Comp();
        const size_t shown = std::min<size_t>(entries_.size(), kFanMax);
        const float iconPx = Px(54), rowH = iconPx + Px(6), pillH = Px(24), pillGap = Px(10);
        const float ax = (o_.icon.left + o_.icon.right) / 2.0f;
        const float ay = (o_.icon.top + o_.icon.bottom) / 2.0f;
        const float baseY = o_.icon.top - Px(10);

        struct Row {
            float cx, cy, angle, pillW;
            std::wstring text;
            int entry;
        };
        std::vector<Row> rows;
        for (size_t i = 0; i <= shown; ++i) {
            const bool link = i == shown;
            Row r{};
            r.cx = ax + 0.9f * s_ * static_cast<float>(i * i);  // the fan curves to the right as it rises
            r.cy = baseY - rowH * static_cast<float>(i) - rowH / 2;
            r.angle = 0.9f * static_cast<float>(i);
            r.entry = link ? -1 : static_cast<int>(i);
            r.text = link ? LinkText(shown) : entries_[i].name;
            r.pillW = std::ceil(TextWidth(r.text, fontPill_.get(), Px(220)) + Px(18));
            rows.push_back(std::move(r));
        }
        float l = 1e9f, t = 1e9f, rt = -1e9f, b = -1e9f;
        for (const Row& r : rows) {
            l = std::min(l, r.cx - iconPx / 2 - pillGap - r.pillW);
            rt = std::max(rt, r.cx + iconPx / 2);
            t = std::min(t, r.cy - rowH / 2);
            b = std::max(b, r.cy + rowH / 2);
        }
        const float m = Px(24);
        const int wx = static_cast<int>(l - m), wy = static_cast<int>(t - m);
        const int ww = static_cast<int>(rt - l + 2 * m), wh = static_cast<int>(b - t + 2 * m);
        popup_.Place(wx, wy, ww, wh);

        const auto ease = c.CreateCubicBezierEasingFunction({0.23f, 1.0f}, {0.32f, 1.0f});
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            Cell cell;
            cell.entry = r.entry;
            cell.iconPx = iconPx;
            const float lx = r.cx - wx, ly = r.cy - wy;
            cell.rect = D2D1::RectF(lx - iconPx / 2 - pillGap - r.pillW, ly - rowH / 2, lx + iconPx / 2, ly + rowH / 2);
            cell.row = c.CreateContainerVisual();
            cell.row.RotationAngleInDegrees(r.angle);
            cell.pillNormal = PillBrush(r.text, r.pillW, pillH, false);
            cell.pillHot = PillBrush(r.text, r.pillW, pillH, true);
            cell.pill = c.CreateSpriteVisual();
            cell.pill.Size({r.pillW, pillH});
            cell.pill.Brush(cell.pillNormal);
            if (r.entry >= 0) {
                cell.pill.Offset({-iconPx / 2 - pillGap - r.pillW, -pillH / 2, 0});
                cell.icon = c.CreateSpriteVisual();
                cell.icon.Size({iconPx, iconPx});
                cell.icon.Offset({-iconPx / 2, -iconPx / 2, 0});
                cell.row.Children().InsertAtTop(cell.icon);
            } else {
                cell.pill.Offset({iconPx / 2 - r.pillW, -pillH / 2, 0});
            }
            cell.row.Children().InsertAtTop(cell.pill);
            popup_.Content().Children().InsertAtTop(cell.row);

            // Fly out of the dock icon, one after another.
            using namespace std::chrono;
            const float3 from{ax - wx, ay - wy, 0}, to{lx, ly, 0};
            const auto delay = duration_cast<winrt::Windows::Foundation::TimeSpan>(milliseconds(12 * static_cast<int>(i)));
            const auto dur = duration_cast<winrt::Windows::Foundation::TimeSpan>(milliseconds(300));
            auto move = c.CreateVector3KeyFrameAnimation();
            move.InsertKeyFrame(0.0f, from);
            move.InsertKeyFrame(1.0f, to, ease);
            move.Duration(dur);
            move.DelayTime(delay);
            move.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
            cell.row.Offset(to);
            cell.row.StartAnimation(L"Offset", move);
            auto grow = c.CreateVector3KeyFrameAnimation();
            grow.InsertKeyFrame(0.0f, {0.4f, 0.4f, 1});
            grow.InsertKeyFrame(1.0f, {1, 1, 1}, ease);
            grow.Duration(dur);
            grow.DelayTime(delay);
            grow.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
            cell.row.StartAnimation(L"Scale", grow);
            auto fade = c.CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(0.0f, 0.0f);
            fade.InsertKeyFrame(1.0f, 1.0f, ease);
            fade.Duration(dur);
            fade.DelayTime(delay);
            fade.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
            cell.row.StartAnimation(L"Opacity", fade);
            cells_.push_back(std::move(cell));
        }
        RequestIcons();
    }

    // ---------------------------------------------------------------- Grid / List

    void BuildPanel() {
        const auto& c = gfx_.Comp();
        const bool grid = mode_ == Mode::Grid;
        const size_t n = entries_.size();
        const float pad = Px(12), header = Px(40);
        int cols = 1;
        size_t shown = 0;
        float cellW = 0, cellH = 0, iconPx = 0;
        if (grid) {
            cols = n <= 3 ? static_cast<int>(std::max<size_t>(n, 1)) : std::clamp(static_cast<int>(std::ceil(std::sqrt(n * 1.4))), 3, 6);
            shown = std::min<size_t>(n, static_cast<size_t>(cols) * 4);
            cellW = Px(96);
            cellH = Px(100);
            iconPx = Px(60);
        } else {
            shown = std::min<size_t>(n, 16);
            cellW = Px(280);
            cellH = Px(28);
            iconPx = Px(20);
        }
        const int rows = std::max(1, static_cast<int>((shown + cols - 1) / cols));
        panelW_ = static_cast<int>(std::ceil(2 * pad + cols * cellW));
        panelH_ = static_cast<int>(std::ceil(header + rows * cellH + pad));
        linkText_ = LinkText(shown) + L"  ›";
        title_ = FolderDisplayName(o_.folder);

        // Place the panel next to the dock icon.
        MONITORINFO mi{sizeof(mi)};
        const POINT center{(o_.icon.left + o_.icon.right) / 2, (o_.icon.top + o_.icon.bottom) / 2};
        GetMonitorInfoW(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& work = mi.rcWork;
        int x = center.x - panelW_ / 2, y = o_.icon.top - static_cast<int>(Px(10)) - panelH_;
        if (o_.edge == Edge::Left) { x = o_.icon.right + static_cast<int>(Px(10)); y = center.y - panelH_ / 2; }
        if (o_.edge == Edge::Right) { x = o_.icon.left - static_cast<int>(Px(10)) - panelW_; y = center.y - panelH_ / 2; }
        x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(std::max(work.left, work.right - panelW_)));
        y = std::clamp(y, static_cast<int>(work.top), static_cast<int>(std::max(work.top, work.bottom - panelH_)));
        popup_.Place(x, y, panelW_, panelH_);

        // The link in the header is cell 0.
        Cell link;
        link.entry = -1;
        const float linkW = TextWidth(linkText_, fontLink_.get(), Px(220)) + Px(8);
        link.rect = D2D1::RectF(panelW_ - pad - linkW, Px(8), panelW_ - pad, header - Px(6));
        cells_.push_back(std::move(link));
        for (size_t i = 0; i < shown; ++i) {
            Cell cell;
            cell.entry = static_cast<int>(i);
            cell.iconPx = iconPx;
            const float cx = pad + (i % cols) * cellW, cy = header + (i / cols) * cellH;
            cell.rect = D2D1::RectF(cx, cy, cx + cellW, cy + cellH);
            cell.icon = c.CreateSpriteVisual();
            cell.icon.Size({iconPx, iconPx});
            if (grid) cell.icon.Offset({cx + (cellW - iconPx) / 2, cy + Px(6), 0});
            else cell.icon.Offset({cx + Px(6), cy + (cellH - iconPx) / 2, 0});
            cells_.push_back(std::move(cell));
        }
        DrawPanel();
        for (auto& cell : cells_)
            if (cell.icon) popup_.Content().Children().InsertAtTop(cell.icon);

        // Zoom in from the dock icon.
        using namespace std::chrono;
        const auto root = popup_.Root();
        const float m = static_cast<float>(popup_.Margin());
        const float rw = panelW_ + 2 * m, rh = panelH_ + 2 * m;
        root.CenterPoint(o_.edge == Edge::Left ? float3{0, rh / 2, 0} : o_.edge == Edge::Right ? float3{rw, rh / 2, 0} : float3{rw / 2, rh, 0});
        const auto ease = c.CreateCubicBezierEasingFunction({0.23f, 1.0f}, {0.32f, 1.0f});
        const auto dur = duration_cast<winrt::Windows::Foundation::TimeSpan>(milliseconds(200));
        auto grow = c.CreateVector3KeyFrameAnimation();
        grow.InsertKeyFrame(0.0f, {0.92f, 0.92f, 1});
        grow.InsertKeyFrame(1.0f, {1, 1, 1}, ease);
        grow.Duration(dur);
        root.StartAnimation(L"Scale", grow);
        auto fade = c.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(0.0f, 0.0f);
        fade.InsertKeyFrame(1.0f, 1.0f, ease);
        fade.Duration(dur);
        root.StartAnimation(L"Opacity", fade);
        RequestIcons();
    }

    void DrawPanel() {
        const bool grid = mode_ == Mode::Grid;
        const auto surface = gfx_.CreateSurface(panelW_, panelH_);
        const auto accent = gfx_.Accent();
        gfx_.Draw(surface, [&](ID2D1DeviceContext* dc) {
            dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            winrt::com_ptr<ID2D1SolidColorBrush> text, dim, link, hot, line;
            dc->CreateSolidColorBrush(o_.dark ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.86f), text.put());
            dc->CreateSolidColorBrush(o_.dark ? D2D1::ColorF(1, 1, 1, 0.5f) : D2D1::ColorF(0, 0, 0, 0.45f), dim.put());
            dc->CreateSolidColorBrush(ToD2D(accent, 1.0f), link.put());
            dc->CreateSolidColorBrush(ToD2D(accent, 0.28f), hot.put());
            dc->CreateSolidColorBrush(o_.dark ? D2D1::ColorF(1, 1, 1, 0.1f) : D2D1::ColorF(0, 0, 0, 0.08f), line.put());
            const float pad = Px(12), header = Px(40);
            dc->DrawTextW(title_.c_str(), static_cast<UINT32>(title_.size()), fontTitle_.get(),
                          D2D1::RectF(pad + Px(4), Px(6), panelW_ / 2.0f, header - Px(4)), text.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            const Cell& lc = cells_[0];
            dc->DrawTextW(linkText_.c_str(), static_cast<UINT32>(linkText_.size()), fontLink_.get(), lc.rect,
                          hover_ == 0 ? text.get() : link.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            dc->DrawLine(D2D1::Point2F(pad, header - 0.5f), D2D1::Point2F(panelW_ - pad, header - 0.5f), line.get());
            if (entries_.empty()) {
                const std::wstring empty = Tr(Text::EmptyFolder);
                dc->DrawTextW(empty.c_str(), static_cast<UINT32>(empty.size()), fontPill_.get(),
                              D2D1::RectF(0, header, static_cast<float>(panelW_), static_cast<float>(panelH_)), dim.get());
            }
            for (size_t i = 1; i < cells_.size(); ++i) {
                const Cell& cell = cells_[i];
                const FolderEntry& e = entries_[cell.entry];
                if (static_cast<int>(i) == hover_)
                    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cell.rect.left + Px(2), cell.rect.top + Px(1),
                                                                           cell.rect.right - Px(2), cell.rect.bottom - Px(1)),
                                                               Px(6), Px(6)),
                                             hot.get());
                if (grid) {
                    const D2D1_RECT_F nameRect = D2D1::RectF(cell.rect.left + Px(4), cell.rect.top + Px(10) + cell.iconPx,
                                                             cell.rect.right - Px(4), cell.rect.bottom - Px(2));
                    dc->DrawTextW(e.name.c_str(), static_cast<UINT32>(e.name.size()), fontName_.get(), nameRect, text.get(),
                                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
                } else {
                    const D2D1_RECT_F nameRect = D2D1::RectF(cell.rect.left + Px(14) + cell.iconPx, cell.rect.top,
                                                             cell.rect.right - Px(8), cell.rect.bottom);
                    dc->DrawTextW(e.name.c_str(), static_cast<UINT32>(e.name.size()), fontRow_.get(), nameRect, text.get(),
                                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        });
        if (!panelSprite_) {
            panelSprite_ = gfx_.Comp().CreateSpriteVisual();
            popup_.Content().Children().InsertAtBottom(panelSprite_);
        }
        panelSprite_.Size({static_cast<float>(panelW_), static_cast<float>(panelH_)});
        panelSprite_.Brush(gfx_.Comp().CreateSurfaceBrush(surface));
    }

    // ---------------------------------------------------------------- Interaction

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
        const int old = hover_;
        hover_ = i;
        if (mode_ == Mode::Fan) {
            if (old >= 0) cells_[old].pill.Brush(cells_[old].pillNormal);
            if (i >= 0) cells_[i].pill.Brush(cells_[i].pillHot);
        } else {
            DrawPanel();
        }
    }

    void OpenCell(int i) {
        if (i < 0) return;
        const Cell& c = cells_[i];
        OpenPath(c.entry >= 0 ? entries_[c.entry].path : o_.folder);
        open_ = false;
    }

    // Drag a file out of the stack and drop it anywhere (Explorer, an app, the desktop).
    void DragOut(int i) {
        const std::wstring path = entries_[cells_[i].entry].path;
        dragging_ = true;
        ReleaseCapture();
        popup_.Hide();
        winrt::com_ptr<IShellItem> item;
        winrt::com_ptr<IDataObject> data;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put()))) &&
            SUCCEEDED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(data.put())))) {
            DWORD effect = 0;
            SHDoDragDrop(nullptr, data.get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
        }
        open_ = false;
    }

    void OnIcon(IconResult* r) {
        std::unique_ptr<IconResult> owned(r);
        if (!r->bitmap || r->id < 0 || r->id >= static_cast<int>(cells_.size())) return;
        Cell& c = cells_[r->id];
        if (!c.icon) return;
        const int px = static_cast<int>(std::ceil(c.iconPx));
        const auto surface = gfx_.CreateSurface(px, px);
        gfx_.Draw(surface, [&](ID2D1DeviceContext* dc) {
            winrt::com_ptr<ID2D1Bitmap1> bmp;
            if (FAILED(dc->CreateBitmapFromWicBitmap(r->bitmap.get(), nullptr, bmp.put()))) return;
            const auto size = bmp->GetSize();
            const float k = std::min(px / size.width, px / size.height);  // pictures keep their shape
            const float w = size.width * k, h = size.height * k;
            const D2D1_RECT_F dest = D2D1::RectF((px - w) / 2, (px - h) / 2, (px + w) / 2, (px + h) / 2);
            dc->DrawBitmap(bmp.get(), &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
        });
        c.icon.Brush(gfx_.Comp().CreateSurfaceBrush(surface));
    }

    LRESULT Handle(HWND, UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
        POINT p{};
        switch (msg) {
            case kMsgIcon:
                OnIcon(reinterpret_cast<IconResult*>(lp));
                handled = true;
                return 0;
            case WM_MOUSEMOVE: {
                GetCursorPos(&p);
                SetHover(CellAt(p));
                if (pressed_ >= 0 && cells_[pressed_].entry >= 0 && !dragging_ &&
                    std::hypot(p.x - pressPt_.x, p.y - pressPt_.y) > Px(6))
                    DragOut(pressed_);
                handled = true;
                return 0;
            }
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN: {
                GetCursorPos(&p);
                const int i = CellAt(p);
                const bool inside = mode_ == Mode::Fan ? i >= 0 : popup_.Contains(p);
                if (!inside) open_ = false;
                if (msg == WM_LBUTTONDOWN) {
                    pressed_ = i;
                    pressPt_ = p;
                }
                handled = true;
                return 0;
            }
            case WM_LBUTTONUP: {
                GetCursorPos(&p);
                const int i = CellAt(p);
                if (i >= 0 && i == pressed_) OpenCell(i);
                pressed_ = -1;
                handled = true;
                return 0;
            }
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE) open_ = false;
                if (wp == VK_RETURN && hover_ >= 0) OpenCell(hover_);
                handled = true;
                return 0;
            case WM_CAPTURECHANGED:
                if (!dragging_ && reinterpret_cast<HWND>(lp) != popup_.Hwnd()) open_ = false;
                return 0;
            case WM_ACTIVATE:
                if (!dragging_ && LOWORD(wp) == WA_INACTIVE) open_ = false;
                return 0;
            default:
                return 0;
        }
    }

    Graphics& gfx_;
    StackOptions o_;
    float s_;
    Mode mode_ = Mode::Grid;
    std::vector<FolderEntry> entries_;
    GlassPopup popup_;
    IconLoader loader_;
    std::vector<Cell> cells_;
    wuc::SpriteVisual panelSprite_{nullptr};
    int panelW_ = 0, panelH_ = 0;
    std::wstring title_, linkText_;
    winrt::com_ptr<IDWriteTextFormat> fontPill_, fontName_, fontRow_, fontTitle_, fontLink_;
    int hover_ = -1;
    int pressed_ = -1;
    POINT pressPt_{};
    bool dragging_ = false;
    bool open_ = true;
};

}  // namespace

void StackView::Run(Graphics& gfx, const StackOptions& options) {
    Session session(gfx, options);
    session.Run();
}

}  // namespace dx
