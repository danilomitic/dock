#include "ui/minimize.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"

namespace dx {
namespace {

constexpr int kGenieStrips = 72;

// "Canonical" space: the dock is always at the bottom, so one genie formula
// works for every edge. Left/right docks are rotated into it and back.
StripRect ToCanonical(Edge e, const RECT& r) {
    switch (e) {
        case Edge::Left: return {static_cast<double>(r.top), -static_cast<double>(r.right), static_cast<double>(r.bottom), -static_cast<double>(r.left)};
        case Edge::Right: return {-static_cast<double>(r.bottom), static_cast<double>(r.left), -static_cast<double>(r.top), static_cast<double>(r.right)};
        case Edge::Bottom:
        default: return {static_cast<double>(r.left), static_cast<double>(r.top), static_cast<double>(r.right), static_cast<double>(r.bottom)};
    }
}

StripRect FromCanonical(Edge e, const StripRect& c) {
    switch (e) {
        case Edge::Left: return {-c.bottom, c.left, -c.top, c.right};
        case Edge::Right: return {c.top, -c.right, c.bottom, -c.left};
        case Edge::Bottom:
        default: return c;
    }
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool MinimizeAnimator::Start(HWND window, RECT from, RECT to, Edge edge, MinimizeEffect effect, double seconds, bool reverse,
                             std::function<void()> done) {
    Stop();
    if (effect == MinimizeEffect::System || IsRectEmpty(&from)) return false;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = HostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DockX.Minimize";
        RegisterClassExW(&wc);
        registered = true;
    }
    // A transparent, click-through window that covers the whole path of the animation.
    UnionRect(&box_, &from, &to);
    InflateRect(&box_, 4, 4);
    host_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                            L"DockX.Minimize", L"", WS_POPUP, box_.left, box_.top, box_.right - box_.left,
                            box_.bottom - box_.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!host_) return false;

    const int count = effect == MinimizeEffect::Genie ? kGenieStrips : 1;
    for (int i = 0; i < count; ++i) {
        HTHUMBNAIL th = nullptr;
        if (FAILED(DwmRegisterThumbnail(host_, window, &th))) break;
        strips_.push_back(th);
    }
    if (strips_.empty() || FAILED(DwmQueryThumbnailSourceSize(strips_[0], &source_)) || source_.cx < 16 || source_.cy < 16) {
        Log(L"minimize: no thumbnail for this window (%ldx%ld)", source_.cx, source_.cy);
        Stop();
        return false;
    }

    edge_ = edge;
    effect_ = effect;
    from_ = ToCanonical(edge, from);
    to_ = ToCanonical(edge, to);
    duration_ = std::max(0.05, seconds);
    time_ = 0;
    reverse_ = reverse;
    done_ = std::move(done);
    Apply(reverse ? 1 : 0);
    ShowWindow(host_, SW_SHOWNOACTIVATE);
    return true;
}

bool MinimizeAnimator::Step(double dt) {
    if (!host_) return false;
    time_ += dt;
    const double t = std::min(1.0, time_ / duration_);
    Apply(reverse_ ? 1 - t : t);
    if (t < 1) return true;
    auto done = std::move(done_);
    Stop();
    if (done) done();
    return false;
}

void MinimizeAnimator::Apply(double t) {
    const int n = static_cast<int>(strips_.size());
    for (int i = 0; i < n; ++i) {
        const StripRect c = effect_ == MinimizeEffect::Genie ? GenieStrip(from_, to_, i, n, t) : ScaleRect(from_, to_, t);
        const StripRect s = FromCanonical(edge_, c);

        // Which part of the window this strip shows: rows for a bottom dock, columns for the sides.
        const double v0 = static_cast<double>(i) / n, v1 = static_cast<double>(i + 1) / n;
        RECT src{0, 0, source_.cx, source_.cy};
        if (edge_ == Edge::Bottom) {
            src.top = static_cast<LONG>(std::floor(v0 * source_.cy));
            src.bottom = static_cast<LONG>(std::ceil(v1 * source_.cy));
        } else if (edge_ == Edge::Left) {
            src.left = static_cast<LONG>(std::floor((1 - v1) * source_.cx));
            src.right = static_cast<LONG>(std::ceil((1 - v0) * source_.cx));
        } else {
            src.left = static_cast<LONG>(std::floor(v0 * source_.cx));
            src.right = static_cast<LONG>(std::ceil(v1 * source_.cx));
        }

        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        // Strips overlap by two pixels so no seams show between them.
        p.rcDestination = {static_cast<LONG>(std::floor(s.left)) - box_.left, static_cast<LONG>(std::floor(s.top)) - box_.top,
                           static_cast<LONG>(std::ceil(s.right)) - box_.left + (edge_ != Edge::Bottom ? 2 : 0),
                           static_cast<LONG>(std::ceil(s.bottom)) - box_.top + (edge_ == Edge::Bottom ? 2 : 0)};
        p.rcSource = src;
        p.fVisible = TRUE;
        p.fSourceClientAreaOnly = FALSE;
        // Scale fades out as it lands; the genie stays solid.
        const double fade = effect_ == MinimizeEffect::Scale ? std::clamp((1 - t) / 0.35, 0.0, 1.0) : std::clamp((1 - t) / 0.08, 0.0, 1.0);
        p.opacity = static_cast<BYTE>(255 * fade);
        DwmUpdateThumbnailProperties(strips_[i], &p);
    }
}

void MinimizeAnimator::Stop() {
    for (HTHUMBNAIL th : strips_) DwmUnregisterThumbnail(th);
    strips_.clear();
    if (host_) DestroyWindow(host_);
    host_ = nullptr;
}

bool GetSystemMinimizeAnimation() {
    ANIMATIONINFO ai{sizeof(ai)};
    SystemParametersInfoW(SPI_GETANIMATION, sizeof(ai), &ai, 0);
    return ai.iMinAnimate != 0;
}

void SetSystemMinimizeAnimation(bool on) {
    ANIMATIONINFO ai{sizeof(ai), on ? 1 : 0};
    SystemParametersInfoW(SPI_SETANIMATION, sizeof(ai), &ai, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

}  // namespace dx
