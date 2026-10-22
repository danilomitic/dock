#pragma once
#include <string>
#include <string_view>

namespace dx {

std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view wide);
std::wstring Lower(std::wstring s);

// UI text. Language comes from the config ("sr" or "en").
enum class Text {
    KeepInDock,
    ShowInExplorer,
    Quit,
    Magnification,
    QuitDock,
    Settings,
    NoWindows,
    DarkTheme,
    AutoHide,
    Position,
    PositionLeft,
    PositionBottom,
    PositionRight,
    HideTaskbar,
    Options,
    ShowAllWindows,
    Hide,
    ForceQuit,
    Open,
    Remove,
    Minimized,
    RecycleBin,
    EmptyRecycleBin,
    SortBy,
    SortName,
    SortDateAdded,
    SortDateModified,
    SortKind,
    ViewContentAs,
    ViewFan,
    ViewGrid,
    ViewList,
    ViewAuto,
    OpenInExplorer,
    MoreInExplorer,  // "%d more in Explorer"
    RemoveFromDock,
    ShowRecent,
    EmptyFolder,
    StartWithWindows,
};
void SetLanguage(std::string_view lang);
const wchar_t* Tr(Text t);

}  // namespace dx
