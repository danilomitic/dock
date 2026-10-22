// DockX Settings — a small macOS "System Settings"-style window.
// Every change is written to config.json and the running dock reloads it live.
#include <windows.h>
#include <unknwn.h>
#include <commdlg.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <windowsx.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/strings.h"

namespace {

using dx::Config;

constexpr UINT WM_DOCK_RELOAD = WM_APP + 5;
constexpr UINT_PTR kTimerFlush = 1;
constexpr UINT_PTR kTimerRestart = 2;
constexpr UINT_PTR kTimerStatus = 3;

constexpr float kSidebarW = 230;
constexpr float kRowH = 46;
constexpr float kRowSubH = 60;
constexpr float kCardPad = 6;
constexpr float kMargin = 28;

std::wstring ExeDir() {
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L'\\'));
}

HWND FindDock() { return FindWindowW(L"DockX.Window", nullptr); }

class App {
public:
    int Run(HINSTANCE inst);

private:
    struct Hit {
        enum class Kind { Toggle, Slider, Choice, Button, Nav } kind;
        int id;
        D2D1_RECT_F rect;
        std::function<void(float x)> act;
    };

    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Wnd(UINT, WPARAM, LPARAM);

    // Text
    const wchar_t* L(const wchar_t* sr, const wchar_t* en) const { return cfg_.language == "en" ? en : sr; }
    bool Dark() const { return cfg_.theme != "light"; }
    D2D1_COLOR_F Ink(float a = 0.92f) const { return Dark() ? D2D1::ColorF(1, 1, 1, a) : D2D1::ColorF(0, 0, 0, a * 0.95f); }
    D2D1_COLOR_F Accent(float a = 1) const { return D2D1::ColorF(accent_.R / 255.f, accent_.G / 255.f, accent_.B / 255.f, a); }

    // Drawing
    void CreateResources();
    void Paint();
    void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius = 0);
    void Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius, float width = 1);
    void Text(const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, D2D1_COLOR_F c);

    // Widgets (immediate mode: drawn and hit-tested every paint)
    void Header(const wchar_t* text);
    void BeginCard(int rows, int subRows = 0);
    void EndCard();
    D2D1_RECT_F Row(const wchar_t* label, const wchar_t* sub = nullptr);
    void Toggle(const wchar_t* label, bool value, std::function<void(bool)> set, const wchar_t* sub = nullptr);
    void Slider(const wchar_t* label, double value, double lo, double hi, double step, const wchar_t* fmt,
                std::function<void(double)> set, double shown = NAN);
    void Choice(const wchar_t* label, const std::vector<std::wstring>& options, int selected, std::function<void(int)> set);
    void Swatches(const wchar_t* label);
    void Buttons(const std::vector<std::pair<std::wstring, std::function<void()>>>& buttons);
    int NextId() { return nextId_++; }

    // Pages
    void Sidebar();
    void PageAppearance();
    void PageAnimation();
    void PageBehavior();
    void PageAdvanced();

    // Saving
    void Changed();
    void Flush();
    void ApplyPreset(int preset);
    void Export();
    void Import();
    void RestartDock();

    HWND hwnd_ = nullptr;
    Config cfg_;
    winrt::Windows::UI::Color accent_{255, 10, 132, 255};
    winrt::com_ptr<ID2D1Factory1> factory_;
    winrt::com_ptr<ID2D1HwndRenderTarget> rt_;
    winrt::com_ptr<ID2D1SolidColorBrush> brush_;
    winrt::com_ptr<IDWriteFactory> dw_;
    winrt::com_ptr<IDWriteTextFormat> fTitle_, fHeader_, fBody_, fSub_, fNav_, fGlyph_, fCenter_, fValue_;

    std::vector<Hit> hits_;
    int nextId_ = 0;
    int hot_ = -1, active_ = -1;
    int section_ = 0;
    float scroll_ = 0, contentH_ = 0;
    float W_ = 900, H_ = 680;
    // layout cursor
    float y_ = 0, cardX_ = 0, cardW_ = 0, cardTop_ = 0;
    int cardRow_ = 0;
    bool dirty_ = false;
    bool dockRunning_ = false;
};

// ---------------------------------------------------------------- setup

