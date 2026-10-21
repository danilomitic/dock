#pragma once
#include <windows.h>

#include <vector>

#include "render/graphics.h"
#include "ui/layout.h"

namespace dx {

// App Exposé: a glass panel next to the dock icon with live previews of
// all of the app's windows. Blocks until closed; returns the chosen window.
class Expose {
public:
    static HWND Run(Graphics& gfx, const std::vector<HWND>& windows, RECT iconOnScreen, Edge edge, bool dark, double scale);
};

}  // namespace dx
