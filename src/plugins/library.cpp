// The plugins on this computer: daw/audio/plugins.py's PluginLibrary (see library.h).
#include "plugins/library.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include <sys/stat.h>

#include "core/util.h"
#include "platform/platform.h"
#include "plugins/wine.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace plug {

#ifdef _WIN32
namespace {
// '/' for '\\' (the program's paths: core/os.h)
std::string slashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 3 && s.back() == '/') s.pop_back();
    return s;
}

// an environment variable's folder (Windows' own names: %CommonProgramFiles%…), else `otherwise`
std::string envFolder(const char* name, const char* otherwise) {
    const char* v = getenv(name);
    return slashes(v && *v ? v : otherwise);
}

// VST2's folder as installers write it in the registry (HKLM, then HKCU: SOFTWARE\VST, VSTPluginsPath)
std::vector<std::string> registryVst2Folders() {
    std::vector<std::string> out;
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
        for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            wchar_t value[1024];
            DWORD size = sizeof value;
            if (RegGetValueW(root, L"SOFTWARE\\VST", L"VSTPluginsPath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | view, nullptr,
                             value, &size) != ERROR_SUCCESS)
                continue;
            int n = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 0 ? n - 1 : 0, '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, value, -1, &s[0], n, nullptr, nullptr);
            s = slashes(s);
            if (!s.empty() && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
        }
    return out;
}
}  // namespace
#endif

std::vector<std::string> defaultFolders() {
#ifdef _WIN32
    // the VST3 folder (and, for 32-bit plugins, its 32-bit twin), and the one of a plugin installed for one user
    std::string common = envFolder("CommonProgramFiles", "C:/Program Files/Common Files");
    std::string common32 = envFolder("CommonProgramFiles(x86)", "C:/Program Files (x86)/Common Files");
    std::vector<std::string> f = {common + "/VST3"};
    if (util::lower(common32) != util::lower(common)) f.push_back(common32 + "/VST3");
    if (const char* local = getenv("LOCALAPPDATA"))
        f.push_back(slashes(local) + "/Programs/Common/VST3");
    return f;
#else
    return {util::joinPath(plat::homeDir(), ".vst3"), "/usr/lib/vst3", "/usr/local/lib/vst3"};
#endif
}

std::vector<std::string> vst2UsualFolders() {
#ifdef _WIN32
    // the registry's, then the usual ones, in Program Files and (32-bit plugins) Program Files (x86)
    std::vector<std::string> f = registryVst2Folders();
    for (const std::string& root : {envFolder("ProgramFiles", "C:/Program Files"),
                                    envFolder("ProgramFiles(x86)", "C:/Program Files (x86)")})
        for (const char* rel : {"/VstPlugins", "/Steinberg/VstPlugins", "/Common Files/VST2", "/Common Files/Steinberg/VST2"})
            if (std::find(f.begin(), f.end(), root + rel) == f.end()) f.push_back(root + rel);
    return f;
#else
    return {util::joinPath(plat::homeDir(), ".vst"), "/usr/lib/vst", "/usr/local/lib/vst", "/usr/lib/lxvst"};
#endif
}

bool PluginInfo::is32Bit() const {
#ifdef _WIN32
    static std::mutex lock;
    static std::map<std::string, bool> known;  // (labels are asked for often; a file's header once)
    std::lock_guard<std::mutex> hold(lock);
    auto it = known.find(path);
    if (it == known.end()) it = known.emplace(path, wine::is32BitPlugin(loadableFile(path))).first;
    return it->second;
#else
    static std::mutex lock;
    static std::map<std::string, bool> known;  // (labels are asked for often; a file's header once)
    std::lock_guard<std::mutex> hold(lock);
    auto it = known.find(path);
    if (it != known.end())
        return it->second;
    bool bits32 = looksWindows() && wine::is32BitPlugin(path);
    known[path] = bits32;
    return bits32;
#endif
}

bool PluginInfo::looksWindows() const {
#ifdef _WIN32
    return false;
#else
    return wine::looksWindows(path);
#endif
}

