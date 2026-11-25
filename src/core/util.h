// Small helpers shared everywhere: files and paths, strings, and the settings store.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/json.h"

namespace util {

// --- debugging ---
// diagnostics on stderr (fonts, slow frames, the audio device, dropouts, MIDI, plugins, status and message
// texts): always on, unless EXBIT_DEBUG=0. The terminal UI sends them to a file (tui/app.cpp)
bool debug();
void setDebug(bool on);  // (tests, headless: off)
// printf to stderr when debug() is on (never from the audio thread: it may block)
#ifdef _WIN32
#define EXBIT_PRINTF gnu_printf  // (the C runtime's own printf takes %zu too; MinGW checks against Microsoft's)
#else
#define EXBIT_PRINTF printf
#endif
void debugf(const char* fmt, ...) __attribute__((format(EXBIT_PRINTF, 1, 2)));

// --- files ---
bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& data);  // written to a temporary file, then renamed
bool fileExists(const std::string& path);
bool isDir(const std::string& path);
bool makeDirs(const std::string& path);
bool removeFile(const std::string& path);
// rename(), replacing `to` if it is there (Windows' own rename doesn't)
bool renameFile(const std::string& from, const std::string& to);
int64_t fileSize(const std::string& path);   // -1 if missing
double fileMtime(const std::string& path);   // seconds since the epoch, 0 if missing
struct DirEntry {
    std::string name;
    bool isDir;
    bool hidden = false;  // Windows: marked hidden or a system file (on Linux, hidden is a name starting with '.')
};
std::vector<DirEntry> listDir(const std::string& path);  // sorted by name, case-insensitively; no "." / ".."
// every file under `root` (recursively, following no symlinked folders twice); fn returns false to stop
void walkFiles(const std::string& root, const std::function<bool(const std::string& path)>& fn);

// --- paths ---
std::string joinPath(const std::string& a, const std::string& b);
std::string dirName(const std::string& path);    // "a/b/c.wav" -> "a/b"
std::string baseName(const std::string& path);   // "a/b/c.wav" -> "c.wav"
std::string stem(const std::string& path);       // "a/b/c.wav" -> "c"
std::string extension(const std::string& path);  // "a/b/c.WAV" -> ".wav" (lower case)
std::string absolutePath(const std::string& path);
std::string expandUser(const std::string& path); // "~/x" -> "/home/me/x"

// --- strings ---
std::string lower(const std::string& s);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);
std::vector<std::string> splitWords(const std::string& s);  // at runs of whitespace
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);
bool containsNoCase(const std::string& hay, const std::string& needle);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);
std::string format(const char* fmt, ...) __attribute__((format(EXBIT_PRINTF, 1, 2)));
// Python-like "{:.2f}" style: fixed decimals
std::string fixed(double v, int decimals);
int compareNatural(const std::string& a, const std::string& b);  // case-insensitive

// --- time ---
double wallClock();  // seconds since the epoch

}  // namespace util

// The settings store (the stand-in for QSettings): a JSON file in plat::configDir()
// ("settings.json"), read once, written on every change (debounced by the caller if needed).
namespace settings {
const Json& get(const std::string& key);                 // null if not set
Json value(const std::string& key, const Json& def);     // the value, or def
void set(const std::string& key, const Json& v);         // and saves
void remove(const std::string& key);
void setPath(const std::string& path);                   // tests: use another file
std::string filePath();                                  // the file (its folder: the settings folder, keymap.txt…)
void flush();
}  // namespace settings
