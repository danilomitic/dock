#include "shell/droptarget.h"

#include <shellapi.h>
#include <shlobj.h>

namespace dx {

DropTarget::DropTarget(HWND hwnd, Callbacks callbacks) : hwnd_(hwnd), cb_(std::move(callbacks)) {
    CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(helper_.put()));
}

std::vector<std::wstring> DropTarget::Files(IDataObject* data) {
    std::vector<std::wstring> out;
    FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&fmt, &medium))) return out;
    if (auto* drop = static_cast<HDROP>(GlobalLock(medium.hGlobal))) {
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring f(len, L'\0');
            DragQueryFileW(drop, i, f.data(), len + 1);
            out.push_back(std::move(f));
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return out;
}

HRESULT DropTarget::DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) {
    files_ = Files(data);
    POINT p{pt.x, pt.y};
    *effect = (!files_.empty() && cb_.enter) ? cb_.enter(files_, p) : DROPEFFECT_NONE;
    if (helper_) helper_->DragEnter(hwnd_, data, &p, *effect);
    return S_OK;
}

HRESULT DropTarget::DragOver(DWORD, POINTL pt, DWORD* effect) {
    POINT p{pt.x, pt.y};
    *effect = (!files_.empty() && cb_.over) ? cb_.over(p) : DROPEFFECT_NONE;
    if (helper_) helper_->DragOver(&p, *effect);
    return S_OK;
}

HRESULT DropTarget::DragLeave() {
    if (helper_) helper_->DragLeave();
    if (cb_.leave) cb_.leave();
    files_.clear();
    return S_OK;
}

HRESULT DropTarget::Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) {
    POINT p{pt.x, pt.y};
    const auto files = Files(data);
    *effect = (!files.empty() && cb_.drop) ? cb_.drop(files, p) : DROPEFFECT_NONE;
    if (helper_) helper_->Drop(data, &p, *effect);
    files_.clear();
    return S_OK;
}

}  // namespace dx
