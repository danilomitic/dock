#include "ui/layout.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace dx {

double MagnifyFalloff(double distance, double radius) {
    if (radius <= 0) return 0;
    const double t = std::abs(distance) / radius;
    if (t >= 1) return 0;
    return 0.5 * (1 + std::cos(std::numbers::pi * t));
}

LayoutResult ComputeLayout(const std::vector<double>& presence, const LayoutParams& p) {
    const size_t n = presence.size();
    LayoutResult r;
    r.center.resize(n);
    r.size.resize(n);
    r.slotLeft.resize(n);
    r.slotRight.resize(n);

    // Resting layout: each icon owns a slot of (icon + gap) * presence.
    std::vector<double> rest(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) rest[i + 1] = rest[i] + (p.iconSize + p.gap) * std::clamp(presence[i], 0.0, 1.0);
    const double restStart = p.centerX - rest[n] / 2;

    // Magnified slots: scale depends on the distance from the cursor in the resting layout.
    const double radius = p.range * (p.iconSize + p.gap);
    std::vector<double> scale(n, 1.0);
    std::vector<double> mag(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const double pr = std::clamp(presence[i], 0.0, 1.0);
        const double c = restStart + (rest[i] + rest[i + 1]) / 2;
        scale[i] = 1 + (p.maxScale - 1) * std::clamp(p.amount, 0.0, 1.0) * MagnifyFalloff(p.mouseX - c, radius);
        mag[i + 1] = mag[i] + (p.iconSize * scale[i] + p.gap) * pr;
    }

    // Keep the point under the cursor fixed, so the dock grows around it (like macOS).
    double start = restStart;
    if (n > 0 && rest[n] > 0) {
        const double mx = p.mouseX - restStart;
        if (mx <= 0) {
            start = restStart;
        } else if (mx >= rest[n]) {
            start = restStart + rest[n] - mag[n];
        } else {
            size_t k = 0;
            while (k + 1 < n && rest[k + 1] <= mx) ++k;
            const double w = rest[k + 1] - rest[k];
            const double t = w > 0 ? (mx - rest[k]) / w : 0;
            start = p.mouseX - (mag[k] + t * (mag[k + 1] - mag[k]));
        }
    }

    for (size_t i = 0; i < n; ++i) {
        const double pr = std::clamp(presence[i], 0.0, 1.0);
        r.slotLeft[i] = start + mag[i];
        r.slotRight[i] = start + mag[i + 1];
        r.center[i] = (r.slotLeft[i] + r.slotRight[i]) / 2;
        // Icons grow in from 60 % size while they are being added.
        r.size[i] = p.iconSize * scale[i] * (0.6 + 0.4 * pr);
    }
    r.start = start;
    r.end = start + mag[n];
    return r;
}

}  // namespace dx
