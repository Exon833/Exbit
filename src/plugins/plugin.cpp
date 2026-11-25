// Plugin::load (VST3 or VST2 by the path), the state blob of a slot ("TDV3"), and reading the
// states of old projects (JUCE's format, juce_state.py).
#include "plugins/plugin.h"

#include <cstring>

#include "core/util.h"
#include "plugins/library.h"
#ifndef EXBIT_IN_HOST
#include "plugins/wine.h"
#endif

namespace plug {

std::unique_ptr<Plugin> loadVst3(const std::string& path, const std::string& name, std::string& error);  // vst3host.cpp
std::unique_ptr<Plugin> loadVst2(const std::string& path, std::string& error);                           // vst2host.cpp

bool isVst3Path(const std::string& path) {
    std::string p = util::lower(path);
#ifdef _WIN32
    for (auto& c : p)
        if (c == '\\')
            c = '/';
#endif
    while (p.size() > 1 && p.back() == '/')
        p.pop_back();
    return util::endsWith(p, ".vst3") || p.find(".vst3/contents/") != std::string::npos;
}

BuiltinFactory createBuiltin = nullptr;  // (set by src/effects/effects.cpp)

std::unique_ptr<Plugin> Plugin::load(const std::string& path, const std::string& name, std::string& error) {
    error.clear();
    if (isBuiltinPath(path)) {
        if (createBuiltin)
            return createBuiltin(path, error);
        error = "There is no built-in effect " + path.substr(8) + " (its script is not in the effects folder)";
        return nullptr;
    }
#ifndef EXBIT_IN_HOST
    if (wine::isWindowsPlugin(path))  // (a Windows plugin: through Wine, in a host of its own; on Windows, every one)
        return loadBridged(path, name, error);
#endif
    if (isVst3Path(path))
        return loadVst3(path, name, error);
    return loadVst2(path, error);
}

static void putU32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; i++)
        out += (char)((v >> (8 * i)) & 0xFF);
}

static uint32_t getU32(const std::string& s, size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)(unsigned char)s[at + i] << (8 * i);
    return v;
}

std::string packState(const std::string& component, const std::string& controller) {
    std::string out = STATE_MAGIC;
    putU32(out, (uint32_t)component.size());
    out += component;
    out += controller;
    return out;
}

bool unpackState(const std::string& blob, std::string& component, std::string& controller) {
    if (blob.size() >= 8 && blob.compare(0, 4, STATE_MAGIC) == 0) {
        size_t size = std::min<size_t>(getU32(blob, 4), blob.size() - 8);
        component = blob.substr(8, size);
        controller = blob.substr(8 + size);
        return true;
    }
    return juceSplit(blob, component, controller);
}

// --- JUCE's states (Exbit up to 0.3.1) ---

static const uint32_t JUCE_MAGIC = 0x21324356;  // "VC2!"
static const char JUCE_ALPHABET[] = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+";

bool isJuceState(const std::string& blob) { return blob.size() >= 8 && getU32(blob, 0) == JUCE_MAGIC; }

std::string juceDecodeBase64(const std::string& text) {
    std::string t = util::trim(text);
    size_t dot = t.find('.');
    std::string count = t.substr(0, dot), chars = dot == std::string::npos ? "" : t.substr(dot + 1);
    long size = std::strtol(count.c_str(), nullptr, 10);
    if (size <= 0)
        return "";
    std::string out((size_t)size, '\0');
    size_t bit = 0;
    for (char c : chars) {
        const char* at = std::strchr(JUCE_ALPHABET, c);
        if (!at || !c)
            continue;  // (Python raises on a stray character; such states never were written)
        int value = (int)(at - JUCE_ALPHABET);
        for (int i = 0; i < 6; i++) {
            if (bit >= (size_t)size * 8)
                return out;
            if (value >> i & 1)
                out[bit >> 3] = (char)((unsigned char)out[bit >> 3] | (1 << (bit & 7)));
            bit++;
        }
    }
    return out;
}

bool juceSplit(const std::string& blob, std::string& component, std::string& controller) {
    if (!isJuceState(blob))
        return false;
    size_t size = std::min<size_t>(getU32(blob, 4), blob.size() - 8);
    std::string xml = blob.substr(8, size);
    xml = xml.substr(0, xml.find('\0'));
    auto part = [&](const std::string& tag) -> std::string {
        std::string open = "<" + tag + ">", close = "</" + tag + ">";
        size_t a = xml.find(open);
        if (a == std::string::npos)
            return "";
        a += open.size();
        size_t b = xml.find('<', a);  // ([^<]*)</tag>
        if (b == std::string::npos || xml.compare(b, close.size(), close) != 0)
            return "";
        std::string body = xml.substr(a, b - a);
        return util::trim(body).empty() ? "" : juceDecodeBase64(body);
    };
    component = part("IComponent");
    controller = part("IEditController");
    return true;
}

}  // namespace plug
