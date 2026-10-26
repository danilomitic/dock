#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dx {

struct PinnedItem {
    std::wstring path;   // .exe or .lnk (empty for Store apps)
    std::wstring aumid;  // AppUserModelID (Store apps, or extra match key)
};

// All sizes are in DIPs (1/96 inch). Defaults match the macOS Dock.
struct Config {
    std::string language = "en";      // "en" | "sr"
    std::string theme = "dark";       // "dark" | "light"

    // Placement and hiding
    std::string position = "bottom";  // "bottom" | "left" | "right"
    bool reserveSpace = true;         // maximized windows stop at the dock (when not auto-hiding)
    bool autoHide = false;
    double showDelay = 0.0;           // seconds before an auto-hidden dock appears
    double hideDelay = 0.4;           // seconds before it hides again
    bool hideInFullscreen = true;
    bool hideTaskbar = false;         // auto-hide the Windows taskbar while DockX runs
    bool taskbarHiddenByDock = false; // internal: restore the taskbar on exit/crash
    int taskbarOriginalState = 0;     // internal: ABM_GETSTATE before we changed it

    double iconSize = 48;
    double spacing = 6;
    double shelfPadding = 6;
    double cornerRadius = 16;

    bool magnification = true;
    double magnifiedSize = 84;
    double magnificationRange = 3;    // icons affected on each side
    double magnifyResponse = 0.25;    // spring response for enter/exit, seconds

    bool blur = true;
    std::optional<uint32_t> glassTint;  // 0xRRGGBB; empty = theme default
    double glassOpacity = 0.40;
    bool border = true;
    bool shadow = true;
    double shadowOpacity = 0.35;

    bool showDots = true;
    bool showLabels = true;
    std::string minimizeEffect = "genie";  // "genie" | "scale" | "system"
    bool systemMinAnimChanged = false;     // internal: we turned the Windows animation off; restore it
    bool launchBounce = true;
    bool attentionBounce = true;
    double animationSpeed = 1.0;      // global multiplier

    bool logging = false;
    bool startWithWindows = true;
    bool shortcutsCreated = false;    // internal: Start menu + desktop shortcuts made once

    // Right side of the dock
    bool showRecent = true;           // running + recently used apps after a separator (macOS default)
    int recentCount = 3;              // closed apps kept in the recent section
    bool showTrash = true;
    bool showHideButton = true;       // a button at the end of the dock that hides / keeps the dock
    bool showStartButton = true;      // a Windows (Start menu) button at the start of the dock
    std::string stackView = "auto";   // "auto" | "fan" | "grid" | "list"
    std::string stackSort = "dateAdded";  // "dateAdded" | "name" | "dateModified" | "kind"

    bool loadFailed = false;  // not saved: the file existed but could not be read
    std::wstring loadInfo;    // not saved: how loading went (written to startup.log)

    bool pinnedInitialized = false;
    std::vector<PinnedItem> pinned;
    bool stacksInitialized = false;
    std::vector<std::wstring> stacks;  // folders shown as Stacks
    std::vector<PinnedItem> recent;    // most recent first
};

std::wstring ConfigDir();         // %APPDATA%\DockX (created if missing)
std::wstring ConfigPath();        // %APPDATA%\DockX\config.json
std::wstring ConfigBackupPath();  // %APPDATA%\DockX\config.backup.json (previous good version)

Config ParseConfig(std::string_view json);                  // defaults if invalid
std::optional<Config> TryParseConfig(std::string_view json);  // nullopt if invalid
std::string SerializeConfig(const Config& c);

Config LoadConfig();             // defaults if missing or invalid
bool SaveConfig(const Config& c);  // atomic write

}  // namespace dx
