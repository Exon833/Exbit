#include "core/util.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef EXBIT_WINE_PE
#include <windows.h>  // (src/winehost/include's: wine_get_dos_file_name)
#endif
#ifdef EXBIT_WINDOWS
#include <windows.h>

#include "core/os.h"
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

#include "platform/platform.h"

namespace util {

namespace {
bool& debugFlag() {
    static bool on = [] {
        const char* v = getenv("EXBIT_DEBUG");
        return !v || std::strcmp(v, "0") != 0;
    }();
    return on;
}
}  // namespace
bool debug() { return debugFlag(); }
void setDebug(bool on) { debugFlag() = on; }
void debugf(const char* fmt, ...) {
    if (!debug()) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

#ifdef EXBIT_WINE_PE
// The 32-bit Wine host built as a Windows program (MinGW; winehost/unixside.h) is given Unix paths,
// as the Linux builds are: its files are opened by their DOS names (Wine's), in UTF-16.
// shortcut: only what the host uses (removeFile, fileSize, fileMtime, listDir and walkFiles
// aren't in this build: a link error says so if the host ever needs one)
static std::wstring dosName(const std::string& path) {
    std::wstring w;
    if (LPWSTR dos = wine_get_dos_file_name(path.c_str())) {
        w = (const wchar_t*)dos;
        HeapFree(GetProcessHeap(), 0, dos);
    }
    return w;
}

bool readFile(const std::string& path, std::string& out) {
    FILE* f = _wfopen(dosName(path).c_str(), L"rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool writeFile(const std::string& path, const std::string& data) {
    std::wstring tmp = dosName(path + ".tmp"), to = dosName(path);
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fclose(f) == 0) && ok;
    if (ok) _wremove(to.c_str());  // (the CRT's rename doesn't replace a file)
    if (!ok || _wrename(tmp.c_str(), to.c_str()) != 0) {
        _wremove(tmp.c_str());
        return false;
    }
    return true;
}

bool fileExists(const std::string& path) {
    struct _stat64 st;
    return _wstat64(dosName(path).c_str(), &st) == 0;
}

bool isDir(const std::string& path) {
    struct _stat64 st;
    return _wstat64(dosName(path).c_str(), &st) == 0 && (st.st_mode & _S_IFDIR);
}

bool makeDirs(const std::string& path) {
    if (path.empty() || isDir(path)) return true;
    std::string parent = dirName(path);
    if (!parent.empty() && parent != path && !makeDirs(parent)) return false;
    return _wmkdir(dosName(path).c_str()) == 0 || isDir(path);
}

#else
bool readFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool writeFile(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        ::remove(tmp.c_str());
        return false;
    }
    if (!renameFile(tmp, path)) {
        ::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool isDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool makeDirs(const std::string& path) {
    if (path.empty() || isDir(path)) return true;
    std::string parent = dirName(path);
    if (!parent.empty() && parent != path && !makeDirs(parent)) return false;
    return mkdir(path.c_str(), 0755) == 0 || isDir(path);
}

bool removeFile(const std::string& path) { return ::remove(path.c_str()) == 0; }

bool renameFile(const std::string& from, const std::string& to) {
#ifdef EXBIT_WINDOWS
    if (MoveFileExW(os::widen(from).c_str(), os::widen(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        return true;
    DWORD e = GetLastError();  // (as errno, for the callers' strerror)
    errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT : e == ERROR_ACCESS_DENIED ? EACCES : EIO;
    return false;
#else
    return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

int64_t fileSize(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (int64_t)st.st_size : -1;
}

double fileMtime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
#ifdef EXBIT_WINDOWS
    return (double)st.st_mtime;  // (whole seconds: the C library's stat has no more)
#else
    return (double)st.st_mtim.tv_sec + st.st_mtim.tv_nsec * 1e-9;
#endif
}

std::vector<DirEntry> listDir(const std::string& path) {
    std::vector<DirEntry> out;
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        bool dir = false;
#ifdef EXBIT_WINDOWS
        DWORD attr = GetFileAttributesW(os::widen(joinPath(path, e->d_name)).c_str());  // (MinGW's dirent has no d_type)
        dir = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
        bool hidden = attr != INVALID_FILE_ATTRIBUTES && (attr & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
        out.push_back({e->d_name, dir, hidden});
        continue;
#else
        if (e->d_type == DT_DIR) dir = true;
        else if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) dir = isDir(joinPath(path, e->d_name));
#endif
        out.push_back({e->d_name, dir});
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        return compareNatural(a.name, b.name) < 0;
    });
    return out;
}

static bool walk(const std::string& dir, const std::function<bool(const std::string&)>& fn,
                 std::set<std::pair<dev_t, std::string>>& seen) {
    struct stat st;
    if (stat(dir.c_str(), &st) != 0) return true;
#ifdef EXBIT_WINDOWS
    if (!seen.insert({0, lower(absolutePath(dir))}).second) return true;  // (no inode numbers: the folder's path)
#else
    if (!seen.insert({st.st_dev, std::to_string(st.st_ino)}).second) return true;
#endif
    DIR* d = opendir(dir.c_str());
    if (!d) return true;
    std::vector<std::string> subdirs;
    bool go = true;
    while (go) {
        dirent* e = readdir(d);
        if (!e) break;
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        std::string p = joinPath(dir, e->d_name);
#ifdef EXBIT_WINDOWS
        bool isdir = isDir(p);
#else
        bool isdir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) isdir = isDir(p);
#endif
        if (isdir) subdirs.push_back(p);
        else go = fn(p);
    }
    closedir(d);
    std::sort(subdirs.begin(), subdirs.end());
    for (auto& s : subdirs) {
        if (!go) break;
        go = walk(s, fn, seen);
    }
    return go;
}

void walkFiles(const std::string& root, const std::function<bool(const std::string& path)>& fn) {
    std::set<std::pair<dev_t, std::string>> seen;
    walk(root, fn, seen);
}
#endif  // EXBIT_WINE_PE

#ifdef EXBIT_WINDOWS
// "C:/…" (or "C:" alone): the drive's 2 characters; else 0
static size_t drivePrefix(const std::string& p) {
    return p.size() >= 2 && p[1] == ':' && std::isalpha((unsigned char)p[0]) ? 2 : 0;
}
#endif

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (b[0] == '/') return b;
#ifdef EXBIT_WINDOWS
    if (drivePrefix(b)) return b;
#endif
    return a.back() == '/' ? a + b : a + "/" + b;
}

std::string dirName(const std::string& path) {
    size_t i = path.find_last_of('/');
    if (i == std::string::npos) return "";
    if (i == 0) return "/";
#ifdef EXBIT_WINDOWS
    if (i == 2 && drivePrefix(path)) return path.substr(0, 3);  // ("C:/x" -> "C:/")
#endif
    return path.substr(0, i);
}

std::string baseName(const std::string& path) {
#ifdef EXBIT_WINDOWS
    size_t i = path.find_last_of("/\\");  // (a path that came in with Windows' own '\\': its name all the same)
#else
    size_t i = path.find_last_of('/');
#endif
    return i == std::string::npos ? path : path.substr(i + 1);
}

std::string stem(const std::string& path) {
    std::string b = baseName(path);
    size_t i = b.find_last_of('.');
    return i == std::string::npos || i == 0 ? b : b.substr(0, i);
}

std::string extension(const std::string& path) {
    std::string b = baseName(path);
    size_t i = b.find_last_of('.');
    return i == std::string::npos || i == 0 ? "" : lower(b.substr(i));
}

std::string absolutePath(const std::string& path) {
    if (path.empty()) return path;
    std::string p = expandUser(path);
#ifdef EXBIT_WINDOWS
    // shortcut: "\\server\share" (UNC) paths and "C:x" (relative to a drive's own folder) aren't handled
    p = os::portablePath(p);
    std::string drive;
    if (!drivePrefix(p)) {
        char buf[4096];
        if (p[0] != '/' && _getcwd(buf, sizeof buf)) p = joinPath(os::portablePath(buf), p);
        else if (p[0] == '/' && _getcwd(buf, sizeof buf) && drivePrefix(buf)) p = std::string(buf, 2) + p;
    }
    if (drivePrefix(p)) {
        drive = p.substr(0, 2);
        drive[0] = (char)std::toupper((unsigned char)drive[0]);
        p = p.substr(2);
    }
#else
    if (p[0] != '/') {
        char buf[4096];
        if (getcwd(buf, sizeof buf)) p = joinPath(buf, p);
    }
#endif
    // normalise "." and ".."
    std::vector<std::string> parts;
    for (auto& part : split(p, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(part);
        }
    }
    std::string out;
    for (auto& part : parts) out += "/" + part;
#ifdef EXBIT_WINDOWS
    return drive + (out.empty() ? "/" : out);
#else
    return out.empty() ? "/" : out;
#endif
}

std::string expandUser(const std::string& path) {
    if (path.size() >= 1 && path[0] == '~' && (path.size() == 1 || path[1] == '/'))
        return plat::homeDir() + path.substr(1);
    return path;
}

std::string lower(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t i = s.find(sep, start);
        if (i == std::string::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, i - start));
        start = i + 1;
    }
}

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isspace((unsigned char)c)) {
            if (!cur.empty()) out.push_back(cur), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool startsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool endsWith(const std::string& s, const std::string& x) { return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0; }
bool containsNoCase(const std::string& hay, const std::string& needle) { return lower(hay).find(lower(needle)) != std::string::npos; }

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t i = 0;
    while ((i = s.find(from, i)) != std::string::npos) {
        s.replace(i, from.size(), to);
        i += to.size();
    }
    return s;
}

