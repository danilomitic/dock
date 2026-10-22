#include "core/config.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "core/json.h"
#include "core/log.h"
#include "core/strings.h"

namespace dx {
namespace {

std::optional<uint32_t> ParseColor(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return std::nullopt;
    uint32_t v = 0;
    for (size_t i = 1; i < 7; ++i) {
        const char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
        else return std::nullopt;
    }
    return v;
}

std::string FormatColor(uint32_t v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%06X", v & 0xFFFFFF);
    return buf;
}

double Clamp(double v, double lo, double hi) { return std::clamp(v, lo, hi); }

}  // namespace

std::wstring ConfigDir() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\DockX";
        CoTaskMemFree(base);
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

std::wstring ConfigPath() { return ConfigDir() + L"\\config.json"; }

Config ParseConfig(std::string_view text) { return TryParseConfig(text).value_or(Config{}); }

std::optional<Config> TryParseConfig(std::string_view text) {
    Config c;
    const auto root = json::Parse(text);
    if (!root || root->type != json::Value::Type::Object) return std::nullopt;
    const json::Value& v = *root;

    c.language = v.Str("language", c.language);
    c.theme = v.Str("theme", c.theme) == "light" ? "light" : "dark";
    {
        const std::string pos = v.Str("position", c.position);
        c.position = (pos == "left" || pos == "right") ? pos : "bottom";
    }
    c.reserveSpace = v.Bool("reserveSpace", c.reserveSpace);
    c.autoHide = v.Bool("autoHide", c.autoHide);
    c.showDelay = Clamp(v.Num("showDelay", c.showDelay), 0, 2);
    c.hideDelay = Clamp(v.Num("hideDelay", c.hideDelay), 0, 5);
    c.hideInFullscreen = v.Bool("hideInFullscreen", c.hideInFullscreen);
    c.hideTaskbar = v.Bool("hideTaskbar", c.hideTaskbar);
    c.taskbarHiddenByDock = v.Bool("taskbarHiddenByDock", c.taskbarHiddenByDock);
    c.taskbarOriginalState = static_cast<int>(v.Num("taskbarOriginalState", c.taskbarOriginalState));
    c.iconSize = Clamp(v.Num("iconSize", c.iconSize), 16, 128);
    c.spacing = Clamp(v.Num("spacing", c.spacing), 0, 32);
    c.shelfPadding = Clamp(v.Num("shelfPadding", c.shelfPadding), 0, 32);
    c.cornerRadius = Clamp(v.Num("cornerRadius", c.cornerRadius), 0, 40);
    c.magnification = v.Bool("magnification", c.magnification);
    c.magnifiedSize = Clamp(v.Num("magnifiedSize", c.magnifiedSize), c.iconSize, 256);
    c.magnificationRange = Clamp(v.Num("magnificationRange", c.magnificationRange), 1, 8);
    c.magnifyResponse = Clamp(v.Num("magnifyResponse", c.magnifyResponse), 0.05, 1.0);
    c.blur = v.Bool("blur", c.blur);
    c.glassTint = ParseColor(v.Str("glassTint", ""));
    c.glassOpacity = Clamp(v.Num("glassOpacity", c.glassOpacity), 0, 1);
    c.border = v.Bool("border", c.border);
    c.shadow = v.Bool("shadow", c.shadow);
    c.shadowOpacity = Clamp(v.Num("shadowOpacity", c.shadowOpacity), 0, 1);
    c.showDots = v.Bool("showDots", c.showDots);
    c.showLabels = v.Bool("showLabels", c.showLabels);
    {
        const std::string fx = v.Str("minimizeEffect", c.minimizeEffect);
        c.minimizeEffect = (fx == "scale" || fx == "system") ? fx : "genie";
    }
    c.systemMinAnimChanged = v.Bool("systemMinAnimChanged", c.systemMinAnimChanged);
    c.launchBounce = v.Bool("launchBounce", c.launchBounce);
    c.attentionBounce = v.Bool("attentionBounce", c.attentionBounce);
    c.animationSpeed = Clamp(v.Num("animationSpeed", c.animationSpeed), 0.25, 4);
    c.logging = v.Bool("logging", c.logging);
    c.startWithWindows = v.Bool("startWithWindows", c.startWithWindows);
    c.shortcutsCreated = v.Bool("shortcutsCreated", c.shortcutsCreated);
    c.showRecent = v.Bool("showRecent", c.showRecent);
    c.recentCount = static_cast<int>(Clamp(v.Num("recentCount", c.recentCount), 0, 10));
    c.showTrash = v.Bool("showTrash", c.showTrash);
    c.showHideButton = v.Bool("showHideButton", c.showHideButton);
    c.showStartButton = v.Bool("showStartButton", c.showStartButton);
    {
        const std::string view = v.Str("stackView", c.stackView);
        c.stackView = (view == "fan" || view == "grid" || view == "list") ? view : "auto";
        const std::string sort = v.Str("stackSort", c.stackSort);
        c.stackSort = (sort == "name" || sort == "dateModified" || sort == "kind") ? sort : "dateAdded";
    }
    if (const json::Value* s = v.Find("stacks"); s && s->type == json::Value::Type::Array) {
        c.stacksInitialized = true;
        for (const auto& item : *s->array)
            if (item.type == json::Value::Type::String && !item.string.empty()) c.stacks.push_back(Widen(item.string));
    }
    if (const json::Value* r = v.Find("recent"); r && r->type == json::Value::Type::Array) {
        for (const auto& item : *r->array) {
            PinnedItem pi{Widen(item.Str("path", "")), Widen(item.Str("aumid", ""))};
            if (!pi.path.empty() || !pi.aumid.empty()) c.recent.push_back(std::move(pi));
        }
    }

    if (const json::Value* p = v.Find("pinned"); p && p->type == json::Value::Type::Array) {
        c.pinnedInitialized = true;
        for (const auto& item : *p->array) {
            PinnedItem pi{Widen(item.Str("path", "")), Widen(item.Str("aumid", ""))};
            if (!pi.path.empty() || !pi.aumid.empty()) c.pinned.push_back(std::move(pi));
        }
    }
    return c;
}

std::string SerializeConfig(const Config& c) {
    std::string o = "{\n";
    auto kv = [&o](const char* k, const std::string& val) {
        o += "  ";
        o += json::Quote(k);
        o += ": ";
        o += val;
        o += ",\n";
    };
    auto b = [](bool x) { return std::string(x ? "true" : "false"); };
    kv("language", json::Quote(c.language));
    kv("theme", json::Quote(c.theme));
    kv("position", json::Quote(c.position));
    kv("reserveSpace", b(c.reserveSpace));
    kv("autoHide", b(c.autoHide));
    kv("showDelay", json::Number(c.showDelay));
    kv("hideDelay", json::Number(c.hideDelay));
    kv("hideInFullscreen", b(c.hideInFullscreen));
    kv("hideTaskbar", b(c.hideTaskbar));
    kv("taskbarHiddenByDock", b(c.taskbarHiddenByDock));
    kv("taskbarOriginalState", json::Number(c.taskbarOriginalState));
    kv("iconSize", json::Number(c.iconSize));
    kv("spacing", json::Number(c.spacing));
    kv("shelfPadding", json::Number(c.shelfPadding));
    kv("cornerRadius", json::Number(c.cornerRadius));
    kv("magnification", b(c.magnification));
    kv("magnifiedSize", json::Number(c.magnifiedSize));
    kv("magnificationRange", json::Number(c.magnificationRange));
    kv("magnifyResponse", json::Number(c.magnifyResponse));
    kv("blur", b(c.blur));
    kv("glassTint", json::Quote(c.glassTint ? FormatColor(*c.glassTint) : ""));
    kv("glassOpacity", json::Number(c.glassOpacity));
    kv("border", b(c.border));
    kv("shadow", b(c.shadow));
    kv("shadowOpacity", json::Number(c.shadowOpacity));
    kv("showDots", b(c.showDots));
    kv("showLabels", b(c.showLabels));
    kv("minimizeEffect", json::Quote(c.minimizeEffect));
    kv("systemMinAnimChanged", b(c.systemMinAnimChanged));
    kv("launchBounce", b(c.launchBounce));
    kv("attentionBounce", b(c.attentionBounce));
    kv("animationSpeed", json::Number(c.animationSpeed));
    kv("logging", b(c.logging));
    kv("startWithWindows", b(c.startWithWindows));
    kv("shortcutsCreated", b(c.shortcutsCreated));
    kv("showRecent", b(c.showRecent));
    kv("recentCount", json::Number(c.recentCount));
    kv("showTrash", b(c.showTrash));
    kv("showHideButton", b(c.showHideButton));
    kv("showStartButton", b(c.showStartButton));
    kv("stackView", json::Quote(c.stackView));
    kv("stackSort", json::Quote(c.stackSort));
    auto items = [&o](const char* key, const std::vector<PinnedItem>& list, bool last) {
        o += "  ";
        o += json::Quote(key);
        o += ": [";
        for (size_t i = 0; i < list.size(); ++i) {
            o += i ? ",\n    " : "\n    ";
            o += "{ \"path\": " + json::Quote(Narrow(list[i].path)) + ", \"aumid\": " + json::Quote(Narrow(list[i].aumid)) + " }";
        }
        o += list.empty() ? "]" : "\n  ]";
        o += last ? "\n" : ",\n";
    };
    o += "  \"stacks\": [";
    for (size_t i = 0; i < c.stacks.size(); ++i) {
        o += i ? ", " : "";
        o += json::Quote(Narrow(c.stacks[i]));
    }
    o += "],\n";
    items("recent", c.recent, false);
    items("pinned", c.pinned, true);
    o += "}\n";
    return o;
}

namespace {
std::optional<Config> ReadConfigFile(const std::wstring& path) {
    // Shared read: another program (antivirus, backup, DockX Settings) may have it open.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    std::string text;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) text.append(buf, got);
    CloseHandle(h);
    return TryParseConfig(text);
}
}  // namespace

std::wstring ConfigBackupPath() { return ConfigDir() + L"\\config.backup.json"; }

// Never starts with an empty dock because the file was briefly locked or damaged:
// retries, then falls back to the backup. If both fail, the result is marked
// `loadFailed` and SaveConfig refuses to overwrite the user's file with it.
Config LoadConfig() {
    const std::wstring path = ConfigPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(ConfigBackupPath().c_str()) == INVALID_FILE_ATTRIBUTES)
        return Config{};  // first run
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (auto c = ReadConfigFile(path)) return *c;
        Sleep(100);
    }
    Log(L"config.json could not be read; trying the backup");
    if (auto c = ReadConfigFile(ConfigBackupPath())) return *c;
    Log(L"config backup could not be read either; settings will not be saved this session");
    Config c;
    c.loadFailed = true;
    return c;
}

bool SaveConfig(const Config& c) {
    if (c.loadFailed) return false;  // don't replace the user's (unreadable) file with defaults
    const std::wstring path = ConfigPath();
    const std::wstring tmp = path + L".tmp";
    // Keep the previous good version as a backup.
    if (ReadConfigFile(path)) CopyFileW(path.c_str(), ConfigBackupPath().c_str(), FALSE);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        const std::string text = SerializeConfig(c);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Log(L"SaveConfig: MoveFileEx failed (%lu)", GetLastError());
        return false;
    }
    return true;
}

}  // namespace dx
