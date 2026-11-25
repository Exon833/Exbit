// A plugin's settings survive a save and reopen: the state given to a fresh instance is the one it
// keeps, through its first buffers too (Surge XT Effects reset its effect once it first processed).
// Needs Surge XT 1.3.4, skipped without it: Linux's (EXBIT_SURGE: the folder holding "Surge XT Effects.vst3"),
// Windows' under Wine (EXBIT_SURGE_WINE: the prefix holding it in Common Files/VST3), and Windows' own.
#include <cstdio>
#include <string>
#include <vector>

#include "check.h"
#include "plugins/library.h"
#include "plugins/plugin.h"
#include "plugins/wine.h"
#include "core/old_name.h"
#include "core/program_environment.h"
#include "core/util.h"

using namespace plug;

namespace {
std::string home() {
    const char* h = getenv("HOME");
    return h ? h : "";
}

// ~/.local/share/exbit-<name>, or the folder set up under the program's old name when only that one is there
std::string sharedTestFolder(const std::string& name) {
    std::string current = home() + "/.local/share/exbit-" + name;
    std::string old = home() + "/.local/share/" + old_name::NAME_PREFIX + name;
    return !util::isDir(current) && util::isDir(old) ? old : current;
}

std::string surgeFolder() {
    if (const char* d = environment::readProgramVariable("SURGE")) return d;
#ifdef _WIN32
    const char* common = getenv("CommonProgramFiles");
    return std::string(common ? common : "C:/Program Files/Common Files") + "/VST3";
#else
    return sharedTestFolder("test-plugins") + "/linux";
#endif
}

std::string surgeWinePrefix() {
    if (const char* d = environment::readProgramVariable("SURGE_WINE")) return d;
    return sharedTestFolder("test-wine");
}

bool exists(const std::string& file) {
    FILE* f = fopen(file.c_str(), "rb");
    if (f) fclose(f);
    return f != nullptr;
}

// (a Windows plugin: its host has handed over its news)
void settle(Plugin& p) { plug::settleBridged(p, 2); }

void blocks(Plugin& p, int n) {
    std::vector<float> l(512, 0.1f), r(512, 0.1f), ol(512), orr(512);
    const float* in[2] = {l.data(), r.data()};
    float* out[2] = {ol.data(), orr.data()};
    for (int i = 0; i < n; i++) {
        p.setTransport(120, 4, 4, false, 0);
        if (p.isInstrument()) p.render({}, out, 512);
        else p.process(in, nullptr, out, 512);
    }
    settle(p);
}

// the effect's settings: a parameter moved, saved, given to a fresh instance, before and after it processed
void testRoundTrip(const std::string& file) {
    std::string error;
    auto a = Plugin::load(file, "", error);
    if (!a) {
        fprintf(stderr, "cannot load %s: %s\n", file.c_str(), error.c_str());
        CHECK(false);
        return;
    }
    a->prepare(48000, 512);
    blocks(*a, 4);
    auto params = a->parameters();
    std::vector<std::pair<uint32_t, double>> moved;  // a few parameters, away from where they start
    for (auto& p : params) {
        if (p.readOnly || p.hidden || moved.size() >= 4) continue;
        double v = a->getParam(p.id);
        double to = v < 0.5 ? 0.66 : 0.21;
        a->setParam(p.id, to);
        moved.push_back({p.id, to});
    }
    blocks(*a, 4);
    for (auto& [id, to] : moved) fprintf(stderr, "  set %u to %g: now %g (%s)\n", id, to, a->getParam(id), a->paramText(id, a->getParam(id)).c_str());
    std::string saved = stateOf(*a);
    CHECK(!saved.empty());
    a.reset();

    auto b = Plugin::load(file, "", error);
    CHECK(b != nullptr);
    if (!b) return;
    b->prepare(48000, 512);
    setState(*b, saved);
    std::vector<double> before;
    for (auto& [id, to] : moved) before.push_back(b->getParam(id));
    blocks(*b, 8);
    for (int i = 0; i < 20; i++) b->idle();
    settle(*b);
    size_t i = 0;
    for (auto& [id, to] : moved) {
        double now = b->getParam(id);
        fprintf(stderr, "  %u: saved %g, after setState %g, after processing %g\n", id, to, before[i], now);
        CHECK_NEAR(before[i], to, 0.02);
        CHECK_NEAR(now, to, 0.02);
        ++i;
    }
    std::string again = stateOf(*b);
    CHECK(!again.empty());
    // what it gives back is what it was given (a fresh reopen of that holds the same values)
    auto c = Plugin::load(file, "", error);
    if (!c) return;
    c->prepare(48000, 512);
    setState(*c, again);
    blocks(*c, 8);
    for (int i = 0; i < 20; i++) c->idle();
    settle(*c);
    for (auto& [id, to] : moved) CHECK_NEAR(c->getParam(id), to, 0.02);
}

// both Surge XT plugins in `folder` (skipped if it hasn't them)
void testFolder(const std::string& folder, const char* binaryDir, const char* ext) {
    std::string fx = folder + "/Surge XT Effects.vst3", synth = folder + "/Surge XT.vst3";
    if (!exists(fx + "/Contents/" + binaryDir + "/Surge XT Effects" + ext)) {
        printf("skipped: no Surge XT in %s\n", folder.c_str());
        return;
    }
    // (a Windows plugin is loaded by its file in the bundle, as the plugin list has it)
    bool windows = std::string(ext) == ".vst3";
    if (windows) {
        fx += std::string("/Contents/") + binaryDir + "/Surge XT Effects.vst3";
        synth += std::string("/Contents/") + binaryDir + "/Surge XT.vst3";
    }
    fprintf(stderr, "Surge XT Effects (%s):\n", fx.c_str());
    testRoundTrip(fx);
    fprintf(stderr, "Surge XT (%s):\n", synth.c_str());
    testRoundTrip(synth);
}
}  // namespace

int main() {
#ifdef _WIN32
    testFolder(surgeFolder(), "x86_64-win", ".vst3");
#else
    testFolder(surgeFolder(), "x86_64-linux", ".so");
    std::string prefix = surgeWinePrefix(), why;
    if (wine::isPrefix(prefix) && wine::available(why)) {
        setenv("WINEPREFIX", prefix.c_str(), 1);
        wine::setPrefixes({prefix});
        testFolder(prefix + "/drive_c/Program Files/Common Files/VST3", "x86_64-win", ".vst3");
    } else {
        printf("skipped: no Wine test prefix %s (%s)\n", prefix.c_str(), why.c_str());
    }
#endif
    TEST_MAIN_END();
}
