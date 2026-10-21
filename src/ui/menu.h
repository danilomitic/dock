#pragma once
#include <windows.h>

#include <string>
#include <vector>

#include "render/graphics.h"

namespace dx {

struct MenuItem {
    std::wstring text;
    UINT id = 0;
    bool checked = false;
    bool separator = false;
    bool disabled = false;
    std::vector<MenuItem> sub;

    static MenuItem Separator() {
        MenuItem m;
        m.separator = true;
        return m;
    }
};

// Where the menu opens relative to the anchor point.
enum class MenuSide { Above, Right, Left, AtCursor };

// A macOS-style context menu: glass background, accent highlight, submenus,
// keyboard navigation. Run() blocks (pumping messages) until it closes and
// returns the chosen id, or 0 if it was dismissed.
class MacMenu {
public:
    static UINT Run(Graphics& gfx, const std::vector<MenuItem>& items, POINT anchor, MenuSide side, bool dark, double scale);
};

}  // namespace dx