int App::Run(HINSTANCE inst) {
    cfg_ = dx::LoadConfig();
    try {
        accent_ = winrt::Windows::UI::ViewManagement::UISettings().GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Accent);
    } catch (...) {
    }
    D2D1_FACTORY_OPTIONS fo{};
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, factory_.put_void());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw_.put()));
    auto fmt = [this](winrt::com_ptr<IDWriteTextFormat>& f, const wchar_t* family, float size, DWRITE_FONT_WEIGHT w,
                      DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        dw_->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", f.put());
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetTextAlignment(align);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    fmt(fTitle_, L"Segoe UI Variable Display", 26, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fmt(fHeader_, L"Segoe UI Variable Text", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    fmt(fBody_, L"Segoe UI Variable Text", 14, DWRITE_FONT_WEIGHT_NORMAL);
    fmt(fSub_, L"Segoe UI Variable Text", 12, DWRITE_FONT_WEIGHT_NORMAL);
    fmt(fNav_, L"Segoe UI Variable Text", 14, DWRITE_FONT_WEIGHT_NORMAL);
    fmt(fGlyph_, L"Segoe Fluent Icons", 16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt(fCenter_, L"Segoe UI Variable Text", 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt(fValue_, L"Segoe UI Variable Text", 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"DockX.Settings";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem();
    const int w = MulDiv(920, dpi, 96), h = MulDiv(700, dpi, 96);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"DockX", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr,
                            nullptr, inst, this);
    if (!hwnd_) return 1;

    // macOS-like window: Mica material, dark title bar in the dark theme.
    const BOOL dark = Dark();
    DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    const int mica = 2;  // DWMSBT_MAINWINDOW
    DwmSetWindowAttribute(hwnd_, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &mica, sizeof(mica));
    const MARGINS m{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd_, &m);

    dockRunning_ = FindDock() != nullptr;
    SetTimer(hwnd_, kTimerStatus, 2000, nullptr);
    ShowWindow(hwnd_, SW_SHOW);
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);  // some launchers start programs minimized
    SetForegroundWindow(hwnd_);
    UpdateWindow(hwnd_);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (dirty_) Flush();
    return 0;
}

void App::CreateResources() {
    if (rt_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    factory_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)), rt_.put());
    const UINT dpi = GetDpiForWindow(hwnd_);
    rt_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush_.put());
}

void App::Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius) {
    brush_->SetColor(c);
    if (radius > 0) rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.get());
    else rt_->FillRectangle(r, brush_.get());
}

void App::Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius, float width) {
    brush_->SetColor(c);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.get(), width);
}

void App::Text(const std::wstring& s, const D2D1_RECT_F& r, IDWriteTextFormat* f, D2D1_COLOR_F c) {
    brush_->SetColor(c);
    rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, r, brush_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// ---------------------------------------------------------------- widgets

void App::Header(const wchar_t* text) {
    y_ += 14;
    Text(text, D2D1::RectF(cardX_ + 6, y_, cardX_ + cardW_, y_ + 22), fHeader_.get(), Ink(0.6f));
    y_ += 26;
}

void App::BeginCard(int rows, int subRows) {
    cardTop_ = y_;
    cardRow_ = 0;
    const float h = (rows - subRows) * kRowH + subRows * kRowSubH + 2 * kCardPad;
    Fill(D2D1::RectF(cardX_, y_, cardX_ + cardW_, y_ + h), Dark() ? D2D1::ColorF(1, 1, 1, 0.055f) : D2D1::ColorF(1, 1, 1, 0.72f), 10);
    Stroke(D2D1::RectF(cardX_ + 0.5f, y_ + 0.5f, cardX_ + cardW_ - 0.5f, y_ + h - 0.5f),
           Dark() ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(0, 0, 0, 0.07f), 10);
    y_ += kCardPad;
}

void App::EndCard() { y_ += kCardPad + 8; }

// Draws the label (and an optional explanation) and returns the area for the control on the right.
D2D1_RECT_F App::Row(const wchar_t* label, const wchar_t* sub) {
    const float h = sub ? kRowSubH : kRowH;
    if (cardRow_++ > 0) Fill(D2D1::RectF(cardX_ + 16, y_, cardX_ + cardW_ - 16, y_ + 1), Ink(0.07f));
    if (sub) {
        Text(label, D2D1::RectF(cardX_ + 16, y_ + 8, cardX_ + cardW_ * 0.55f, y_ + 30), fBody_.get(), Ink());
        Text(sub, D2D1::RectF(cardX_ + 16, y_ + 30, cardX_ + cardW_ - 120, y_ + 50), fSub_.get(), Ink(0.55f));
    } else {
        Text(label, D2D1::RectF(cardX_ + 16, y_, cardX_ + cardW_ * 0.5f, y_ + h), fBody_.get(), Ink());
    }
    const D2D1_RECT_F control = D2D1::RectF(cardX_ + cardW_ * 0.45f, y_, cardX_ + cardW_ - 16, y_ + h);
    y_ += h;
    return control;
}

void App::Toggle(const wchar_t* label, bool value, std::function<void(bool)> set, const wchar_t* sub) {
    const D2D1_RECT_F area = Row(label, sub);
    const float cy = (area.top + area.bottom) / 2;
    const D2D1_RECT_F track = D2D1::RectF(area.right - 42, cy - 12, area.right, cy + 12);
    const int id = NextId();
    const bool hot = id == hot_;
    Fill(track, value ? Accent() : Ink(hot ? 0.28f : 0.2f), 12);
    const float kx = value ? track.right - 12 : track.left + 12;
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 1));
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), 9.5f, 9.5f), brush_.get());
    hits_.push_back({Hit::Kind::Toggle, id, D2D1::RectF(track.left - 6, area.top, track.right + 2, area.bottom),
                     [value, set](float) { set(!value); }});
}

