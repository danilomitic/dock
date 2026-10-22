#include "core/strings.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

namespace dx {

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string Narrow(std::wstring_view s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

namespace {
bool g_serbian = false;
}

void SetLanguage(std::string_view lang) { g_serbian = (lang != "en"); }

const wchar_t* Tr(Text t) {
    switch (t) {
        case Text::KeepInDock: return g_serbian ? L"Zadrži u Docku" : L"Keep in Dock";
        case Text::ShowInExplorer: return g_serbian ? L"Prikaži u Exploreru" : L"Show in Explorer";
        case Text::Quit: return g_serbian ? L"Zatvori" : L"Quit";
        case Text::Magnification: return g_serbian ? L"Uvećavanje" : L"Magnification";
        case Text::QuitDock: return g_serbian ? L"Izađi iz DockX" : L"Quit DockX";
        case Text::Settings: return g_serbian ? L"Podešavanja Docka…" : L"Dock Settings…";
        case Text::NoWindows: return g_serbian ? L"Nema prozora" : L"No windows";
        case Text::DarkTheme: return g_serbian ? L"Tamna tema" : L"Dark theme";
        case Text::AutoHide: return g_serbian ? L"Automatski sakrij Dock" : L"Automatically hide the Dock";
        case Text::Position: return g_serbian ? L"Položaj na ekranu" : L"Position on screen";
        case Text::PositionLeft: return g_serbian ? L"Levo" : L"Left";
        case Text::PositionBottom: return g_serbian ? L"Dole" : L"Bottom";
        case Text::PositionRight: return g_serbian ? L"Desno" : L"Right";
        case Text::HideTaskbar: return g_serbian ? L"Sakrij Windows taskbar" : L"Hide the Windows taskbar";
        case Text::Options: return g_serbian ? L"Opcije" : L"Options";
        case Text::ShowAllWindows: return g_serbian ? L"Prikaži sve prozore" : L"Show All Windows";
        case Text::Hide: return g_serbian ? L"Sakrij" : L"Hide";
        case Text::ForceQuit: return g_serbian ? L"Prisilno zatvori" : L"Force Quit";
        case Text::Open: return g_serbian ? L"Otvori" : L"Open";
        case Text::Remove: return g_serbian ? L"Ukloni" : L"Remove";
        case Text::Minimized: return g_serbian ? L"Minimizovan" : L"Minimized";
        case Text::RecycleBin: return g_serbian ? L"Korpa" : L"Recycle Bin";
        case Text::EmptyRecycleBin: return g_serbian ? L"Isprazni korpu" : L"Empty Recycle Bin";
        case Text::SortBy: return g_serbian ? L"Sortiraj po" : L"Sort by";
        case Text::SortName: return g_serbian ? L"Imenu" : L"Name";
        case Text::SortDateAdded: return g_serbian ? L"Datumu dodavanja" : L"Date Added";
        case Text::SortDateModified: return g_serbian ? L"Datumu izmene" : L"Date Modified";
        case Text::SortKind: return g_serbian ? L"Vrsti" : L"Kind";
        case Text::ViewContentAs: return g_serbian ? L"Prikaži sadržaj kao" : L"View content as";
        case Text::ViewFan: return g_serbian ? L"Lepeza" : L"Fan";
        case Text::ViewGrid: return g_serbian ? L"Mreža" : L"Grid";
        case Text::ViewList: return g_serbian ? L"Lista" : L"List";
        case Text::ViewAuto: return g_serbian ? L"Automatski" : L"Automatic";
        case Text::OpenInExplorer: return g_serbian ? L"Otvori u Exploreru" : L"Open in Explorer";
        case Text::MoreInExplorer: return g_serbian ? L"Još %d u Exploreru" : L"%d more in Explorer";
        case Text::RemoveFromDock: return g_serbian ? L"Ukloni iz Docka" : L"Remove from Dock";
        case Text::ShowRecent: return g_serbian ? L"Prikaži nedavne aplikacije" : L"Show recent apps";
        case Text::EmptyFolder: return g_serbian ? L"Prazno" : L"Empty";
        case Text::StartWithWindows: return g_serbian ? L"Pokreni sa Windowsom" : L"Start with Windows";
    }
    return L"";
}

}  // namespace dx
