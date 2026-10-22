// Minimal unit tests for the pure parts (layout math, springs, config/JSON).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "anim/spring.h"
#include "core/config.h"
#include "core/json.h"
#include "ui/layout.h"

namespace {
int g_failed = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failed;                                                   \
        }                                                                 \
    } while (0)

bool Near(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }

void TestFalloff() {
    CHECK(Near(dx::MagnifyFalloff(0, 100), 1));
    CHECK(Near(dx::MagnifyFalloff(100, 100), 0));
    CHECK(Near(dx::MagnifyFalloff(-50, 100), 0.5));
    CHECK(dx::MagnifyFalloff(200, 100) == 0);
}

void TestRestLayoutCentered() {
    dx::LayoutParams p;
    p.iconSize = 48;
    p.gap = 6;
    p.centerX = 500;
    p.amount = 0;
    const auto r = dx::ComputeLayout({1, 1, 1, 1}, p);
    CHECK(Near((r.start + r.end) / 2, 500));
    CHECK(Near(r.end - r.start, 4 * 54));
    for (double s : r.size) CHECK(Near(s, 48));
}

void TestCursorPointStaysFixed() {
    dx::LayoutParams p;
    p.centerX = 500;
    p.amount = 1;
    p.maxScale = 2;
    const std::vector<double> presence(7, 1.0);
    for (double mx = 330; mx <= 670; mx += 7.3) {
        p.mouseX = mx;
        dx::LayoutParams rest = p;
        rest.amount = 0;
        const auto r0 = dx::ComputeLayout(presence, rest);
        const auto r1 = dx::ComputeLayout(presence, p);
        if (mx <= r0.start || mx >= r0.end) continue;
        // Same fractional position inside the same slot.
        for (size_t i = 0; i < presence.size(); ++i) {
            if (mx >= r0.slotLeft[i] && mx < r0.slotRight[i]) {
                const double t0 = (mx - r0.slotLeft[i]) / (r0.slotRight[i] - r0.slotLeft[i]);
                const double t1 = (mx - r1.slotLeft[i]) / (r1.slotRight[i] - r1.slotLeft[i]);
                CHECK(Near(t0, t1, 1e-9));
            }
        }
    }
}

void TestNoOverlapAndMonotonic() {
    dx::LayoutParams p;
    p.centerX = 400;
    p.amount = 1;
    p.maxScale = 2.5;
    p.mouseX = 410;
    const auto r = dx::ComputeLayout({1, 1, 0.3, 1, 1, 1}, p);
    for (size_t i = 0; i + 1 < r.center.size(); ++i) {
        CHECK(r.center[i] < r.center[i + 1]);
        CHECK(Near(r.slotRight[i], r.slotLeft[i + 1]));
    }
}

void TestContinuityAcrossSlots() {
    dx::LayoutParams p;
    p.centerX = 400;
    p.amount = 1;
    p.maxScale = 2;
    const std::vector<double> presence(5, 1.0);
    double prevStart = 0;
    bool first = true;
    for (double mx = 200; mx <= 600; mx += 0.25) {
        p.mouseX = mx;
        const auto r = dx::ComputeLayout(presence, p);
        if (!first) CHECK(std::abs(r.start - prevStart) < 2.0);  // no jumps
        prevStart = r.start;
        first = false;
    }
}

void TestSeparators() {
    dx::LayoutParams p;
    p.centerX = 500;
    p.separatorWidth = 12;
    p.amount = 1;
    p.maxScale = 2;
    const std::vector<dx::LayoutEntry> entries{{1, false}, {1, false}, {1, true}, {1, false}};
    for (double mx : {420.0, 470.0, 500.0, 530.0}) {
        p.mouseX = mx;
        const auto r = dx::ComputeLayout(entries, p);
        CHECK(Near(r.slotRight[2] - r.slotLeft[2], 12));  // separators keep their width under magnification
        CHECK(Near(r.size[2], 0));
        for (size_t i = 0; i + 1 < entries.size(); ++i) CHECK(Near(r.slotRight[i], r.slotLeft[i + 1]));
    }
    p.amount = 0;
    const auto rest = dx::ComputeLayout(entries, p);
    CHECK(Near(rest.end - rest.start, 3 * 54 + 12));
}