void App::Slider(const wchar_t* label, double value, double lo, double hi, double step, const wchar_t* fmt,
                 std::function<void(double)> set, double shown) {
    const D2D1_RECT_F area = Row(label);
    const float cy = (area.top + area.bottom) / 2;
    const float right = area.right - 70;
    const float left = std::max(area.left, right - 240);
    const float t = static_cast<float>(std::clamp((value - lo) / (hi - lo), 0.0, 1.0));
    const float kx = left + t * (right - left);
    const int id = NextId();
    Fill(D2D1::RectF(left, cy - 2, right, cy + 2), Ink(0.18f), 2);
    Fill(D2D1::RectF(left, cy - 2, kx, cy + 2), Accent(), 2);
    brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.25f));
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy + 1), 10, 10), brush_.get());
    brush_->SetColor(D2D1::ColorF(1, 1, 1, 1));
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), id == active_ || id == hot_ ? 10.0f : 9.0f, id == active_ || id == hot_ ? 10.0f : 9.0f),
                     brush_.get());
    wchar_t buf[48];
    swprintf_s(buf, fmt, std::isnan(shown) ? value : shown);
    Text(buf, D2D1::RectF(right + 8, area.top, area.right, area.bottom), fValue_.get(), Ink(0.7f));
    hits_.push_back({Hit::Kind::Slider, id, D2D1::RectF(left - 10, area.top, right + 10, area.bottom),
                     [left, right, lo, hi, step, set](float x) {
                         double v = lo + std::clamp((x - left) / (right - left), 0.0f, 1.0f) * (hi - lo);
                         v = std::round(v / step) * step;
                         set(std::clamp(v, lo, hi));
                     }});
}

void App::Choice(const wchar_t* label, const std::vector<std::wstring>& options, int selected, std::function<void(int)> set) {
    const D2D1_RECT_F area = Row(label);
    const float cy = (area.top + area.bottom) / 2;
    float total = 0;
    std::vector<float> widths;
    for (const auto& o : options) {
        winrt::com_ptr<IDWriteTextLayout> l;
        dw_->CreateTextLayout(o.c_str(), static_cast<UINT32>(o.size()), fCenter_.get(), 400, 30, l.put());
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        widths.push_back(m.width + 26);
        total += m.width + 26;
    }
    float x = area.right - total - 4;
    const D2D1_RECT_F box = D2D1::RectF(x - 2, cy - 15, area.right, cy + 15);
    Fill(box, Ink(0.08f), 8);
    for (size_t i = 0; i < options.size(); ++i) {
        const D2D1_RECT_F seg = D2D1::RectF(x, cy - 13, x + widths[i], cy + 13);
        const int id = NextId();
        if (static_cast<int>(i) == selected) {
            Fill(seg, Dark() ? D2D1::ColorF(1, 1, 1, 0.2f) : D2D1::ColorF(1, 1, 1, 1), 6);
        } else if (id == hot_) {
            Fill(seg, Ink(0.06f), 6);
        }
        Text(options[i], seg, fCenter_.get(), Ink(static_cast<int>(i) == selected ? 0.95f : 0.7f));
        hits_.push_back({Hit::Kind::Choice, id, seg, [i, set](float) { set(static_cast<int>(i)); }});
        x += widths[i];
    }
}

