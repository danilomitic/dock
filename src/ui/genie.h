#pragma once
#include <string>

namespace dx {

enum class MinimizeEffect { Genie, Scale, System };

MinimizeEffect ParseMinimizeEffect(const std::string& name);  // "genie" | "scale" | "system"

struct StripRect {
    double left = 0, top = 0, right = 0, bottom = 0;
};

// The macOS genie shape for a dock at the bottom: where horizontal strip `row`
// (of `rows`) of a window moving from `from` into `to` is at time t in 0..1.
// The bottom of the window narrows first and pours into the icon.
StripRect GenieStrip(const StripRect& from, const StripRect& to, int row, int rows, double t);

// The Scale effect: the whole window shrinks into the icon.
StripRect ScaleRect(const StripRect& from, const StripRect& to, double t);

double EaseInOutCubic(double x);

}  // namespace dx
