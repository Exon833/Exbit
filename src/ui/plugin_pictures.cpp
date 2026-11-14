// Pictures of plugins for the patch panel (plugin_pictures.py).
#include "ui/plugin_pictures.h"

#include "paths.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>

#include "core/json.h"
#include "core/util.h"
#include "platform/platform.h"

namespace plugin_pictures {

namespace {
std::string folderOverride;
struct Cached { double mtime; std::shared_ptr<const Image> image; };
std::map<std::string, Cached>& loaded() {  // file -> (its mtime, picture)
    static std::map<std::string, Cached> m;
    return m;
}

std::string upper(std::string s) {
    for (auto& ch : s)
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    return s;
}

// re.sub(r"[^\w.-]+", "_", name).strip("_")[:40] or "plugin" (\w: letters, digits, _, any non-ASCII letter)
std::string readableName(const std::string& name) {
    std::string out;
    bool inRun = false;
    for (size_t i = 0; i < name.size();) {
        unsigned char ch = (unsigned char)name[i];
        size_t len = ch < 0x80 ? 1 : ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : ch >= 0xC0 ? 2 : 1;
        bool word = ch >= 0x80 || std::isalnum(ch) || ch == '_' || ch == '.' || ch == '-';
        if (word) {
            out += name.substr(i, len);
            inRun = false;
        } else if (!inRun) {
            out += '_';
            inRun = true;
        }
        i += len;
    }
    size_t a = out.find_first_not_of('_');
    if (a == std::string::npos) return "plugin";
    size_t b = out.find_last_not_of('_');
    out = out.substr(a, b - a + 1);
    // the first 40 characters (not bytes)
    size_t i = 0;
    int count = 0;
    while (i < out.size() && count < 40) {
        unsigned char ch = (unsigned char)out[i];
        i += ch < 0x80 ? 1 : ch >= 0xF0 ? 4 : ch >= 0xE0 ? 3 : ch >= 0xC0 ? 2 : 1;
        ++count;
    }
    out = out.substr(0, std::min(i, out.size()));
    return out.empty() ? "plugin" : out;
}

// moduleinfo.json is JSON5-ish: drop // comment lines and trailing commas (as the Python program's regexes)
std::string relaxed(const std::string& text) {
    std::string noComments;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t first = line.find_first_not_of(" \t\r");
        if (!(first != std::string::npos && line.compare(first, 2, "//") == 0)) noComments += line;
        if (end == std::string::npos) break;
        noComments += '\n';
        start = end + 1;
    }
    std::string out;
    for (size_t i = 0; i < noComments.size(); ++i) {
        if (noComments[i] == ',') {
            size_t j = i + 1;
            while (j < noComments.size() && std::isspace((unsigned char)noComments[j])) ++j;
            if (j < noComments.size() && (noComments[j] == '}' || noComments[j] == ']')) continue;
        }
        out += noComments[i];
    }
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out = out.substr(3);  // (utf-8-sig)
    return out;
}
}  // namespace

std::string sha1Hex(const std::string& data) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string msg = data;
    uint64_t bits = (uint64_t)data.size() * 8;
    msg += (char)0x80;
    while (msg.size() % 64 != 56) msg += (char)0;
    for (int i = 7; i >= 0; --i) msg += (char)((bits >> (i * 8)) & 0xff);
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)(uint8_t)msg[chunk + i * 4] << 24) | ((uint32_t)(uint8_t)msg[chunk + i * 4 + 1] << 16) |
                   ((uint32_t)(uint8_t)msg[chunk + i * 4 + 2] << 8) | (uint32_t)(uint8_t)msg[chunk + i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::string out;
    for (uint32_t v : h) out += util::format("%08x", v);
    return out;
}

std::string folder() {
    return folderOverride.empty() ? paths::dataDir("plugin_pictures") : folderOverride;
}

void setFolder(const std::string& f) { folderOverride = f; }

std::string fileFor(const PluginSlot& slot) {
    std::string key = slot.path + "|" + slot.pluginName;
    return util::joinPath(folder(), readableName(slot.name) + "-" + sha1Hex(key).substr(0, 10) + ".png");
}

