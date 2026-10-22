#include "shell/icons.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstdio>
#include <memory>
#include <vector>

#include "core/log.h"
#include "core/strings.h"

namespace dx {
namespace {


std::wstring CacheDir() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\DockX";
        CoTaskMemFree(base);
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\cache";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

std::wstring CachePath(const std::wstring& source) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (wchar_t c : Lower(source)) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    wchar_t name[40];
    swprintf_s(name, L"\\icon_%016llx.png", static_cast<unsigned long long>(h));
    return CacheDir() + name;
}

winrt::com_ptr<IWICBitmap> ToPbgraBitmap(IWICImagingFactory* wic, IWICBitmapSource* src) {
    winrt::com_ptr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(conv.put()))) return nullptr;
    if (FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return nullptr;
    winrt::com_ptr<IWICBitmap> out;
    if (FAILED(wic->CreateBitmapFromSource(conv.get(), WICBitmapCacheOnLoad, out.put()))) return nullptr;
    return out;
}

// Some old icons come back with alpha = 0 everywhere; make those opaque.
void FixAlpha(IWICBitmap* bmp) {
    UINT w = 0, h = 0;
    bmp->GetSize(&w, &h);
    WICRect rc{0, 0, static_cast<INT>(w), static_cast<INT>(h)};
    winrt::com_ptr<IWICBitmapLock> lock;
    if (FAILED(bmp->Lock(&rc, WICBitmapLockRead | WICBitmapLockWrite, lock.put()))) return;
    UINT size = 0;
    BYTE* data = nullptr;
    lock->GetDataPointer(&size, &data);
    if (!data) return;
    for (UINT i = 3; i < size; i += 4)
        if (data[i]) return;
    for (UINT i = 3; i < size; i += 4) data[i] = 255;
}

// "stock:<SHSTOCKICONID>" — system icons like the empty/full Recycle Bin.
winrt::com_ptr<IWICBitmap> ExtractStockIcon(IWICImagingFactory* wic, int id, int px) {
    SHSTOCKICONINFO sii{sizeof(sii)};
    if (FAILED(SHGetStockIconInfo(static_cast<SHSTOCKICONID>(id), SHGSI_ICONLOCATION, &sii))) return nullptr;
    HICON icon = nullptr;
    if (FAILED(SHDefExtractIconW(sii.szPath, sii.iIcon, 0, &icon, nullptr, MAKELONG(px, 16))) || !icon) return nullptr;
    winrt::com_ptr<IWICBitmap> bmp;
    const HRESULT hr = wic->CreateBitmapFromHICON(icon, bmp.put());
    DestroyIcon(icon);
    if (FAILED(hr)) return nullptr;
    return ToPbgraBitmap(wic, bmp.get());
}

winrt::com_ptr<IWICBitmap> ExtractIcon(IWICImagingFactory* wic, const std::wstring& source, int px, bool thumbnail) {
    if (source.starts_with(L"stock:")) return ExtractStockIcon(wic, _wtoi(source.c_str() + 6), px);
    winrt::com_ptr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(factory.put())))) return nullptr;
    HBITMAP hbm = nullptr;
    const SIZE size{px, px};
    const SIIGBF first = thumbnail ? (SIIGBF_RESIZETOFIT | SIIGBF_BIGGERSIZEOK) : (SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK);
    if (FAILED(factory->GetImage(size, first, &hbm)) && FAILED(factory->GetImage(size, SIIGBF_BIGGERSIZEOK, &hbm)))
        return nullptr;
    winrt::com_ptr<IWICBitmap> bmp;
    const HRESULT hr = wic->CreateBitmapFromHBITMAP(hbm, nullptr, WICBitmapUsePremultipliedAlpha, bmp.put());
    DeleteObject(hbm);
    if (FAILED(hr)) return nullptr;
    auto out = ToPbgraBitmap(wic, bmp.get());
    if (out) FixAlpha(out.get());
    return out;
}

void SavePng(IWICImagingFactory* wic, IWICBitmapSource* bmp, const std::wstring& path) {
    const std::wstring tmp = path + L".tmp";
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapEncoder> enc;
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::com_ptr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateStream(stream.put())) || FAILED(stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)))
        return;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) ||
        FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(frame.put(), nullptr)) || FAILED(frame->Initialize(nullptr)))
        return;
    if (FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(bmp, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&fmt)) ||
        FAILED(frame->WriteSource(conv.get(), nullptr)) || FAILED(frame->Commit()) || FAILED(enc->Commit()))
        return;
    stream = nullptr;
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring FileDescription(const std::wstring& exe) {
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    if (!size) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) return {};
    struct Lang {
        WORD lang;
        WORD codepage;
    }* langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &len) || len < sizeof(Lang))
        return {};
    wchar_t key[64];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].codepage);
    wchar_t* value = nullptr;
    if (!VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&value), &len) || !value || !len) return {};
    return value;
}

std::wstring AppName(const std::wstring& source, const std::wstring& exe) {
    const std::wstring lowerExe = Lower(exe);
    if (lowerExe.ends_with(L"\\explorer.exe")) return L"File Explorer";
    if (lowerExe.ends_with(L".exe")) {
        std::wstring d = FileDescription(exe);
        if (!d.empty()) return d;
    }
    winrt::com_ptr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(item.put())))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name))) {
            std::wstring r = name;
            CoTaskMemFree(name);
            return r;
        }
    }
    const size_t slash = source.find_last_of(L"\\/");
    std::wstring file = slash == std::wstring::npos ? source : source.substr(slash + 1);
    if (const size_t dot = file.find_last_of(L'.'); dot != std::wstring::npos) file.resize(dot);
    return file;
}

}  // namespace

winrt::com_ptr<IWICBitmap> LoadCachedIcon(IWICImagingFactory* wic, const std::wstring& source) {
    const std::wstring path = CachePath(source);
    winrt::com_ptr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec.put())))
        return nullptr;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, frame.put()))) return nullptr;
    return ToPbgraBitmap(wic, frame.get());
}

void IconLoader::Start(HWND notify, UINT msg) {
    hwnd_ = notify;
    msg_ = msg;
    quit_ = false;
    thread_ = std::thread([this] { Run(); });
}

void IconLoader::Stop() {
    {
        std::scoped_lock lock(mutex_);
        quit_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void IconLoader::Request(IconRequest r) {
    {
        std::scoped_lock lock(mutex_);
        queue_.push_back(std::move(r));
    }
    cv_.notify_one();
}

void IconLoader::Run() {
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    winrt::com_ptr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put())))) {
        Log(L"IconLoader: no WIC");
        CoUninitialize();
        return;
    }
    for (;;) {
        IconRequest req;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
            if (quit_) break;
            req = std::move(queue_.front());
            queue_.pop_front();
        }
        auto result = std::make_unique<IconResult>();
        result->id = req.id;
        if (!req.source.starts_with(L"stock:")) result->name = AppName(req.source, req.exePath);
        if (req.needIcon) {
            result->bitmap = ExtractIcon(wic.get(), req.source, req.size, req.thumbnail);
            if (result->bitmap && req.cache) SavePng(wic.get(), result->bitmap.get(), CachePath(req.source));
            else if (!result->bitmap) Log(L"No icon for %s", req.source.c_str());
        }
        if (PostMessageW(hwnd_, msg_, 0, reinterpret_cast<LPARAM>(result.get()))) (void)result.release();
    }
    CoUninitialize();
}

}  // namespace dx
