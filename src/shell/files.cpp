#include "shell/files.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <winrt/base.h>

#include <algorithm>
#include <functional>
#include <thread>

#include "core/log.h"
#include "core/strings.h"

namespace dx {
namespace {

ULONGLONG ToU64(const FILETIME& ft) { return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

// Runs a shell file operation on a background STA thread (it may show progress UI).
void RunFileOp(std::vector<std::wstring> files, const std::function<void(IFileOperation*, IShellItem*)>& perItem,
               DWORD flags) {
    std::thread([files = std::move(files), perItem, flags] {
        (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        {
            winrt::com_ptr<IFileOperation> op;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.put())))) {
                op->SetOperationFlags(flags);
                for (const auto& f : files) {
                    winrt::com_ptr<IShellItem> item;
                    if (SUCCEEDED(SHCreateItemFromParsingName(f.c_str(), nullptr, IID_PPV_ARGS(item.put())))) perItem(op.get(), item.get());
                }
                if (FAILED(op->PerformOperations())) Log(L"File operation failed");
            }
        }
        CoUninitialize();
    }).detach();
}

}  // namespace

bool IsRecycleBinFull() {
    SHQUERYRBINFO info{sizeof(info)};
    return SUCCEEDED(SHQueryRecycleBinW(nullptr, &info)) && info.i64NumItems > 0;
}

void OpenRecycleBin() { OpenPath(L"shell:RecycleBinFolder"); }

void EmptyRecycleBin(HWND owner) {
    std::thread([owner] { SHEmptyRecycleBinW(owner, nullptr, 0); }).detach();
}

void RecycleFiles(std::vector<std::wstring> files) {
    RunFileOp(std::move(files), [](IFileOperation* op, IShellItem* item) { op->DeleteItem(item, nullptr); },
              FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD);
}

void CopyOrMoveInto(const std::wstring& folder, std::vector<std::wstring> files) {
    const std::wstring dest = folder;
    RunFileOp(std::move(files),
              [dest](IFileOperation* op, IShellItem* item) {
                  winrt::com_ptr<IShellItem> target;
                  if (FAILED(SHCreateItemFromParsingName(dest.c_str(), nullptr, IID_PPV_ARGS(target.put())))) return;
                  PWSTR src = nullptr;
                  bool sameDrive = false;
                  if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &src))) {
                      sameDrive = PathGetDriveNumberW(src) == PathGetDriveNumberW(dest.c_str()) && PathGetDriveNumberW(src) >= 0;
                      CoTaskMemFree(src);
                  }
                  if (sameDrive) op->MoveItem(item, target.get(), nullptr, nullptr);
                  else op->CopyItem(item, target.get(), nullptr, nullptr);
              },
              FOF_ALLOWUNDO | FOFX_ADDUNDORECORD);
}

void OpenPath(const std::wstring& path) {
    std::thread([path] {
        (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpFile = path.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) Log(L"Open failed (%lu): %s", GetLastError(), path.c_str());
        CoUninitialize();
    }).detach();
}

std::wstring DownloadsFolder() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

std::wstring FolderDisplayName(const std::wstring& folder) {
    winrt::com_ptr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(item.put())))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name))) {
            std::wstring r = name;
            CoTaskMemFree(name);
            return r;
        }
    }
    const size_t slash = folder.find_last_of(L"\\/");
    return slash == std::wstring::npos ? folder : folder.substr(slash + 1);
}

std::vector<FolderEntry> ListFolder(const std::wstring& folder, const std::string& sort) {
    std::vector<FolderEntry> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || Lower(name) == L"desktop.ini") continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        FolderEntry e;
        e.path = folder + L"\\" + name;
        e.name = name;
        e.directory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (const size_t dot = name.find_last_of(L'.'); dot != std::wstring::npos && !e.directory) e.ext = Lower(name.substr(dot + 1));
        e.added = ToU64(fd.ftCreationTime);
        e.modified = ToU64(fd.ftLastWriteTime);
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (sort == "name") {
        std::sort(out.begin(), out.end(), [](const FolderEntry& a, const FolderEntry& b) {
            return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });
    } else if (sort == "kind") {
        std::sort(out.begin(), out.end(), [](const FolderEntry& a, const FolderEntry& b) {
            if (a.directory != b.directory) return a.directory;
            if (a.ext != b.ext) return a.ext < b.ext;
            return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });
    } else if (sort == "dateModified") {
        std::sort(out.begin(), out.end(), [](const FolderEntry& a, const FolderEntry& b) { return a.modified > b.modified; });
    } else {
        std::sort(out.begin(), out.end(), [](const FolderEntry& a, const FolderEntry& b) { return a.added > b.added; });
    }
    return out;
}

}  // namespace dx
