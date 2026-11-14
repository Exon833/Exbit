// Where the program keeps your own files (paths.py): skins, plugin pictures, the cache, recovery
// files, your templates. One folder: $XDG_DATA_HOME/TinyDAW, or ~/.local/share/TinyDAW (Windows:
// %APPDATA%\TinyDAW). Settings stay in plat::configDir().
#pragma once
#include <string>

namespace paths {

// the folder (not made here); sub: a folder in it ("skins", "templates", "recovery"…)
std::string dataDir(const std::string& sub = "");
// the same, made if missing ("" if it can't be made)
std::string makeDataDir(const std::string& sub = "");

// tests: use another folder ("" = back to the usual one)
void setDataDir(const std::string& dir);

}  // namespace paths
