#include "audio/modulation.h"

#include <algorithm>
#include <cmath>

#include "audio/automation.h"
#include "audio/plugin_audio.h"
#include "plugins/plugin.h"

using plugin_audio::active;
using plugin_audio::usable;

namespace modulation {

effects::Effect* modulatorOf(const PluginSlot* slot) {
    if (!slot) return nullptr;
    auto inst = aload(slot->instance);
    auto* e = dynamic_cast<effects::Effect*>(inst.get());
    return e && e->isModulator() ? e : nullptr;
}

bool isModulatorPath(const std::string& path) { return path.rfind("builtin:mod_", 0) == 0; }
bool isModulator(const PluginSlot* slot) { return slot && isModulatorPath(slot->path); }

// --- GUI thread ---

namespace {
bool hasSlot(const std::vector<SlotPtr>& slots, const PluginSlot* s) {
    for (auto& x : slots)
        if (x.get() == s) return true;
    return false;
}
}  // namespace

std::vector<SlotPtr> targetsFor(const Track& track, const PluginSlot* from) {
    std::vector<SlotPtr> out;
    for (auto& s : track.effects)
        if (s.get() != from) out.push_back(s);
    for (auto& s : track.instruments()) out.push_back(s);
    return out;
}

std::vector<ModCable> kept(const Track& track) {
    std::vector<ModCable> out;
    auto effects = track.effects.copy();
    auto instruments = track.instruments();
    for (auto& c : track.modCables)
        if (hasSlot(effects, c.from) && isModulator(c.from) && (hasSlot(effects, c.to) || hasSlot(instruments, c.to)))
            out.push_back(c);
    return out;
}

void resolve(Track& track) {
    for (auto& c : track.modCables) {
        if (!c.to || !c.state) continue;
        auto instance = aload(c.to->instance);
        auto target = std::atomic_load(&c.state->target);
        if (!instance || instance == target) continue;
        // (the audio thread uses the id only while `target` is the slot's instance: cleared first)
        std::atomic_store(&c.state->target, std::shared_ptr<plug::Plugin>());
        c.state->hasTarget = false;
        for (auto& p : automation::parameters(*c.to))
            if (p.name == c.param) {
                c.state->paramId = p.id;
                if (c.state->base.load() < 0) c.state->base = instance->getParam(p.id);
                c.state->applied = -1;
                c.state->hasTarget = true;
                break;
            }
        std::atomic_store(&c.state->target, instance);
    }
}

void resolve(Project& project) {
    for (auto& t : project.allTracks())
        if (!t->modCables.empty()) resolve(*t);
}

void release(const ModCable& cable) {
    if (!cable.state || !cable.state->hasTarget) return;
    auto target = std::atomic_load(&cable.state->target);
    double base = cable.state->base;
    if (target && cable.to && target == aload(cable.to->instance) && base >= 0) target->setParam(cable.state->paramId, base);
    cable.state->applied = -1;
}

void userEdited(Project& project, const PluginSlot* slot, const std::vector<uint32_t>& ids) {
    if (ids.empty() || !slot) return;
    auto inst = aload(slot->instance);
    if (!inst) return;
    for (auto& t : project.allTracks())
        for (auto& c : t->modCables) {
            if (c.to != slot || !c.state->hasTarget || std::atomic_load(&c.state->target) != inst) continue;
            uint32_t id = c.state->paramId;
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) continue;
            double now = inst->getParam(id), applied = c.state->applied;
            if (applied >= 0 && std::fabs(now - applied) < 1e-6) continue;  // (the modulation's own value)
            c.state->base = now;  // (every cable to this parameter: they share a base, below)
        }
}

// --- audio thread ---

effects::ModClock clockAt(const TempoMap& map, bool playing, int64_t frame, int frames, int sr) {
    effects::ModClock c;
    c.playing = playing;
    c.frames = frames;
    c.sr = sr;
    double seconds = playing ? (double)frame / sr : 0.0;
    c.seconds = seconds;
    c.beats = playing ? map.beatAt(seconds) : 0.0;
    c.tempo = map.bpmAt(seconds);
    c.beatsPerBar = map.beatsPerBarAt(seconds);
    return c;
}

int toNextStep(bool playing, int64_t frame, int done, int left) {
    if (!playing) return std::min(left, BLOCK - done % BLOCK);
    int64_t into = ((frame % BLOCK) + BLOCK) % BLOCK;
    return (int)std::min<int64_t>(left, BLOCK - into);
}

void apply(const std::vector<ModCable>& cables, const effects::ModClock& clock, bool instruments) {
    // (a few cables: their modulators' values worked out once each, then each parameter's sum)
    constexpr int MAX = 64;
    const PluginSlot* from[MAX];
    double value[MAX];
    int sources = 0;
    struct Target {
        plug::Plugin* plugin;
        uint32_t id;
        ModState* state;  // the first cable's: its base is the parameter's
        double sum;
    } targets[MAX];
    int count = 0;
    for (auto& c : cables) {
        ModState* st = c.state.get();
        if (!st || !st->hasTarget.load(std::memory_order_acquire)) continue;
        auto target = std::atomic_load(&st->target);
        if (!target || !c.to || target != aload(c.to->instance) || st->base.load(std::memory_order_relaxed) < 0) continue;
        if (!instruments && c.to->kind == "instrument") continue;
        double v = 0;
        int k = 0;
        for (; k < sources; k++)
            if (from[k] == c.from) break;
        if (k < sources) {
            v = value[k];
        } else {
            effects::Effect* m = active(c.from) ? modulatorOf(c.from) : nullptr;  // (bypassed: still)
            v = m ? m->modulation(clock) : 0.0;
            if (sources < MAX) from[sources] = c.from, value[sources] = v, sources++;
        }
        uint32_t id = st->paramId.load(std::memory_order_relaxed);
        int t = 0;
        for (; t < count; t++)
            if (targets[t].plugin == target.get() && targets[t].id == id) break;
        if (t == count) {
            if (count == MAX) continue;
            targets[count++] = {target.get(), id, st, 0.0};
        }
        targets[t].sum += c.depth * v;
    }
    for (int t = 0; t < count; t++) {
        Target& g = targets[t];
        double val = std::max(0.0, std::min(1.0, g.state->base.load(std::memory_order_relaxed) + g.sum));
        if (val != g.state->applied.load(std::memory_order_relaxed)) {
            g.plugin->setParam(g.id, val);
            g.state->applied.store(val, std::memory_order_relaxed);
        }
    }
    // (every cable to a parameter keeps the same last value, so userEdited sees it whichever it reads)
    for (auto& c : cables) {
        ModState* st = c.state.get();
        if (!st || !st->hasTarget.load(std::memory_order_acquire)) continue;
        uint32_t id = st->paramId.load(std::memory_order_relaxed);
        auto target = std::atomic_load(&st->target);
        for (int t = 0; t < count; t++)
            if (targets[t].plugin == target.get() && targets[t].id == id && targets[t].state != st) {
                st->applied.store(targets[t].state->applied.load(std::memory_order_relaxed), std::memory_order_relaxed);
                st->base.store(targets[t].state->base.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
    }
}

bool takeAutomation(const Track& track, const PluginSlot* slot, uint32_t paramId, double value) {
    auto cables = track.modCables.get();
    bool taken = false;
    for (auto& c : *cables)
        if (c.to == slot && c.state && c.state->hasTarget.load(std::memory_order_acquire) &&
            c.state->paramId.load(std::memory_order_relaxed) == paramId) {
            c.state->base.store(value, std::memory_order_relaxed);
            taken = true;
        }
    return taken;
}

}  // namespace modulation
