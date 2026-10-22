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
    double separatorWidth = 12;  // slot width of a separator line, px
};

// One slot in the dock: an icon, or a separator line between sections.
struct LayoutEntry {
    double presence = 1;     // 0..1 while being added or removed
    bool separator = false;  // separators never magnify and have no icon
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

LayoutResult ComputeLayout(const std::vector<LayoutEntry>& entries, const LayoutParams& p);
// All icons, no separators. presence[i] in 0..1 scales an icon's slot while it is added or removed.
LayoutResult ComputeLayout(const std::vector<double>& presence, const LayoutParams& p);

// ---- Screen edges ----
// The dock is laid out in "dock space": u runs along the screen edge,
// v grows away from the edge (0 = the edge itself). These map it to the window.
enum class Edge { Bottom, Left, Right };

struct DockRect {
    double u = 0, v = 0, du = 0, dv = 0;
};
struct WinRect {
    double x = 0, y = 0, w = 0, h = 0;
};

Edge ParseEdge(const char* name);
WinRect ToWindow(Edge e, const DockRect& r, double winW, double winH);
void ToDock(Edge e, double x, double y, double winW, double winH, double& u, double& v);
// Moves something by `dv` away from (positive) or toward (negative) the edge, in window coords.
void EdgeOffset(Edge e, double dv, double& dx, double& dy);

}  // namespace dx