std::string defaultCachePath() { return util::joinPath(plat::configDir(), "plugin_cache.json"); }

PluginChoiceNeeded::PluginChoiceNeeded(std::vector<std::string> n)
    : std::runtime_error([&] {
          std::string s;
          for (size_t i = 0; i < n.size(); i++)
              s += (i ? ", " : "") + n[i];
          return s;
      }()),
      names(std::move(n)) {}

std::string loadableFile(const std::string& path) {
#ifndef TINYDAW_IN_HOST
    if (util::isDir(path) && !util::fileExists(vst3Binary(path)))  // (64-bit, else 32-bit: in a host for it)
        for (const char* arch : {"Contents/x86_64-win", "Contents/x86-win"}) {
            std::string windows = util::joinPath(util::joinPath(path, arch), util::baseName(path));
            if (util::fileExists(windows) && !util::isDir(windows))
                return windows;
        }
#endif
#if defined(_WIN32) && defined(__i386__)
    std::string inner = util::joinPath(util::joinPath(path, "Contents\\x86-win"), util::baseName(path));
#elif defined(_WIN32)
    std::string inner = util::joinPath(util::joinPath(path, "Contents\\x86_64-win"), util::baseName(path));
#else
    std::string inner = vst3Binary(path);
#endif
    return util::isDir(path) && util::fileExists(inner) && !util::isDir(inner) ? inner : path;
}