void App::Swatches(const wchar_t* label) {
    const D2D1_RECT_F area = Row(label);
    const float cy = (area.top + area.bottom) / 2;
    // Automatic (theme color), a few macOS-like tints, and "custom".
    static const uint32_t kColors[] = {0x282828, 0x1C1C2E, 0x0A3D62, 0x1E5631, 0x5B2C6F, 0x7B241C, 0xF4F4F4, 0xFFE4C4};
    float x = area.right - 11;
    // custom picker (rainbow ring)
    {
        const int id = NextId();
        const D2D1_POINT_2F c = D2D1::Point2F(x, cy);
        const bool custom = cfg_.glassTint && std::find(std::begin(kColors), std::end(kColors), *cfg_.glassTint) == std::end(kColors);
        const D2D1_COLOR_F ring[] = {D2D1::ColorF(1, 0.3f, 0.3f), D2D1::ColorF(1, 0.8f, 0.2f), D2D1::ColorF(0.3f, 0.85f, 0.4f),
                                     D2D1::ColorF(0.25f, 0.6f, 1), D2D1::ColorF(0.7f, 0.4f, 1)};
        for (int k = 0; k < 5; ++k) {
            brush_->SetColor(ring[k]);
            const float a0 = k * 6.2832f / 5, a1 = (k + 1) * 6.2832f / 5;
            winrt::com_ptr<ID2D1PathGeometry> g;
            factory_->CreatePathGeometry(g.put());
            winrt::com_ptr<ID2D1GeometrySink> s;
            g->Open(s.put());
            s->BeginFigure(c, D2D1_FIGURE_BEGIN_FILLED);
            s->AddLine(D2D1::Point2F(c.x + 10 * std::cos(a0), c.y + 10 * std::sin(a0)));
            s->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.x + 10 * std::cos(a1), c.y + 10 * std::sin(a1)), D2D1::SizeF(10, 10), 0,
                                       D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
            rt_->FillGeometry(g.get(), brush_.get());
        }
        if (custom || id == hot_) {
            brush_->SetColor(custom ? Accent() : Ink(0.4f));
            rt_->DrawEllipse(D2D1::Ellipse(c, 13, 13), brush_.get(), 2);
        }
        hits_.push_back({Hit::Kind::Button, id, D2D1::RectF(x - 13, cy - 13, x + 13, cy + 13), [this](float) {
                             static COLORREF custom[16]{};
                             const uint32_t cur = cfg_.glassTint.value_or(0x282828);
                             CHOOSECOLORW cc{sizeof(cc)};
                             cc.hwndOwner = hwnd_;
                             cc.rgbResult = RGB((cur >> 16) & 0xFF, (cur >> 8) & 0xFF, cur & 0xFF);
                             cc.lpCustColors = custom;
                             cc.Flags = CC_FULLOPEN | CC_RGBINIT;
                             if (ChooseColorW(&cc)) {
                                 cfg_.glassTint = (GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
                                 Changed();
                             }
                         }});
        x -= 30;
    }
    for (int i = static_cast<int>(std::size(kColors)) - 1; i >= -1; --i) {
        const int id = NextId();
        const D2D1_POINT_2F c = D2D1::Point2F(x, cy);
        const bool selected = i < 0 ? !cfg_.glassTint : (cfg_.glassTint && *cfg_.glassTint == kColors[i]);
        if (i < 0) {
            // "Auto": half dark / half light, follows the theme.
            brush_->SetColor(D2D1::ColorF(0.16f, 0.16f, 0.16f));
            rt_->FillEllipse(D2D1::Ellipse(c, 10, 10), brush_.get());
            brush_->SetColor(D2D1::ColorF(0.95f, 0.95f, 0.95f));
            rt_->PushAxisAlignedClip(D2D1::RectF(c.x, c.y - 11, c.x + 11, c.y + 11), D2D1_ANTIALIAS_MODE_ALIASED);
            rt_->FillEllipse(D2D1::Ellipse(c, 10, 10), brush_.get());
            rt_->PopAxisAlignedClip();
        } else {
            const uint32_t v = kColors[i];
            brush_->SetColor(D2D1::ColorF(((v >> 16) & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, (v & 0xFF) / 255.f));
            rt_->FillEllipse(D2D1::Ellipse(c, 10, 10), brush_.get());
        }
        brush_->SetColor(Ink(0.25f));
        rt_->DrawEllipse(D2D1::Ellipse(c, 10, 10), brush_.get(), 1);
        if (selected || id == hot_) {
            brush_->SetColor(selected ? Accent() : Ink(0.4f));
            rt_->DrawEllipse(D2D1::Ellipse(c, 13, 13), brush_.get(), 2);
        }
        hits_.push_back({Hit::Kind::Choice, id, D2D1::RectF(x - 13, cy - 13, x + 13, cy + 13), [this, i](float) {
                             if (i < 0) cfg_.glassTint.reset();
                             else cfg_.glassTint = kColors[i];
                             Changed();
                         }});
        x -= 28;
    }
}

void App::Buttons(const std::vector<std::pair<std::wstring, std::function<void()>>>& buttons) {
    const float h = kRowH;
    if (cardRow_++ > 0) Fill(D2D1::RectF(cardX_ + 16, y_, cardX_ + cardW_ - 16, y_ + 1), Ink(0.07f));
    float x = cardX_ + 14;
    for (const auto& [text, fn] : buttons) {
        winrt::com_ptr<IDWriteTextLayout> l;
        dw_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fCenter_.get(), 400, 30, l.put());
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        const D2D1_RECT_F b = D2D1::RectF(x, y_ + 9, x + m.width + 28, y_ + h - 9);
        const int id = NextId();
        Fill(b, id == active_ ? Ink(0.2f) : id == hot_ ? Ink(0.14f) : Ink(0.09f), 7);
        Text(text, b, fCenter_.get(), Ink());
        auto action = fn;
        hits_.push_back({Hit::Kind::Button, id, b, [action](float) { action(); }});
        x = b.right + 10;
    }
    y_ += h;
}

// ---------------------------------------------------------------- pages

