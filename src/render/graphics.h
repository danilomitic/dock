#pragma once
#include <windows.h>
#include <unknwn.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <wincodec.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.h>

#include <functional>
#include <utility>
#include <vector>

namespace dx {

namespace wuc = winrt::Windows::UI::Composition;
using float2 = winrt::Windows::Foundation::Numerics::float2;
using float3 = winrt::Windows::Foundation::Numerics::float3;

// A soft drop shadow for a rounded rectangle, stretched with a nine-grid brush.
// Size the visual `extent` px larger than the shape on every side.
struct ShadowBrush {
    wuc::CompositionNineGridBrush brush{nullptr};
    float extent = 0;
};

// Owns the GPU devices and the Windows.UI.Composition tree for one window.
// Everything visible is a composition visual; the system compositor animates
// and renders it, so our own thread only wakes up when something changes.
class Graphics {
public:
    bool Init(HWND hwnd);

    [[nodiscard]] const wuc::Compositor& Comp() const { return compositor_; }
    [[nodiscard]] const wuc::ContainerVisual& Root() const { return root_; }
    [[nodiscard]] IDWriteFactory* DWrite() const { return dwrite_.get(); }
    [[nodiscard]] IWICImagingFactory* Wic() const { return wic_.get(); }

    wuc::CompositionDrawingSurface CreateSurface(int width, int height) const;

    // Draws into a surface with Direct2D. The context uses 96 DPI, so 1 unit = 1 pixel.
    bool Draw(const wuc::CompositionDrawingSurface& surface, const std::function<void(ID2D1DeviceContext*)>& fn) const;

    ShadowBrush CreateShadow(float radius, float sigma) const;

    // A composition tree for another window (menus, popups) sharing this compositor.
    wuc::ContainerVisual AttachWindow(HWND hwnd);
    void DetachWindow(HWND hwnd);

    // The Windows accent color (used for menu highlights, like macOS).
    winrt::Windows::UI::Color Accent() const;

private:
    std::vector<std::pair<HWND, wuc::Desktop::DesktopWindowTarget>> extraTargets_;
    winrt::Windows::System::DispatcherQueueController dispatcher_{nullptr};
    wuc::Compositor compositor_{nullptr};
    wuc::Desktop::DesktopWindowTarget target_{nullptr};
    wuc::ContainerVisual root_{nullptr};
    wuc::CompositionGraphicsDevice graphicsDevice_{nullptr};

    winrt::com_ptr<ID3D11Device> d3d_;
    winrt::com_ptr<ID2D1Factory1> d2dFactory_;
    winrt::com_ptr<ID2D1Device> d2dDevice_;
    winrt::com_ptr<IDWriteFactory> dwrite_;
    winrt::com_ptr<IWICImagingFactory> wic_;
};

inline winrt::Windows::UI::Color Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return winrt::Windows::UI::Color{a, r, g, b};
}

}  // namespace dx
