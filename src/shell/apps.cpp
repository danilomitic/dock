#include "shell/apps.h"

#include <initguid.h>  // must come before propkey.h so PKEY_* are defined here
#include <appmodel.h>
#include <dwmapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winrt/base.h>

#include <algorithm>
#include <thread>

#include "core/log.h"

namespace dx {
namespace {

std::wstring ProcessPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[1024];
    DWORD n = ARRAYSIZE(buf);
    std::wstring r;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) r.assign(buf, n);
    CloseHandle(h);
    return r;
}

std::wstring ProcessAumid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    UINT32 len = 0;
    std::wstring r;
    if (GetApplicationUserModelId(h, &len, nullptr) == ERROR_INSUFFICIENT_BUFFER && len > 0) {
        r.resize(len);
        if (GetApplicationUserModelId(h, &len, r.data()) == ERROR_SUCCESS) r.resize(len > 0 ? len - 1 : 0);
        else r.clear();
    }
    CloseHandle(h);
    return r;
}

std::wstring StoreString(IPropertyStore* ps, const PROPERTYKEY& key) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring r;
    if (SUCCEEDED(ps->GetValue(key, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) r = pv.pwszVal;
    PropVariantClear(&pv);
    return r;
}

std::wstring WindowAumid(HWND h) {
    winrt::com_ptr<IPropertyStore> ps;
    if (FAILED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(ps.put())))) return {};
    return StoreString(ps.get(), PKEY_AppUserModel_ID);
}

bool IsCloaked(HWND h) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

struct EnumContext {
    DWORD selfPid;
    std::vector<AppWindow>* out;
};

BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumContext*>(lp);
    if (!IsWindowVisible(h)) return TRUE;
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    const bool appWindow = (ex & WS_EX_APPWINDOW) != 0;
    if ((ex & WS_EX_TOOLWINDOW) && !appWindow) return TRUE;
    if ((ex & WS_EX_NOACTIVATE) && !appWindow) return TRUE;
    if (GetWindow(h, GW_OWNER) && !appWindow) return TRUE;
    if (IsCloaked(h)) return TRUE;

    wchar_t cls[128]{};
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    static constexpr const wchar_t* kSkip[] = {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
                                               L"Windows.UI.Core.CoreWindow"};
    for (const wchar_t* s : kSkip)
        if (wcscmp(cls, s) == 0) return TRUE;

    const bool frame = wcscmp(cls, L"ApplicationFrameWindow") == 0;
    if (!frame && GetWindowTextLengthW(h) == 0) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == ctx->selfPid) return TRUE;

    // UWP apps live inside ApplicationFrameHost; the real app owns the CoreWindow child.
    DWORD appPid = pid;
    if (frame) {
        if (HWND core = FindWindowExW(h, nullptr, L"Windows.UI.Core.CoreWindow", nullptr)) {
            DWORD cp = 0;
            GetWindowThreadProcessId(core, &cp);
            if (cp) appPid = cp;
        }
    }

    AppWindow w;
    w.hwnd = h;
    w.exe = ProcessPath(appPid);
    const std::wstring processAumid = ProcessAumid(appPid);
    w.aumid = WindowAumid(h);
    if (w.aumid.empty()) w.aumid = processAumid;
    w.packaged = frame || !processAumid.empty();
    if (frame && w.aumid.empty()) return TRUE;  // cannot identify it
    ctx->out->push_back(std::move(w));
    return TRUE;
}

}  // namespace

std::vector<AppWindow> EnumAppWindows(DWORD selfPid) {
    std::vector<AppWindow> out;
    EnumContext ctx{selfPid, &out};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

ShortcutInfo ReadShortcut(const std::wstring& lnk) {
    ShortcutInfo r;
    winrt::com_ptr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())))) return r;
    if (FAILED(link.as<IPersistFile>()->Load(lnk.c_str(), STGM_READ))) return r;
    wchar_t buf[MAX_PATH]{};
    if (SUCCEEDED(link->GetPath(buf, MAX_PATH, nullptr, SLGP_RAWPATH)) && buf[0]) {
        wchar_t expanded[MAX_PATH]{};
        ExpandEnvironmentStringsW(buf, expanded, MAX_PATH);
        r.target = expanded;
    }
    if (const auto ps = link.try_as<IPropertyStore>()) r.aumid = StoreString(ps.get(), PKEY_AppUserModel_ID);
    return r;
}

