#pragma once
#include <windows.h>
#include <dwmapi.h>

#include <functional>
#include <vector>

#include "ui/genie.h"
#include "ui/layout.h"

namespace dx {

// Plays the macOS minimize animation for one window, drawn with live DWM
// thumbnails cut into strips (no code runs inside the other app).
// Driven by the dock's frame clock: 0 % CPU when nothing is minimizing.
class MinimizeAnimator {
public:
    MinimizeAnimator() = default;
    MinimizeAnimator(const MinimizeAnimator&) = delete;
    MinimizeAnimator& operator=(const MinimizeAnimator&) = delete;
    ~MinimizeAnimator() { Stop(); }

    // `reverse` plays it from the dock back to the window (restore). `done` runs at the end.
    bool Start(HWND window, RECT from, RECT to, Edge edge, MinimizeEffect effect, double seconds, bool reverse,
               std::function<void()> done = {});
    bool Step(double dt);  // false once finished
    void Stop();
    [[nodiscard]] bool Running() const { return host_ != nullptr; }

private:
    void Apply(double t);

    HWND host_ = nullptr;
    RECT box_{};  // the host window, screen pixels
    std::vector<HTHUMBNAIL> strips_;
    SIZE source_{};
    StripRect from_{}, to_{};  // in "canonical" space where the dock is at the bottom
    Edge edge_ = Edge::Bottom;
    MinimizeEffect effect_ = MinimizeEffect::Genie;
    double time_ = 0, duration_ = 0.5;
    bool reverse_ = false;
    std::function<void()> done_;
};

// The Windows "animate windows when minimizing and maximizing" setting.
// DockX turns it off while it draws its own animation, and restores it.
bool GetSystemMinimizeAnimation();
void SetSystemMinimizeAnimation(bool on);

}  // namespace dx