std::string format(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::string out;
    if (n < (int)sizeof buf) {
        out.assign(buf, n < 0 ? 0 : n);
    } else {
        out.resize((size_t)n + 1);
        vsnprintf(&out[0], out.size(), fmt, ap2);
        out.resize((size_t)n);
    }
    va_end(ap2);
    return out;
}

std::string fixed(double v, int decimals) { return format("%.*f", decimals, v); }

int compareNatural(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[j];
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit((unsigned char)a[i2])) i2++;
            while (j2 < b.size() && std::isdigit((unsigned char)b[j2])) j2++;
            std::string na = a.substr(i, i2 - i), nb = b.substr(j, j2 - j);
            na.erase(0, std::min(na.find_first_not_of('0'), na.size()));
            nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size()));
            if (na.size() != nb.size()) return na.size() < nb.size() ? -1 : 1;
            if (int c = na.compare(nb)) return c < 0 ? -1 : 1;
            i = i2, j = j2;
            continue;
        }
        int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb ? -1 : 1;
        i++, j++;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return a.compare(b) < 0 ? -1 : (a == b ? 0 : 1);
}

double wallClock() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

}  // namespace util

namespace settings {
namespace {
std::mutex lock;
Json data;
bool loaded = false;
std::string path;

void load() {
    if (loaded) return;
    loaded = true;
    if (path.empty()) path = util::joinPath(plat::configDir(), "settings.json");
    std::string text;
    if (util::readFile(path, text)) data = Json::parse(text);
    if (!data.isObject()) data = Json::object();
}

void save() {
    util::makeDirs(util::dirName(path));
    util::writeFile(path, data.dump(1));
}
}  // namespace

const Json& get(const std::string& key) {
    std::lock_guard<std::mutex> hold(lock);
    load();
    return static_cast<const Json&>(data)[key];
}

Json value(const std::string& key, const Json& def) {
    std::lock_guard<std::mutex> hold(lock);
    load();
    const Json& j = static_cast<const Json&>(data)[key];
    return j.isNull() ? def : j;
}

void set(const std::string& key, const Json& v) {
    std::lock_guard<std::mutex> hold(lock);
    load();
    data.set(key, v);
    save();
}

void remove(const std::string& key) {
    std::lock_guard<std::mutex> hold(lock);
    load();
    data.erase(key);
    save();
}

void setPath(const std::string& p) {
    std::lock_guard<std::mutex> hold(lock);
    path = p;
    loaded = false;
    data = Json();
}

void flush() {
    std::lock_guard<std::mutex> hold(lock);
    if (loaded) save();
}

std::string filePath() {
    std::lock_guard<std::mutex> hold(lock);
    load();
    return path;
}
}  // namespace settings
