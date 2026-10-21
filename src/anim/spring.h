#pragma once
#include <cmath>
#include <numbers>

namespace dx {

// Damped spring described the Apple way: damping ratio (1 = no overshoot)
// and response (seconds to reach the target, roughly). Changing the target
// keeps the current value and velocity, so motion is always interruptible.
struct Spring {
    double value = 0;
    double velocity = 0;
    double target = 0;
    double damping = 1.0;
    double response = 0.3;

    void Step(double dt) {
        if (dt <= 0) return;
        const double w = 2.0 * std::numbers::pi / response;
        const double k = w * w;
        const double c = 2.0 * damping * w;
        // Fixed 1 ms sub-steps keep the integration stable at any frame rate.
        const int n = static_cast<int>(std::ceil(dt / 0.001));
        const double h = dt / n;
        for (int i = 0; i < n; ++i) {
            const double a = -k * (value - target) - c * velocity;
            velocity += a * h;
            value += velocity * h;
        }
    }

    [[nodiscard]] bool Settled(double eps) const {
        return std::abs(value - target) < eps && std::abs(velocity) < eps * 10;
    }

    void Snap() {
        value = target;
        velocity = 0;
    }
};

}  // namespace dx