std::string bundleOf(const std::string& path) {
    std::string bundle = util::dirName(util::dirName(util::dirName(path)));
    return util::endsWith(util::lower(bundle), ".vst3") && util::isDir(bundle) ? bundle : "";
}

std::string snapshot(const PluginSlot& slot) {
    std::string bundle = bundleOf(slot.path);
    if (bundle.empty()) return "";
    std::string dir = util::joinPath(bundle, "Contents/Resources/Snapshots");
    std::vector<std::string> files;
    for (auto& e : util::listDir(dir)) {
        if (e.isDir) continue;
        if (e.name.find("_snapshot") != std::string::npos && util::endsWith(util::lower(e.name), ".png"))
            files.push_back(e.name);
    }
    std::set<std::string> ids;
    for (auto& f : files) ids.insert(upper(f.substr(0, f.find("_snapshot"))));
    if (ids.size() > 1) {  // several plugins in one bundle: the one named so in its moduleinfo.json
        std::string text;
        if (!util::readFile(util::joinPath(bundle, "Contents/Resources/moduleinfo.json"), text)) return "";
        std::string error;
        Json doc = Json::parse(relaxed(text), &error);
        if (!error.empty() || !doc.isObject()) return "";
        std::set<std::string> named;
        for (auto& c : doc["Classes"].items()) {
            std::string n = c.str("Name");
            if (n == slot.pluginName || n == slot.name) named.insert(upper(c.str("CID")));
        }
        std::set<std::string> both;
        for (auto& id : ids)
            if (named.count(id)) both.insert(id);
        ids = both;
    }
    if (ids.size() != 1) return "";
    std::string id = *ids.begin();
    std::vector<std::string> mine;
    for (auto& f : files)
        if (upper(f).rfind(id, 0) == 0) mine.push_back(f);
    // the high-resolution one first
    std::stable_sort(mine.begin(), mine.end(), [](const std::string& a, const std::string& b) {
        return (a.find("_2.0x") == std::string::npos ? 1 : 0) < (b.find("_2.0x") == std::string::npos ? 1 : 0);
    });
    return mine.empty() ? "" : util::joinPath(dir, mine[0]);
}

std::shared_ptr<const Image> picture(const PluginSlot& slot) {
    for (const std::string& file : {fileFor(slot), snapshot(slot)}) {
        if (file.empty() || !util::fileExists(file) || util::isDir(file)) continue;
        double mtime = util::fileMtime(file);
        auto it = loaded().find(file);
        if (it == loaded().end() || it->second.mtime != mtime) {
            auto image = std::make_shared<Image>();
            if (!loadPng(file, *image) || image->empty()) continue;
            loaded()[file] = {mtime, image};
            it = loaded().find(file);
        }
        return it->second.image;
    }
    return nullptr;
}

bool hasPicture(const PluginSlot& slot) { return picture(slot) != nullptr; }

bool fromCapture(const Image& captured, Image& out) {
    int width = captured.width(), height = captured.height();
    if (width < 1 || height < 1) return false;
    int stepX = std::max(1, width / 16), stepY = std::max(1, height / 16);
    uint32_t first = captured.pixel(0, 0) | 0xff000000u;
    bool varied = false;
    for (int x = 0; x < width && !varied; x += stepX)
        for (int y = 0; y < height; y += stepY)
            if ((captured.pixel(x, y) | 0xff000000u) != first) {
                varied = true;
                break;
            }
    if (!varied) return false;
    out = captured;
    // (opaque, as QImage's RGB32)
    for (int y = 0; y < height; ++y) {
        uint32_t* row = out.row(y);
        for (int x = 0; x < width; ++x) row[x] |= 0xff000000u;
    }
    return true;
}

bool save(const PluginSlot& slot, const Image& image) {
    if (image.empty()) return false;
    Image kept = image;
    if (image.width() > WIDTH) {
        int h = std::max(1, (int)std::lround((double)image.height() * WIDTH / image.width()));
        kept = image.scaled(WIDTH, h);
    }
    if (!util::makeDirs(folder())) return false;
    return savePng(fileFor(slot), kept);
}

}  // namespace plugin_pictures