void TestEdges() {
    const double W = 300, H = 1000;  // a left/right dock window is tall
    for (dx::Edge e : {dx::Edge::Bottom, dx::Edge::Left, dx::Edge::Right}) {
        const double ww = e == dx::Edge::Bottom ? 1000 : W;
        const double wh = e == dx::Edge::Bottom ? W : H;
        const dx::DockRect r{100, 10, 48, 60};
        const dx::WinRect w = dx::ToWindow(e, r, ww, wh);
        // A point inside the rect maps back to the same dock coordinates.
        double u = 0, v = 0;
        dx::ToDock(e, w.x + w.w / 2, w.y + w.h / 2, ww, wh, u, v);
        CHECK(Near(u, r.u + r.du / 2));
        CHECK(Near(v, r.v + r.dv / 2));
        CHECK(w.x >= 0 && w.y >= 0 && w.x + w.w <= ww && w.y + w.h <= wh);
    }
    CHECK(dx::ParseEdge("left") == dx::Edge::Left);
    CHECK(dx::ParseEdge("bogus") == dx::Edge::Bottom);
    double ox = 0, oy = 0;
    dx::EdgeOffset(dx::Edge::Bottom, 10, ox, oy);
    CHECK(Near(oy, -10) && Near(ox, 0));
    dx::EdgeOffset(dx::Edge::Right, 10, ox, oy);
    CHECK(Near(ox, -10));
}

void TestSpring() {
    dx::Spring s;
    s.target = 1;
    s.response = 0.3;
    s.damping = 1;
    double maxV = 0;
    for (int i = 0; i < 240; ++i) {
        s.Step(1.0 / 120);
        maxV = std::max(maxV, s.value);
    }
    CHECK(s.Settled(1e-3));
    CHECK(maxV <= 1.0 + 1e-3);  // critically damped: no overshoot
}

void TestJsonAndConfig() {
    const auto v = dx::json::Parse(R"({"a": [1, 2.5, "x\u00e9"], "b": {"c": true}})");
    CHECK(v.has_value());
    CHECK(v->Find("a")->array->size() == 3);
    CHECK(v->Find("a")->array->at(2).string == "x\xC3\xA9");
    CHECK(v->Find("b")->Bool("c", false));
    CHECK(!dx::json::Parse("{\"a\": }").has_value());

    dx::Config c;
    c.iconSize = 52;
    c.theme = "light";
    c.position = "right";
    c.autoHide = true;
    c.taskbarOriginalState = 2;
    c.glassTint = 0x123456;
    c.pinned.push_back({L"C:\\Windows\\explorer.exe", L""});
    c.pinned.push_back({L"", L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App"});
    c.stacks.push_back(L"C:\\Users\\x\\Downloads");
    c.recent.push_back({L"C:\\app\\a.exe", L""});
    c.stackView = "grid";
    const dx::Config d = dx::ParseConfig(dx::SerializeConfig(c));
    CHECK(Near(d.iconSize, 52));
    CHECK(d.theme == "light");
    CHECK(d.position == "right");
    CHECK(d.autoHide);
    CHECK(d.taskbarOriginalState == 2);
    CHECK(d.glassTint && *d.glassTint == 0x123456);
    CHECK(d.pinnedInitialized && d.pinned.size() == 2);
    CHECK(d.pinned[0].path == L"C:\\Windows\\explorer.exe");
    CHECK(d.pinned[1].aumid == L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App");
    CHECK(d.stacksInitialized && d.stacks.size() == 1 && d.stacks[0] == L"C:\\Users\\x\\Downloads");
    CHECK(d.recent.size() == 1 && d.recent[0].path == L"C:\\app\\a.exe");
    CHECK(d.stackView == "grid");
}
}  // namespace

int main() {
    TestFalloff();
    TestRestLayoutCentered();
    TestCursorPointStaysFixed();
    TestNoOverlapAndMonotonic();
    TestContinuityAcrossSlots();
    TestEdges();
    TestSeparators();
    TestSpring();
    TestJsonAndConfig();
    if (g_failed) {
        std::printf("%d check(s) failed\n", g_failed);
        return EXIT_FAILURE;
    }
    std::printf("All tests passed\n");
    return EXIT_SUCCESS;
}
