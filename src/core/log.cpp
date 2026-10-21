#include "core/log.h"

#include <windows.h>
#include <shlobj.h>

#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace dx {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
bool g_enabled = false;

void OpenFile() {
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) return;
    std::wstring dir = std::wstring(base) + L"\\DockX";
    CoTaskMemFree(base);
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\dockx.log";
    // Simple rotation: start fresh when the file is over 1 MB.
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    const bool big = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 1024 * 1024);
    g_file = _wfsopen(path.c_str(), big ? L"w, ccs=UTF-8" : L"a, ccs=UTF-8", _SH_DENYNO);  // readable while running
}
}  // namespace

void EnableFileLog(bool on) {
    std::scoped_lock lock(g_mutex);
    g_enabled = on;
    if (on && !g_file) OpenFile();
}

void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
    std::scoped_lock lock(g_mutex);
    if (g_enabled && g_file) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fwprintf(g_file, L"%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        fflush(g_file);
    }
}

}  // namespace dx
