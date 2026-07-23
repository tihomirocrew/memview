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

    // is_discarded() only catches syntax errors; a wrong-typed value makes
    // value()/get<>() throw. Treat that like a corrupt file and keep the
    // defaults rather than crashing at startup.
    try
    {
        s.darkTheme = j.value("darkTheme", s.darkTheme);

        mem::SymbolSettings& sym = s.symbols.settings();
        sym.enabled   = j.value("symbolsEnabled", sym.enabled);
        sym.useServer = j.value("symbolServer",   sym.useServer);
        sym.cacheDir  = j.value("symbolCacheDir", sym.cacheDir);
        if (j.contains("symbolServers") && j["symbolServers"].is_array())
            sym.serverUrls = j["symbolServers"].get<std::vector<std::string>>();
        else if (j.contains("symbolServerUrl")) // single-server config, before the list
            sym.serverUrls = { j["symbolServerUrl"].get<std::string>() };
        if (j.contains("symbolSearchDirs") && j["symbolSearchDirs"].is_array())
            sym.extraDirs = j["symbolSearchDirs"].get<std::vector<std::string>>();
    }
    catch (const nlohmann::json::exception&)
    {
        // A field was the wrong type; the ones read before it still applied.
    }
}

void saveConfig(const AppState& s)
{
    char path[MAX_PATH];
    if (!configPath(path, sizeof(path), true))
        return;

    const mem::SymbolSettings& sym = s.symbols.settings();
    const nlohmann::json j = {
        { "darkTheme",        s.darkTheme },
        { "symbolsEnabled",   sym.enabled },
        { "symbolServer",     sym.useServer },
        { "symbolServers",    sym.serverUrls },
        { "symbolCacheDir",   sym.cacheDir },
        { "symbolSearchDirs", sym.extraDirs },
    };

    std::ofstream f(path);
    if (f)
        f << j.dump(4) << '\n';
}

} // namespace app
