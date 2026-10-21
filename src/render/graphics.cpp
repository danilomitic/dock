#include "render/graphics.h"

#include <DispatcherQueue.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <cmath>

#include "core/log.h"

namespace abi = ABI::Windows::UI::Composition;

namespace dx {

bool Graphics::Init(HWND hwnd) {
    try {
        if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
            DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
            winrt::check_hresult(CreateDispatcherQueueController(
                options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(dispatcher_))));
        }

        compositor_ = wuc::Compositor();
        const auto desktopInterop = compositor_.as<abi::Desktop::ICompositorDesktopInterop>();
        winrt::check_hresult(desktopInterop->CreateDesktopWindowTarget(
            hwnd, TRUE, reinterpret_cast<abi::Desktop::IDesktopWindowTarget**>(winrt::put_abi(target_))));

        root_ = compositor_.CreateContainerVisual();
        root_.RelativeSizeAdjustment({1.0f, 1.0f});
        target_.Root(root_);

        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                       d3d_.put(), nullptr, nullptr);
        if (FAILED(hr)) {
            Log(L"Hardware D3D11 device failed (0x%08X), using WARP", hr);
            winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                                                   D3D11_SDK_VERSION, d3d_.put(), nullptr, nullptr));
        }

        D2D1_FACTORY_OPTIONS fo{};
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo,
                                               d2dFactory_.put_void()));
        const auto dxgi = d3d_.as<IDXGIDevice>();
        winrt::check_hresult(d2dFactory_->CreateDevice(dxgi.get(), d2dDevice_.put()));

        const auto interop = compositor_.as<abi::ICompositorInterop>();
        winrt::check_hresult(interop->CreateGraphicsDevice(
            d2dDevice_.get(), reinterpret_cast<abi::ICompositionGraphicsDevice**>(winrt::put_abi(graphicsDevice_))));

        winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                 reinterpret_cast<IUnknown**>(dwrite_.put())));
        winrt::check_hresult(
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.put())));
        return true;
    } catch (const winrt::hresult_error& e) {
        Log(L"Graphics init failed: 0x%08X %s", static_cast<unsigned>(e.code().value), e.message().c_str());
        return false;
    }
}

wuc::CompositionDrawingSurface Graphics::CreateSurface(int width, int height) const {
    using namespace winrt::Windows::Graphics::DirectX;
    return graphicsDevice_.CreateDrawingSurface(
        {static_cast<float>(std::max(1, width)), static_cast<float>(std::max(1, height))},
        DirectXPixelFormat::B8G8R8A8UIntNormalized, DirectXAlphaMode::Premultiplied);
}

ShadowBrush Graphics::CreateShadow(float radius, float sigma) const {
    ShadowBrush out;
    const float ext = 3 * sigma;
    const float inset = ext + radius;
    const int n = static_cast<int>(std::ceil(2 * inset + 4));
    const auto surface = CreateSurface(n, n);
    Draw(surface, [&](ID2D1DeviceContext* dc) {
        winrt::com_ptr<ID2D1Bitmap1> shape;
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(dc->CreateBitmap(D2D1::SizeU(n, n), nullptr, 0, props, shape.put()))) return;
        winrt::com_ptr<ID2D1Image> target;
        dc->GetTarget(target.put());
        D2D1_MATRIX_3X2_F transform;
        dc->GetTransform(&transform);
        dc->SetTarget(shape.get());
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        winrt::com_ptr<ID2D1SolidColorBrush> black;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), black.put());
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ext, ext, n - ext, n - ext), radius, radius), black.get());
        dc->SetTarget(target.get());
        dc->SetTransform(transform);
        winrt::com_ptr<ID2D1Effect> fx;
        if (FAILED(dc->CreateEffect(CLSID_D2D1Shadow, fx.put()))) return;
        fx->SetInput(0, shape.get());
        fx->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, sigma);
        dc->DrawImage(fx.get());
    });
    out.brush = compositor_.CreateNineGridBrush();
    out.brush.Source(compositor_.CreateSurfaceBrush(surface));
    out.brush.SetInsets(inset);
    out.extent = ext;
    return out;
}

wuc::ContainerVisual Graphics::AttachWindow(HWND hwnd) {
    const auto interop = compositor_.as<abi::Desktop::ICompositorDesktopInterop>();
    wuc::Desktop::DesktopWindowTarget target{nullptr};
    winrt::check_hresult(interop->CreateDesktopWindowTarget(
        hwnd, TRUE, reinterpret_cast<abi::Desktop::IDesktopWindowTarget**>(winrt::put_abi(target))));
    auto root = compositor_.CreateContainerVisual();
    root.RelativeSizeAdjustment({1.0f, 1.0f});
    target.Root(root);
    extraTargets_.emplace_back(hwnd, target);
    return root;
}

void Graphics::DetachWindow(HWND hwnd) {
    std::erase_if(extraTargets_, [hwnd](const auto& p) { return p.first == hwnd; });
}

winrt::Windows::UI::Color Graphics::Accent() const {
    try {
        return winrt::Windows::UI::ViewManagement::UISettings().GetColorValue(
            winrt::Windows::UI::ViewManagement::UIColorType::Accent);
    } catch (...) {
        return Rgba(10, 132, 255);  // macOS blue
    }
}

bool Graphics::Draw(const wuc::CompositionDrawingSurface& surface,
                    const std::function<void(ID2D1DeviceContext*)>& fn) const {
    const auto interop = surface.as<abi::ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    const HRESULT hr = interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset);
    if (FAILED(hr)) {
        Log(L"BeginDraw failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    dc->SetDpi(96.0f, 96.0f);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    fn(dc.get());
    interop->EndDraw();
    return true;
}

}  // namespace dx
