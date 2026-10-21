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
    std::string language = "sr";      // "sr" | "en"
    std::string theme = "dark";       // "dark" | "light"

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
    bool launchBounce = true;
    bool attentionBounce = true;
    double animationSpeed = 1.0;      // global multiplier

    bool logging = false;

    bool pinnedInitialized = false;
    std::vector<PinnedItem> pinned;
};

std::wstring ConfigDir();   // %APPDATA%\DockX (created if missing)
std::wstring ConfigPath();  // %APPDATA%\DockX\config.json

Config ParseConfig(std::string_view json);
std::string SerializeConfig(const Config& c);

Config LoadConfig();             // defaults if missing or invalid
bool SaveConfig(const Config& c);  // atomic write

}  // namespace dx
