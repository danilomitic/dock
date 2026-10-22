#include "ui/genie.h"

#include <algorithm>
#include <cmath>

namespace dx {
namespace {
double Lerp(double a, double b, double t) { return a + (b - a) * t; }
double Smooth(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3 - 2 * x);
}
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

// The macOS genie, in two overlapping phases:
//  1. the window's sides bend into a smooth funnel that ends at the icon;
//  2. the content slides down that funnel and is squeezed into the icon.
// Each strip takes the funnel's width at its current height.
StripRect GenieStrip(const StripRect& from, const StripRect& to, int row, int rows, double t) {
    t = std::clamp(t, 0.0, 1.0);
    const double h = std::max(1.0, from.bottom - from.top);
    const double iconH = std::max(1.0, to.bottom - to.top);
    const double travel = std::max(0.0, to.top - from.top);  // how far the window's top must fall
    const double bend = EaseInOutCubic(t / 0.4);              // phase 1
    const double slide = EaseInOutCubic((t - 0.2) / 0.8);     // phase 2

    // Position along the funnel (0 = the window's top edge where it started).
    auto place = [&](double v) {
        const double s = v * h + slide * travel;
        const double inFunnel = to.top - from.top;
        if (s <= inFunnel) return from.top + s;
        return to.top + (s - inFunnel) * (iconH / h);  // inside the icon: squeezed to its height
    };
    const double v0 = static_cast<double>(row) / rows;
    const double v1 = static_cast<double>(row + 1) / rows;
    StripRect r;
    r.top = place(v0);
    r.bottom = place(v1);

    // The funnel: full window width at the top, curving smoothly to the icon's width.
    const double y = (r.top + r.bottom) / 2;
    const double depth = Smooth((y - from.top) / std::max(1.0, to.top - from.top));
    const double k = std::max(bend * depth, y >= to.top ? 1.0 : 0.0);
    r.left = Lerp(from.left, to.left, k);
    r.right = Lerp(from.right, to.right, k);
    return r;
}

StripRect ScaleRect(const StripRect& from, const StripRect& to, double t) {
    const double e = EaseInOutCubic(t);
    return {Lerp(from.left, to.left, e), Lerp(from.top, to.top, e), Lerp(from.right, to.right, e), Lerp(from.bottom, to.bottom, e)};
}

}  // namespace dx
