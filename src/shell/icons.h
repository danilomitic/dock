#pragma once
#include <windows.h>
#include <wincodec.h>
#include <winrt/base.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace dx {

struct IconRequest {
    int id = 0;
    std::wstring source;   // path, or shell:AppsFolder\<aumid>
    std::wstring exePath;  // for the app name (file description)
    bool needIcon = true;  // false when the icon came from the disk cache
    bool cache = true;     // false for files in a Stack (their thumbnails change)
    int size = 256;        // pixels
    bool thumbnail = false;  // allow picture/document previews instead of the file-type icon
};

struct IconResult {
    int id = 0;
    winrt::com_ptr<IWICBitmap> bitmap;  // 32bpp PBGRA, may be null
    std::wstring name;
};

// Loads a cached icon (PNG in %LOCALAPPDATA%\DockX\cache) on the calling thread.
winrt::com_ptr<IWICBitmap> LoadCachedIcon(IWICImagingFactory* wic, const std::wstring& source);

// Extracts icons and app names on a background thread and posts
// `msg` (lParam = IconResult*, receiver deletes it) to the window.
class IconLoader {
public:
    IconLoader() = default;
    IconLoader(const IconLoader&) = delete;
    IconLoader& operator=(const IconLoader&) = delete;
    ~IconLoader() { Stop(); }

    void Start(HWND notify, UINT msg);
    void Stop();
    void Request(IconRequest r);

private:
    void Run();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<IconRequest> queue_;
    bool quit_ = false;
    HWND hwnd_ = nullptr;
    UINT msg_ = 0;
};

}  // namespace dx
