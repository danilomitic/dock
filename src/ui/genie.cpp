#include "ui/genie.h"

#include <algorithm>
#include <cmath>

namespace dx {
namespace {
double Lerp(double a, double b, double t) { return a + (b - a) * t; }
double Smooth(double x) { return x * x * (3 - 2 * x); }
}  // namespace

MinimizeEffect ParseMinimizeEffect(const std::string& name) {
    if (name == "scale") return MinimizeEffect::Scale;
    if (name == "system") return MinimizeEffect::System;
    return MinimizeEffect::Genie;
}

double EaseInOutCubic(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x < 0.5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2;
}

StripRect GenieStrip(const StripRect& from, const StripRect& to, int row, int rows, double t) {
    t = std::clamp(t, 0.0, 1.0);
    const double v0 = static_cast<double>(row) / rows;
    const double v1 = static_cast<double>(row + 1) / rows;
    const double mid = (v0 + v1) / 2;
    const double fh = from.bottom - from.top, th = to.bottom - to.top;

    // Rows near the bottom start sliding down first.
    constexpr double kLag = 0.35;
    const double travel = EaseInOutCubic((t - kLag * (1 - mid)) / (1 - kLag));

    StripRect r;
    r.top = Lerp(from.top + v0 * fh, to.top + v0 * th, travel);
    r.bottom = Lerp(from.top + v1 * fh, to.top + v1 * th, travel);

    // The sides pinch in early, more the further down the funnel a row is.
    const double y = (r.top + r.bottom) / 2;
    const double funnel = std::clamp((y - from.top) / std::max(1.0, to.bottom - from.top), 0.0, 1.0);
    const double squeeze = std::max(EaseInOutCubic(t / 0.45) * Smooth(funnel), travel);
    r.left = Lerp(from.left, to.left, squeeze);
    r.right = Lerp(from.right, to.right, squeeze);
    return r;
}

StripRect ScaleRect(const StripRect& from, const StripRect& to, double t) {
    const double e = EaseInOutCubic(t);
    return {Lerp(from.left, to.left, e), Lerp(from.top, to.top, e), Lerp(from.right, to.right, e), Lerp(from.bottom, to.bottom, e)};
}

}  // namespace dx