void LaunchApp(const std::wstring& path, const std::wstring& aumid) {
    std::wstring target = !path.empty() ? path : L"shell:AppsFolder\\" + aumid;
    std::thread([target = std::move(target)] {
        (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpFile = target.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) Log(L"Launch failed (%lu): %s", GetLastError(), target.c_str());
        CoUninitialize();
    }).detach();
}

void OpenFilesWith(const std::wstring& exe, const std::wstring& aumid, std::vector<std::wstring> files) {
    std::thread([exe, aumid, files = std::move(files)] {
        (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool done = false;
        if (exe.empty() && !aumid.empty()) {
            // Store apps: ask Windows to activate the app for these files.
            winrt::com_ptr<IApplicationActivationManager> mgr;
            std::vector<PIDLIST_ABSOLUTE> pidls;
            for (const auto& f : files) {
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (SUCCEEDED(SHParseDisplayName(f.c_str(), nullptr, &pidl, 0, nullptr))) pidls.push_back(pidl);
            }
            winrt::com_ptr<IShellItemArray> items;
            if (!pidls.empty() &&
                SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()),
                                                            const_cast<PCIDLIST_ABSOLUTE_ARRAY>(pidls.data()), items.put())) &&
                SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
                                           IID_PPV_ARGS(mgr.put())))) {
                DWORD pid = 0;
                done = SUCCEEDED(mgr->ActivateForFile(aumid.c_str(), items.get(), L"open", &pid));
            }
            for (auto p : pidls) CoTaskMemFree(p);
        }
        if (!done && !exe.empty()) {
            std::wstring args;
            for (const auto& f : files) args += L"\"" + f + L"\" ";
            SHELLEXECUTEINFOW sei{sizeof(sei)};
            sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            sei.lpFile = exe.c_str();
            sei.lpParameters = args.c_str();
            sei.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&sei)) Log(L"OpenFilesWith failed (%lu): %s", GetLastError(), exe.c_str());
        }
        CoUninitialize();
    }).detach();
}

void MinimizeWindows(const std::vector<HWND>& windows) {
    for (HWND w : windows) ShowWindowAsync(w, SW_MINIMIZE);
}

void CloseWindows(const std::vector<HWND>& windows) {
    for (HWND w : windows) PostMessageW(w, WM_CLOSE, 0, 0);
}

void ForceQuitWindows(const std::vector<HWND>& windows) {
    std::vector<DWORD> pids;
    for (HWND w : windows) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        // UWP windows belong to ApplicationFrameHost; the app owns the CoreWindow child.
        if (HWND core = FindWindowExW(w, nullptr, L"Windows.UI.Core.CoreWindow", nullptr)) GetWindowThreadProcessId(core, &pid);
        if (pid && std::find(pids.begin(), pids.end(), pid) == pids.end()) pids.push_back(pid);
    }
    for (DWORD pid : pids) {
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
            TerminateProcess(h, 1);
            CloseHandle(h);
        }
    }
}

void ShowInExplorer(const std::wstring& path) {
    std::wstring args = L"/select,\"" + path + L"\"";
    std::thread([args = std::move(args)] {
        (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        CoUninitialize();
    }).detach();
}

void ActivateWindow(HWND h) {
    if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
    if (!SetForegroundWindow(h)) SwitchToThisWindow(h, TRUE);
}

std::wstring WindowTitle(HWND h) {
    wchar_t buf[256]{};
    GetWindowTextW(h, buf, ARRAYSIZE(buf));
    return buf;
}

bool IsAppsFolderItem(const std::wstring& aumid) {
    if (aumid.empty()) return false;
    winrt::com_ptr<IShellItem> item;
    return SUCCEEDED(SHCreateItemFromParsingName((L"shell:AppsFolder\\" + aumid).c_str(), nullptr, IID_PPV_ARGS(item.put())));
}

std::vector<PinnedItem> DefaultPinnedApps() {
    std::vector<PinnedItem> v;
    auto addPath = [&v](const wchar_t* p) {
        wchar_t expanded[MAX_PATH]{};
        ExpandEnvironmentStringsW(p, expanded, MAX_PATH);
        if (GetFileAttributesW(expanded) != INVALID_FILE_ATTRIBUTES) v.push_back({expanded, L""});
    };
    auto addApp = [&v](const wchar_t* aumid) {
        if (IsAppsFolderItem(aumid)) v.push_back({L"", aumid});
    };
    addPath(L"%WINDIR%\\explorer.exe");
    addPath(L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe");
    addPath(L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe");
    addApp(L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App");
    addApp(L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App");
    addApp(L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App");
    addApp(L"windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel");
    return v;
}

}  // namespace dx
