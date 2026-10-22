#pragma once
#include <windows.h>

#include <string>

#include "render/graphics.h"
#include "ui/layout.h"

namespace dx {

struct StackOptions {
    std::wstring folder;
    std::string view = "auto";  // "auto" | "fan" | "grid" | "list"
    std::string sort = "dateAdded";
    RECT icon{};                // the stack's dock icon, screen pixels
    Edge edge = Edge::Bottom;
    bool dark = true;
    double scale = 1;
    bool held = false;  // opened by pressing and holding: releasing over a file opens it
};

// Opens a Stack (a folder in the dock) as a macOS-style fan, grid or list.
// Click an item to open it, drag it out to drop it anywhere. Blocks until closed.
class StackView {
public:
    static void Run(Graphics& gfx, const StackOptions& options);
};

}  // namespace dx
