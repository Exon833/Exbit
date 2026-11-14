// Small helpers shared everywhere: files and paths, strings, and the settings store.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/json.h"

namespace util {

// --- files ---
bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& data);  // written to a temporary file, then renamed
bool fileExists(const std::string& path);
bool isDir(const std::string& path);
bool makeDirs(const std::string& path);
bool removeFile(const std::string& path);
int64_t fileSize(const std::string& path);   // -1 if missing
double fileMtime(const std::string& path);   // seconds since the epoch, 0 if missing
struct DirEntry { std::string name; bool isDir; };
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
char pathSep();

// --- strings ---
std::string lower(const std::string& s);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);
std::vector<std::string> splitWords(const std::string& s);  // at runs of whitespace
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);
bool containsNoCase(const std::string& hay, const std::string& needle);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);
std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
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
void flush();
}  // namespace settings
