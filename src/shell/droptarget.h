#pragma once
#include <windows.h>
#include <unknwn.h>
#include <oleidl.h>
#include <shobjidl.h>
#include <winrt/base.h>

#include <functional>
#include <string>
#include <vector>

namespace dx {

// OLE drop target for files dragged from Explorer onto the dock.
// The callbacks get screen coordinates and return a DROPEFFECT_*.
class DropTarget : public winrt::implements<DropTarget, IDropTarget> {
public:
    struct Callbacks {
        std::function<DWORD(const std::vector<std::wstring>& files, POINT screen)> enter;
        std::function<DWORD(POINT screen)> over;
        std::function<void()> leave;
        std::function<DWORD(const std::vector<std::wstring>& files, POINT screen)> drop;
    };

    DropTarget(HWND hwnd, Callbacks callbacks);

    HRESULT __stdcall DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;
    HRESULT __stdcall DragOver(DWORD keys, POINTL pt, DWORD* effect) override;
    HRESULT __stdcall DragLeave() override;
    HRESULT __stdcall Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;

private:
    static std::vector<std::wstring> Files(IDataObject* data);

    HWND hwnd_;
    Callbacks cb_;
    std::vector<std::wstring> files_;
    winrt::com_ptr<IDropTargetHelper> helper_;  // draws Explorer's drag image over our window
};

}  // namespace dx
