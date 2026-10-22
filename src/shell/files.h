#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace dx {

// ---- Recycle Bin ----
bool IsRecycleBinFull();
void OpenRecycleBin();
void EmptyRecycleBin(HWND owner);                    // Windows asks for confirmation
void RecycleFiles(std::vector<std::wstring> files);  // never a permanent delete

// ---- Folders (Stacks) ----
void CopyOrMoveInto(const std::wstring& folder, std::vector<std::wstring> files);  // move on the same drive, like Explorer
void OpenPath(const std::wstring& path);
std::wstring DownloadsFolder();
std::wstring FolderDisplayName(const std::wstring& folder);

struct FolderEntry {
    std::wstring path;
    std::wstring name;
    std::wstring ext;  // lower-case, for sorting by kind
    bool directory = false;
    ULONGLONG added = 0;     // creation time: when it arrived in the folder
    ULONGLONG modified = 0;
};
// Visible files and folders, sorted by "dateAdded" (newest first), "dateModified", "name" or "kind".
std::vector<FolderEntry> ListFolder(const std::wstring& folder, const std::string& sort);

}  // namespace dx