void App::Sidebar() {
    Fill(D2D1::RectF(0, 0, kSidebarW, H_), Dark() ? D2D1::ColorF(0, 0, 0, 0.18f) : D2D1::ColorF(1, 1, 1, 0.35f));
    Fill(D2D1::RectF(kSidebarW - 1, 0, kSidebarW, H_), Ink(0.08f));
    // App badge
    Fill(D2D1::RectF(20, 22, 56, 58), Accent(), 9);
    Text(L"", D2D1::RectF(20, 22, 56, 58), fGlyph_.get(), D2D1::ColorF(1, 1, 1));
    Text(L"DockX", D2D1::RectF(66, 22, kSidebarW, 42), fBody_.get(), Ink());
    Text(L(L"Podešavanja", L"Settings"), D2D1::RectF(66, 40, kSidebarW, 58), fSub_.get(), Ink(0.55f));

    struct Nav {
        const wchar_t* glyph;
        const wchar_t* sr;
        const wchar_t* en;
    };
    const Nav nav[] = {{L"", L"Izgled", L"Appearance"},
                       {L"", L"Animacije", L"Animations"},
                       {L"", L"Ponašanje", L"Behavior"},
                       {L"", L"Napredno", L"Advanced"}};
    float y = 84;
    for (int i = 0; i < 4; ++i) {
        const D2D1_RECT_F r = D2D1::RectF(12, y, kSidebarW - 12, y + 34);
        const int id = NextId();
        if (i == section_) Fill(r, Accent(), 7);
        else if (id == hot_) Fill(r, Ink(0.07f), 7);
        const D2D1_COLOR_F c = i == section_ ? D2D1::ColorF(1, 1, 1) : Ink();
        Text(nav[i].glyph, D2D1::RectF(r.left + 6, r.top, r.left + 34, r.bottom), fGlyph_.get(), c);
        Text(L(nav[i].sr, nav[i].en), D2D1::RectF(r.left + 40, r.top, r.right, r.bottom), fNav_.get(), c);
        hits_.push_back({Hit::Kind::Nav, id, r, [this, i](float) {
                             section_ = i;
                             scroll_ = 0;
                         }});
        y += 38;
    }

    // Dock status at the bottom.
    const D2D1_RECT_F st = D2D1::RectF(16, H_ - 92, kSidebarW - 16, H_ - 64);
    brush_->SetColor(dockRunning_ ? D2D1::ColorF(0.2f, 0.78f, 0.35f) : D2D1::ColorF(0.9f, 0.3f, 0.3f));
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(st.left + 6, (st.top + st.bottom) / 2), 4.5f, 4.5f), brush_.get());
    Text(dockRunning_ ? L(L"Dock radi", L"Dock is running") : L(L"Dock nije pokrenut", L"Dock is not running"),
         D2D1::RectF(st.left + 18, st.top, st.right, st.bottom), fSub_.get(), Ink(0.7f));
    const D2D1_RECT_F b = D2D1::RectF(16, H_ - 58, kSidebarW - 16, H_ - 24);
    const int id = NextId();
    Fill(b, id == hot_ ? Ink(0.14f) : Ink(0.09f), 7);
    Text(dockRunning_ ? L(L"Restartuj Dock", L"Restart Dock") : L(L"Pokreni Dock", L"Start Dock"), b, fCenter_.get(), Ink());
    hits_.push_back({Hit::Kind::Button, id, b, [this](float) {
                         if (dockRunning_) RestartDock();
                         else ShellExecuteW(nullptr, L"open", (ExeDir() + L"\\DockX.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                     }});
}

void App::PageAppearance() {
    Text(L(L"Izgled", L"Appearance"), D2D1::RectF(cardX_, y_, cardX_ + cardW_, y_ + 40), fTitle_.get(), Ink());
    y_ += 44;
    BeginCard(5);
    Choice(L(L"Tema", L"Theme"), {L(L"Tamna", L"Dark"), L(L"Svetla", L"Light")}, cfg_.theme == "light" ? 1 : 0, [this](int i) {
        cfg_.theme = i ? "light" : "dark";
        Changed();
    });
    Slider(L(L"Veličina ikonica", L"Icon size"), cfg_.iconSize, 24, 96, 1, L"%.0f px", [this](double v) {
        cfg_.iconSize = v;
        cfg_.magnifiedSize = std::max(cfg_.magnifiedSize, v);
        Changed();
    });
    Slider(L(L"Razmak između ikonica", L"Space between icons"), cfg_.spacing, 0, 20, 1, L"%.0f px", [this](double v) {
        cfg_.spacing = v;
        Changed();
    });
    Slider(L(L"Margina docka", L"Dock padding"), cfg_.shelfPadding, 0, 24, 1, L"%.0f px", [this](double v) {
        cfg_.shelfPadding = v;
        Changed();
    });
    Slider(L(L"Zaobljenost ivica", L"Corner radius"), cfg_.cornerRadius, 0, 32, 1, L"%.0f px", [this](double v) {
        cfg_.cornerRadius = v;
        Changed();
    });
    EndCard();

    Header(L(L"STAKLO", L"GLASS"));
    BeginCard(6);
    Toggle(L(L"Zamagljeno staklo", L"Blurred glass"), cfg_.blur, [this](bool v) {
        cfg_.blur = v;
        Changed();
    });
    Swatches(L(L"Boja stakla", L"Glass color"));
    Slider(L(L"Gustina stakla", L"Glass density"), cfg_.glassOpacity, 0, 1, 0.01, L"%.0f %%", [this](double v) {
        cfg_.glassOpacity = v;
        Changed();
    }, cfg_.glassOpacity * 100);
    Toggle(L(L"Svetla ivica", L"Light edge"), cfg_.border, [this](bool v) {
        cfg_.border = v;
        Changed();
    });
    Toggle(L(L"Senka", L"Shadow"), cfg_.shadow, [this](bool v) {
        cfg_.shadow = v;
        Changed();
    });
    Slider(L(L"Jačina senke", L"Shadow strength"), cfg_.shadowOpacity, 0, 1, 0.01, L"%.0f %%", [this](double v) {
        cfg_.shadowOpacity = v;
        Changed();
    }, cfg_.shadowOpacity * 100);
    EndCard();

    Header(L(L"IKONICE", L"ICONS"));
    BeginCard(2);
    Toggle(L(L"Tačkice ispod otvorenih aplikacija", L"Dots under open apps"), cfg_.showDots, [this](bool v) {
        cfg_.showDots = v;
        Changed();
    });
    Toggle(L(L"Nazivi iznad ikonica", L"Names above icons"), cfg_.showLabels, [this](bool v) {
        cfg_.showLabels = v;
        Changed();
    });
    EndCard();
}

void App::PageAnimation() {
    Text(L(L"Animacije", L"Animations"), D2D1::RectF(cardX_, y_, cardX_ + cardW_, y_ + 40), fTitle_.get(), Ink());
    y_ += 44;
    BeginCard(4);
    Toggle(L(L"Uvećavanje", L"Magnification"), cfg_.magnification, [this](bool v) {
        cfg_.magnification = v;
        Changed();
    });
    Slider(L(L"Veličina uvećanja", L"Magnified size"), cfg_.magnifiedSize, cfg_.iconSize, 160, 1, L"%.0f px", [this](double v) {
        cfg_.magnifiedSize = v;
        Changed();
    });
    Slider(L(L"Koliko ikonica raste", L"Icons affected"), cfg_.magnificationRange, 1, 6, 0.5, L"%.1f", [this](double v) {
        cfg_.magnificationRange = v;
        Changed();
    });
    Slider(L(L"Brzina uvećanja", L"Magnification speed"), cfg_.magnifyResponse, 0.08, 0.6, 0.01, L"%.2f s", [this](double v) {
        cfg_.magnifyResponse = v;
        Changed();
    });
    EndCard();

    Header(L(L"POKRET", L"MOTION"));
    BeginCard(3);
    Toggle(L(L"Skakutanje pri pokretanju", L"Bounce when opening apps"), cfg_.launchBounce, [this](bool v) {
        cfg_.launchBounce = v;
        Changed();
    });
    Toggle(L(L"Skakutanje kad aplikacija traži pažnju", L"Bounce when an app needs attention"), cfg_.attentionBounce,
           [this](bool v) {
               cfg_.attentionBounce = v;
               Changed();
           });
    Slider(L(L"Brzina svih animacija", L"Animation speed"), cfg_.animationSpeed, 0.5, 2, 0.1, L"%.1f×", [this](double v) {
        cfg_.animationSpeed = v;
        Changed();
    });
    EndCard();
}

void App::PageBehavior() {
    Text(L(L"Ponašanje", L"Behavior"), D2D1::RectF(cardX_, y_, cardX_ + cardW_, y_ + 40), fTitle_.get(), Ink());
    y_ += 44;
    BeginCard(6, 1);
    const int pos = cfg_.position == "left" ? 0 : cfg_.position == "right" ? 2 : 1;
    Choice(L(L"Položaj na ekranu", L"Position on screen"), {L(L"Levo", L"Left"), L(L"Dole", L"Bottom"), L(L"Desno", L"Right")}, pos,
           [this](int i) {
               cfg_.position = i == 0 ? "left" : i == 2 ? "right" : "bottom";
               Changed();
           });
    Toggle(L(L"Rezerviši prostor na ekranu", L"Reserve screen space"), cfg_.reserveSpace, [this](bool v) {
        cfg_.reserveSpace = v;
        Changed();
    }, L(L"Maksimizovani prozori staju iznad docka. Isključi ako ne želiš da se pomeraju ikonice na desktopu.",
         L"Maximized windows stop above the dock. Turn off to keep desktop icons where they are."));
    Toggle(L(L"Automatski sakrij Dock", L"Automatically hide the Dock"), cfg_.autoHide, [this](bool v) {
        cfg_.autoHide = v;
        Changed();
    });
    Slider(L(L"Kašnjenje pojavljivanja", L"Show delay"), cfg_.showDelay, 0, 1, 0.05, L"%.2f s", [this](double v) {
        cfg_.showDelay = v;
        Changed();
    });
    Slider(L(L"Kašnjenje sakrivanja", L"Hide delay"), cfg_.hideDelay, 0, 2, 0.05, L"%.2f s", [this](double v) {
        cfg_.hideDelay = v;
        Changed();
    });
    Toggle(L(L"Sakrij kad je aplikacija preko celog ekrana", L"Hide when an app is fullscreen"), cfg_.hideInFullscreen,
           [this](bool v) {
               cfg_.hideInFullscreen = v;
               Changed();
           });
    EndCard();

    Header(L(L"WINDOWS", L"WINDOWS"));
    BeginCard(2);
    Toggle(L(L"Sakrij Windows taskbar", L"Hide the Windows taskbar"), cfg_.hideTaskbar, [this](bool v) {
        cfg_.hideTaskbar = v;
        Changed();
    });
    Toggle(L(L"Pokreni sa Windowsom", L"Start with Windows"), cfg_.startWithWindows, [this](bool v) {
        cfg_.startWithWindows = v;
        Changed();
    });
    EndCard();

    Header(L(L"DESNA STRANA DOCKA", L"RIGHT SIDE OF THE DOCK"));
    BeginCard(5);
    Toggle(L(L"Prikaži nedavne aplikacije", L"Show recent apps"), cfg_.showRecent, [this](bool v) {
        cfg_.showRecent = v;
        Changed();
    });
    Slider(L(L"Broj nedavnih aplikacija", L"Number of recent apps"), cfg_.recentCount, 0, 10, 1, L"%.0f", [this](double v) {
        cfg_.recentCount = static_cast<int>(v);
        Changed();
    });
    Toggle(L(L"Korpa u docku", L"Recycle Bin in the Dock"), cfg_.showTrash, [this](bool v) {
        cfg_.showTrash = v;
        Changed();
    });
    const std::vector<std::string> views{"auto", "fan", "grid", "list"};
    const int view = static_cast<int>(std::find(views.begin(), views.end(), cfg_.stackView) - views.begin());
    Choice(L(L"Prikaz fascikle", L"Stack view"),
           {L(L"Automatski", L"Automatic"), L(L"Lepeza", L"Fan"), L(L"Mreža", L"Grid"), L(L"Lista", L"List")}, view, [this, views](int i) {
               cfg_.stackView = views[i];
               Changed();
           });
    const std::vector<std::string> sorts{"dateAdded", "name", "dateModified", "kind"};
    const int sort = static_cast<int>(std::find(sorts.begin(), sorts.end(), cfg_.stackSort) - sorts.begin());
    Choice(L(L"Sortiranje fascikle", L"Stack sorting"),
           {L(L"Dodato", L"Added"), L(L"Ime", L"Name"), L(L"Izmena", L"Modified"), L(L"Vrsta", L"Kind")}, sort, [this, sorts](int i) {
               cfg_.stackSort = sorts[i];
               Changed();
           });
    EndCard();
}

void App::PageAdvanced() {
    Text(L(L"Napredno", L"Advanced"), D2D1::RectF(cardX_, y_, cardX_ + cardW_, y_ + 40), fTitle_.get(), Ink());
    y_ += 44;
    BeginCard(2);
    Choice(L(L"Jezik", L"Language"), {L"Srpski", L"English"}, cfg_.language == "en" ? 1 : 0, [this](int i) {
        cfg_.language = i ? "en" : "sr";
        Changed();
    });
    Toggle(L(L"Log fajl (za prijavu grešaka)", L"Log file (for bug reports)"), cfg_.logging, [this](bool v) {
        cfg_.logging = v;
        Changed();
    });
    EndCard();

    Header(L(L"UNAPRED PODEŠENO", L"PRESETS"));
    BeginCard(1);
    Buttons({{L"macOS", [this] { ApplyPreset(0); }},
             {L(L"Minimalno", L"Minimal"), [this] { ApplyPreset(1); }},
             {L(L"Lite (slabiji računar)", L"Lite (slower PC)"), [this] { ApplyPreset(2); }},
             {L(L"Veliko", L"Large"), [this] { ApplyPreset(3); }}});
    EndCard();

    Header(L(L"PODEŠAVANJA", L"SETTINGS"));
    BeginCard(2);
    Buttons({{L(L"Vrati podrazumevano", L"Reset to defaults"), [this] { ApplyPreset(0); }},
             {L(L"Izvezi…", L"Export…"), [this] { Export(); }},
             {L(L"Uvezi…", L"Import…"), [this] { Import(); }}});
    Buttons({{L(L"Otvori config fajl", L"Open config file"),
              [] { ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + dx::ConfigPath() + L"\"").c_str(), nullptr, SW_SHOWNORMAL); }},
             {L(L"Restartuj Dock", L"Restart Dock"), [this] { RestartDock(); }}});
    EndCard();
}

// ---------------------------------------------------------------- paint & input

void App::Paint() {
    CreateResources();
    hits_.clear();
    nextId_ = 0;
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));  // the Mica backdrop shows through
    Sidebar();

    const D2D1_RECT_F content = D2D1::RectF(kSidebarW, 0, W_, H_);
    rt_->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_ALIASED);
    cardX_ = kSidebarW + kMargin;
    cardW_ = std::max(360.0f, W_ - kSidebarW - 2 * kMargin);
    y_ = 26 - scroll_;
    const size_t navHits = hits_.size();
    switch (section_) {
        case 0: PageAppearance(); break;
        case 1: PageAnimation(); break;
        case 2: PageBehavior(); break;
        default: PageAdvanced(); break;
    }
    contentH_ = y_ + scroll_ + 20;
    rt_->PopAxisAlignedClip();
    // Content hits outside the visible area are ignored.
    std::erase_if(hits_, [&, i = size_t{0}](const Hit& h) mutable {
        return i++ >= navHits && (h.rect.bottom < 0 || h.rect.top > H_);
    });
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        rt_ = nullptr;
        brush_ = nullptr;
    }
}