namespace {

// a JSON5-ish text (comment lines, trailing commas) as JSON: re.sub(r"(?m)^\s*//[^\n]*", "") and
// re.sub(r",(\s*[}\]])", r"\1")
std::string relaxedJson(const std::string& text) {
    std::string t = text;
    if (t.compare(0, 3, "\xEF\xBB\xBF") == 0)
        t = t.substr(3);  // (utf-8-sig)
    std::string noComments;
    size_t i = 0;
    while (i < t.size()) {
        size_t end = t.find('\n', i);
        std::string line = t.substr(i, end == std::string::npos ? std::string::npos : end - i);
        size_t first = line.find_first_not_of(" \t\r\f\v");
        if (!(first != std::string::npos && line.compare(first, 2, "//") == 0))
            noComments += line;
        else
            noComments += line.substr(0, first);  // (the comment goes; what was before it stays)
        if (end == std::string::npos)
            break;
        noComments += '\n';
        i = end + 1;
    }
    std::string out;
    for (size_t k = 0; k < noComments.size(); k++) {
        char c = noComments[k];
        if (c == ',') {
            size_t j = k + 1;
            while (j < noComments.size() && std::isspace((unsigned char)noComments[j]))
                j++;
            if (j < noComments.size() && (noComments[j] == '}' || noComments[j] == ']'))
                continue;
        }
        out += c;
    }
    return out;
}

Json infosJson(const std::vector<PluginInfo>& infos) {
    Json list = Json::array();
    for (const auto& p : infos)
        list.push(Json::object({{"name", p.name},
                                {"kind", p.kind.empty() ? Json() : Json(p.kind)},
                                {"plugin_name", p.pluginName.empty() ? Json() : Json(p.pluginName)}}));
    return list;
}

std::vector<PluginInfo> infosFrom(const std::string& path, const Json& entry) {
    std::vector<PluginInfo> found;
    for (const auto& e : entry["plugins"].items())
        found.push_back({path, e.str("name"), e.str("kind"), e.str("plugin_name")});
    return found;
}

void sortByName(std::vector<PluginInfo>& list) {
    std::stable_sort(list.begin(), list.end(),
                     [](const PluginInfo& a, const PluginInfo& b) { return util::lower(a.name) < util::lower(b.name); });
}

// glob(os.path.join(folder, "**", "*" + suffix), recursive=True): files and folders (hidden ones
// left out, as glob does), sorted. Inside a Wine prefix (a folder chosen may be one, or hold one) no
// symlinked folder is followed: Wine's all lead out of it (dosdevices/z: to "/", the users' folders
// to their home folders), and following them walked the whole disk; nor are dosdevices and
// drive_c/windows (Windows' own DLLs, no plugins) looked through. Never more than MAX_FOLDERS folders.
constexpr int MAX_FOLDERS = 50000;

struct Walk {
    std::set<std::pair<dev_t, ino_t>> seen;
    std::set<std::string> seenPaths;  // (Windows: no inode numbers; its folders, as named, lowercase)
    int folders = 0;
};

bool isWinePrefix(const std::string& folder) {
#ifdef _WIN32
    (void)folder;
    return false;
#else
    return wine::isPrefix(folder);
#endif
}

void findNamed(const std::string& folder, const std::string& suffix, std::vector<std::string>& out, Walk& walk,
               bool inWine = false, int depth = 0) {
    struct stat st;
#ifdef _WIN32
    if (depth > 32 || ++walk.folders > MAX_FOLDERS || stat(folder.c_str(), &st) != 0 ||
        !walk.seenPaths.insert(util::lower(folder)).second)
        return;
#else
    if (depth > 32 || ++walk.folders > MAX_FOLDERS || stat(folder.c_str(), &st) != 0 ||
        !walk.seen.insert({st.st_dev, st.st_ino}).second)
        return;
#endif
    bool prefix = !inWine && isWinePrefix(folder);
    inWine = inWine || prefix;
    for (const auto& e : util::listDir(folder)) {
        if (e.name.empty() || e.name[0] == '.')
            continue;
        std::string path = util::joinPath(folder, e.name);
        if (util::endsWith(e.name, suffix))
            out.push_back(path);
        if (!e.isDir)
            continue;
#ifndef _WIN32  // (no Wine prefixes there)
        if (inWine) {
            struct stat link;
            if (lstat(path.c_str(), &link) == 0 && S_ISLNK(link.st_mode))
                continue;  // (a link out of the prefix)
            if ((prefix && e.name == "dosdevices") ||
                (util::baseName(folder) == "drive_c" && util::lower(e.name) == "windows"))
                continue;
        }
#endif
        findNamed(path, suffix, out, walk, inWine, depth + 1);
    }
}

std::vector<std::string> globAll(const std::string& folder, const std::string& suffix) {
    std::vector<std::string> out;
    Walk walk;
    findNamed(folder, suffix, out, walk);
    std::sort(out.begin(), out.end());
    return out;
}

double secondsNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

bool readModuleinfo(const std::string& bundle, KindList& classes) {
    classes.clear();
    std::string info = util::joinPath(util::joinPath(bundle, "Contents/Resources"), "moduleinfo.json");
    std::string text;
    if (!util::fileExists(info) || util::isDir(info) || !util::readFile(info, text))
        return false;
    std::string error;
    Json doc = Json::parse(relaxedJson(text), &error);
    if (!error.empty() || !doc.isObject())
        return false;
    for (const auto& c : doc["Classes"].items()) {
        if (c.str("Category") != "Audio Module Class")
            continue;
        bool instrument = false;
        for (const auto& s : c["Sub Categories"].items())
            if (s.toString().find("Instrument") != std::string::npos)
                instrument = true;
        classes.push_back({c.str("Name"), instrument ? INSTRUMENT : EFFECT});
    }
    return !classes.empty();
}

PluginLibrary::PluginLibrary(std::vector<std::string> folders_, std::string cachePath_, std::vector<std::string> vst2)
    : folders(std::move(folders_)), vst2Folders(std::move(vst2)), cachePath(cachePath_.empty() ? defaultCachePath() : cachePath_) {
    std::string text;
    if (util::readFile(cachePath, text))
        cache = Json::parse(text);
    if (!cache.isObject())
        cache = Json::object();
    plugins = scan();
}

std::vector<PluginInfo> PluginLibrary::scan() {
    std::vector<PluginInfo> found;
    for (const auto& folder : folders) {
        for (const auto& path : globAll(folder, ".vst3")) {
            if (path.find("/Contents/") != std::string::npos || path.find("\\Contents\\") != std::string::npos)
                continue;  // the inner file of a bundle: listed through the bundle
            std::string file = loadableFile(path);
            std::string stem = util::stem(path);
            const Json& cached = std::as_const(cache)[file];
            if (cached.isObject() && cached["mtime"].isNumber() && cached["mtime"].toDouble() == util::fileMtime(file)) {
                auto infos = infosFrom(file, cached);
                found.insert(found.end(), infos.begin(), infos.end());
                continue;
            }
            KindList classes;
            bool known = util::isDir(path) && readModuleinfo(path, classes);
            if (known && classes.size() > 1) {
                for (const auto& c : classes)
                    found.push_back({file, c.first, c.second, c.first});
            } else if (known) {
                found.push_back({file, classes[0].first, classes[0].second, ""});
            } else {
                found.push_back({file, stem, "", ""});
            }
        }
    }
    auto vst2 = scanVst2();
    util::debugf("plugins: %zu VST3 and %zu VST2 found in %zu folder(s)\n", found.size(), vst2.size(), folders.size());
    found.insert(found.end(), vst2.begin(), vst2.end());
    sortByName(found);
    return found;
}

std::vector<PluginInfo> PluginLibrary::scanVst2() {
    std::vector<PluginInfo> found;
    bool changed = false;
#ifdef _WIN32
    const std::vector<std::string> suffixes = {".dll", ".DLL"};
#else
    const std::vector<std::string> suffixes = {".so", ".dll", ".DLL"};  // (.dll: Windows plugins, through Wine)
#endif
    std::vector<std::string> files;
    for (const auto& folder : vst2Folders)
        for (const auto& suffix : suffixes)
            for (const auto& path : globAll(folder, suffix))
                if (std::find(files.begin(), files.end(), path) == files.end())
                    files.push_back(path);
    {
        for (const auto& path : files) {
            if (util::isDir(path) || isVst3Path(path))
                continue;
            double mtime = util::fileMtime(path);
            if (mtime == 0)
                continue;
            const Json* cached = &std::as_const(cache)[path];
            if (!(cached->isObject() && (*cached)["mtime"].isNumber() && (*cached)["mtime"].toDouble() == mtime)) {
                std::vector<PluginInfo> plugins;
                if (isVst2File(path))
                    plugins.push_back({path, util::stem(path) + VST2_SUFFIX, "", ""});
                cache.set(path, Json::object({{"mtime", mtime}, {"plugins", infosJson(plugins)}}));  // (not a plugin: remembered too)
                cached = &std::as_const(cache)[path];
                changed = true;
            }
            auto infos = infosFrom(path, *cached);
            found.insert(found.end(), infos.begin(), infos.end());
        }
    }
    if (changed)
        saveCache();
    return found;
}

std::vector<std::string> PluginLibrary::unknownFiles() const {
    std::vector<std::string> files;
    for (const auto& p : plugins)
        if (p.kind.empty() && std::find(files.begin(), files.end(), p.path) == files.end())
            files.push_back(p.path);
    std::vector<std::string> result;
    for (const auto& f : files)
        if (!probeFailedBefore(f))
            result.push_back(f);
    return result;
}

bool PluginLibrary::probeFailedBefore(const std::string& file) const {
    const Json& cached = cache[file];
    if (!util::fileExists(file))
        return true;
    if (!cached.isObject())
        return false;
    const Json& failed = cached["probe_failed"];
    bool truthy = failed.isString() ? !failed.toString().empty() : failed.toBool(false);
    return truthy && cached["mtime"].isNumber() && cached["mtime"].toDouble() == util::fileMtime(file);
}

void PluginLibrary::learn(const std::string& file, const KindList& classes) {
    std::vector<PluginInfo> listed;
    for (const auto& p : plugins)
        if (p.path == file)
            listed.push_back(p);
    std::vector<PluginInfo> infos;
    if (classes.size() == 1 && listed.size() == 1) {
        infos.push_back({file, listed[0].name, classes[0].second, listed[0].pluginName});
    } else {
        for (const auto& c : classes)
            infos.push_back({file, c.first, c.second, c.first});
    }
    remember(file, infos);
}

void PluginLibrary::probeFailed(const std::string& file, const std::string& why) {
    if (!util::fileExists(file))
        return;
    double mtime = util::fileMtime(file);
    std::vector<PluginInfo> listed;
    for (const auto& p : plugins)
        if (p.path == file)
            listed.push_back(p);
    cache.set(file, Json::object({{"mtime", mtime}, {"probe_failed", why}, {"plugins", infosJson(listed)}}));
    saveCache();
}

std::vector<PluginInfo> PluginLibrary::ofKind(const std::string& kind) const {
    std::vector<PluginInfo> result;
    for (const auto& p : plugins)
        if (p.kind == kind || p.kind.empty())
            result.push_back(p);
    return result;
}

std::unique_ptr<Plugin> PluginLibrary::load(const PluginInfo& info, double sampleRate) {
    if (isBuiltinPath(info.path)) {  // (a built-in effect: src/effects; nothing to learn or time)
        std::string error;
        auto effect = Plugin::load(info.path, "", error);
        if (!effect)
            throw std::runtime_error(error);
        effect->prepare(sampleRate);
        return effect;
    }
    double started = secondsNow();
    std::string error;
    std::unique_ptr<Plugin> plugin;
    if (isVst2Path(info.path)) {
        plugin = Plugin::load(info.path, "", error);
    } else {
        if (info.pluginName.empty()) {
            auto classes = vst3Classes(info.path, error);
            if (classes.empty() && !error.empty())
                throw std::runtime_error(error);
            if (classes.size() > 1) {
                std::vector<std::string> names;
                std::vector<PluginInfo> infos;
                for (const auto& c : classes) {
                    names.push_back(c.name);
                    infos.push_back({info.path, c.name, "", c.name});
                }
                remember(info.path, infos);
                throw PluginChoiceNeeded(names);
            }
        }
        plugin = Plugin::load(info.path, info.pluginName, error);
    }
    if (!plugin)
        throw std::runtime_error(error.empty() ? "Cannot load " + info.path : error);
    plugin->prepare(sampleRate);
    std::string kind = plugin->isInstrument() ? INSTRUMENT : EFFECT;
    Json& times = cache[LOAD_TIMES];
    if (!times.isObject())
        times = Json::object();
    times.set(loadKey(info), std::round((secondsNow() - started) * 100.0) / 100.0);
    if (info.kind != kind) {
        std::vector<PluginInfo> sameFile;
        for (const auto& p : plugins)
            if (p.path == info.path && p != info)
                sameFile.push_back(p);
        sameFile.push_back({info.path, info.name, kind, info.pluginName});
        remember(info.path, sameFile);
    } else {
        saveCache();
    }
    return plugin;
}

std::string PluginLibrary::loadKey(const PluginInfo& info) { return info.path + "|" + info.pluginName; }

double PluginLibrary::lastLoadSeconds(const PluginInfo& info) const {
    const Json& v = cache[LOAD_TIMES][loadKey(info)];
    return v.isNumber() ? v.toDouble() : -1;
}

void PluginLibrary::saveCache() {
    util::makeDirs(util::dirName(cachePath));
    util::writeFile(cachePath, cache.dump(1));  // (only a cache: a failure doesn't matter)
}

void PluginLibrary::remember(const std::string& file, const std::vector<PluginInfo>& infos) {
    cache.set(file, Json::object({{"mtime", util::fileMtime(file)}, {"plugins", infosJson(infos)}}));
    saveCache();
    std::vector<PluginInfo> kept;
    for (const auto& p : plugins)
        if (p.path != file)
            kept.push_back(p);
    kept.insert(kept.end(), infos.begin(), infos.end());
    sortByName(kept);
    plugins = kept;
}

// --- states ---

std::string stateOf(Plugin& plugin) {
    std::string component = plugin.componentState();  // (first: a Windows plugin gives both at once)
    return packState(component, plugin.controllerState());
}

void setState(Plugin& plugin, const std::string& state) {
    std::string component, controller;
    if (!unpackState(state, component, controller))
        throw std::invalid_argument("unknown plugin state");
    plugin.setState(component, controller);
}

}  // namespace plug
