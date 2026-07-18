#include "app/config.hpp"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>

namespace app {

namespace {

// Full path of config.json, or false if %APPDATA% is unset. `create` also makes
// the MemView directory so a first-ever save doesn't fail.
bool configPath(char* out, size_t n, bool create)
{
    char appdata[MAX_PATH];
    const DWORD len = GetEnvironmentVariableA("APPDATA", appdata,
        (DWORD)sizeof(appdata));
    if (len == 0 || len >= sizeof(appdata))
        return false;

    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\MemView", appdata);
    if (create)
        CreateDirectoryA(dir, nullptr); // ok if it already exists

    snprintf(out, n, "%s\\config.json", dir);
    return true;
}

} // namespace

void loadConfig(AppState& s)
{
    char path[MAX_PATH];
    if (!configPath(path, sizeof(path), false))
        return;

    std::ifstream f(path);
    if (!f)
        return; // first run, keep defaults

    // Tolerant parse: a corrupt file falls back to defaults; missing keys keep
    // their current values.
    const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded())
        return;

    s.darkTheme = j.value("darkTheme", s.darkTheme);
}

void saveConfig(const AppState& s)
{
    char path[MAX_PATH];
    if (!configPath(path, sizeof(path), true))
        return;

    const nlohmann::json j = {
        { "darkTheme", s.darkTheme },
    };

    std::ofstream f(path);
    if (f)
        f << j.dump(4) << '\n';
}

} // namespace app
