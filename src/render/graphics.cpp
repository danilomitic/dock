#include "render/graphics.h"

#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>

#include <algorithm>

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