LRESULT CALLBACK App::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        if (!self->hwnd_) self->hwnd_ = hwnd;
        return self->Wnd(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::Wnd(UINT msg, WPARAM wp, LPARAM lp) {
    const float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
    auto dip = [dpi](int px) { return px * 96.0f / dpi; };
    auto hitAt = [this](float x, float y) -> const Hit* {
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
            if (x >= it->rect.left && x < it->rect.right && y >= it->rect.top && y < it->rect.bottom) return &*it;
        return nullptr;
    };
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            Paint();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            W_ = dip(LOWORD(lp));
            H_ = dip(HIWORD(lp));
            if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentH_ - H_));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            const auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            rt_ = nullptr;
            brush_ = nullptr;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {static_cast<LONG>(720 * dpi / 96), static_cast<LONG>(520 * dpi / 96)};
            return 0;
        }
        case WM_MOUSEWHEEL:
            scroll_ = std::clamp(scroll_ - GET_WHEEL_DELTA_WPARAM(wp) * 0.5f, 0.0f, std::max(0.0f, contentH_ - H_));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            const float x = dip(GET_X_LPARAM(lp)), y = dip(GET_Y_LPARAM(lp));
            if (active_ >= 0) {
                for (const Hit& h : hits_)
                    if (h.id == active_ && h.kind == Hit::Kind::Slider) h.act(x);
            }
            const Hit* h = hitAt(x, y);
            const int hot = h ? h->id : -1;
            if (hot != hot_ || active_ >= 0) {
                hot_ = hot;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSELEAVE:
            hot_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const float x = dip(GET_X_LPARAM(lp)), y = dip(GET_Y_LPARAM(lp));
            if (const Hit* h = hitAt(x, y)) {
                active_ = h->id;
                SetCapture(hwnd_);
                if (h->kind == Hit::Kind::Slider || h->kind == Hit::Kind::Choice || h->kind == Hit::Kind::Nav) h->act(x);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            const float x = dip(GET_X_LPARAM(lp)), y = dip(GET_Y_LPARAM(lp));
            const int was = active_;
            active_ = -1;
            ReleaseCapture();
            if (const Hit* h = hitAt(x, y); h && h->id == was && (h->kind == Hit::Kind::Toggle || h->kind == Hit::Kind::Button)) {
                const auto act = h->act;  // copy: acting may repaint
                act(x);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerFlush) {
                KillTimer(hwnd_, kTimerFlush);
                Flush();
            } else if (wp == kTimerRestart) {
                if (!FindDock()) {
                    KillTimer(hwnd_, kTimerRestart);
                    ShellExecuteW(nullptr, L"open", (ExeDir() + L"\\DockX.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            } else if (wp == kTimerStatus) {
                const bool running = FindDock() != nullptr;
                if (running != dockRunning_) {
                    dockRunning_ = running;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ---------------------------------------------------------------- saving

// Changes are written at most every 40 ms (sliders) and the dock reloads them live.
void App::Changed() {
    dirty_ = true;
    SetTimer(hwnd_, kTimerFlush, 40, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void App::Flush() {
    dirty_ = false;
    // The dock owns its app lists: keep whatever is in the file now.
    const Config cur = dx::LoadConfig();
    Config out = cfg_;
    out.pinned = cur.pinned;
    out.pinnedInitialized = cur.pinnedInitialized;
    out.recent = cur.recent;
    out.stacks = cur.stacks;
    out.stacksInitialized = cur.stacksInitialized;
    out.taskbarHiddenByDock = cur.taskbarHiddenByDock;
    out.taskbarOriginalState = cur.taskbarOriginalState;
    out.shortcutsCreated = cur.shortcutsCreated;
    out.magnifiedSize = std::max(out.magnifiedSize, out.iconSize);
    dx::SaveConfig(out);
    if (HWND dock = FindDock()) PostMessageW(dock, WM_DOCK_RELOAD, 0, 0);
}

void App::ApplyPreset(int preset) {
    const Config defaults;
    const std::string language = cfg_.language;
    const bool startup = cfg_.startWithWindows;
    // Keep where the dock is and what is in it; reset how it looks and moves.
    const std::string position = cfg_.position;
    cfg_ = defaults;
    cfg_.language = language;
    cfg_.startWithWindows = startup;
    cfg_.position = position;
    switch (preset) {
        case 1:  // Minimal
            cfg_.iconSize = 40;
            cfg_.magnification = false;
            cfg_.shadow = false;
            cfg_.border = false;
            cfg_.glassOpacity = 0.25;
            cfg_.showRecent = false;
            break;
        case 2:  // Lite: fewer effects for slower PCs
            cfg_.blur = false;
            cfg_.magnification = false;
            cfg_.shadow = false;
            cfg_.launchBounce = false;
            cfg_.attentionBounce = false;
            break;
        case 3:  // Large
            cfg_.iconSize = 64;
            cfg_.magnifiedSize = 112;
            cfg_.cornerRadius = 20;
            break;
        default: break;
    }
    Changed();
}

void App::Export() {
    wchar_t file[MAX_PATH] = L"DockX-settings.json";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"JSON\0*.json\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = L"json";
    if (!GetSaveFileNameW(&ofn)) return;
    Flush();
    CopyFileW(dx::ConfigPath().c_str(), file, FALSE);
}

void App::Import() {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"JSON\0*.json\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    std::ifstream f(file, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    cfg_ = dx::ParseConfig(ss.str());  // only the settings are used; Flush keeps the dock's apps
    Changed();
}

void App::RestartDock() {
    if (dirty_) Flush();
    if (HWND dock = FindDock()) PostMessageW(dock, WM_CLOSE, 0, 0);
    SetTimer(hwnd_, kTimerRestart, 250, nullptr);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // One settings window: bring the existing one forward.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DockX.Settings");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(L"DockX.Settings", nullptr)) {
            ShowWindow(w, SW_RESTORE);
            SetForegroundWindow(w);
        }
        return 0;
    }
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    App app;
    const int code = app.Run(inst);
    if (mutex) CloseHandle(mutex);
    return code;
}
