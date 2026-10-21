#pragma once
#include <vector>

namespace dx {

// Pure macOS-style dock layout math (no UI), so it can be unit-tested.

struct LayoutParams {
    double iconSize = 48;      // base icon size, px
    double gap = 6;            // space between icons, px
    double maxScale = 1.75;    // magnified size / base size
    double range = 3;          // icons affected on each side of the cursor
    double amount = 0;         // magnification strength 0..1 (animated)
    double mouseX = 0;         // cursor x, px
    double centerX = 0;        // where the resting dock is centered, px
};

struct LayoutResult {
    std::vector<double> center;  // icon center x
    std::vector<double> size;    // icon size (square)
    std::vector<double> slotLeft;
    std::vector<double> slotRight;
    double start = 0;            // left edge of the first slot
    double end = 0;              // right edge of the last slot
};

// 1 at distance 0, falling smoothly to 0 at `radius` (cosine bell).
double MagnifyFalloff(double distance, double radius);

// presence[i] in 0..1 scales an icon's slot while it is added or removed.
LayoutResult ComputeLayout(const std::vector<double>& presence, const LayoutParams& p);

}  // namespace dx
