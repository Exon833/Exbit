// Duplicating and moving tracks (track_ops.h).
#include "model/track_ops.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "audio/modulation.h"
#include "audio/patch.h"
#include "core/util.h"
#include "model/routing.h"

namespace track_ops {

SlotPtr freshCopy(const SlotPtr& slot) {
    if (!slot) return nullptr;
    auto s = std::make_shared<PluginSlot>(*slot);
    s->state = slot->currentState();
    s->lastRead.clear();
    s->instance = nullptr;  // (not shared with anyone yet)
    s->failed = false;
    s->error.clear();
    return s;
}

std::vector<SlotPtr> slotsOf(const Track& t) {
    std::vector<SlotPtr> out;
    if (SlotPtr i = std::atomic_load(&t.instrument)) out.push_back(i);
    for (auto& s : t.rack) out.push_back(s);
    for (auto& s : t.effects) out.push_back(s);
    return out;
}

TrackPtr copyOf(const Track& t) {
    auto c = std::make_shared<Track>(t.name, t.kind);
    c->muted = t.muted;
    c->soloed = t.soloed;
    c->output = t.output;
    c->volumeDb = t.volumeDb;
    c->delayMs = t.delayMs;
    c->pan = t.pan;
    c->sends = t.sends;
    c->height = t.height;
    c->midiEffects = t.midiEffects;
    c->sampler = std::atomic_load(&t.sampler);
    c->padKit = std::atomic_load(&t.padKit);
    c->needsInstrument = t.needsInstrument;
    c->bendRange = t.bendRange;
    c->midiIn = t.midiIn;
    c->midiInChannel = t.midiInChannel;
    c->ext = t.ext;
    c->monitor = t.monitor;  // (not armed: a copy is recorded onto only when armed itself)
    c->inputCh = t.inputCh;
    c->inputStereo = t.inputStereo;
    c->color = t.color;
    c->tone = t.tone;
    c->group = t.group;
    c->into = std::atomic_load(&t.into);  // (where it plays: model/routing.h; a copy can't make a loop)
    c->showAutomation = t.showAutomation;
    c->usesRack = t.usesRack;
    if (auto f = std::atomic_load(&t.frozen)) c->frozen = copyClip(f);
    // the plugins: new slots, and every cable, modulation cable and lane pointing at the copy's own
    std::map<const PluginSlot*, SlotPtr> to{{t.controls.get(), c->controls}};
    auto fresh = [&](const SlotPtr& s) { return to[s.get()] = freshCopy(s); };
    if (SlotPtr i = std::atomic_load(&t.instrument)) c->instrument = fresh(i);
    std::vector<SlotPtr> rack, fx;
    for (auto& s : t.rack) rack.push_back(fresh(s));
    for (auto& s : t.effects) fx.push_back(fresh(s));
    c->rack = rack;
    c->effects = fx;
    auto end = [&](End e) {
        if ((e.type == End::Slot || e.type == End::Sidechain) && to.count(e.slot)) e.slot = to[e.slot].get();
        if (e.type == End::TrackSource && e.track == &t) e.track = c.get();
        return e;
    };
    auto cables = [&](const Cables& from) {
        std::vector<Cable> out;
        for (auto& k : from) out.emplace_back(end(k.first), end(k.second));
        return out;
    };
    c->cables = cables(t.cables);
    c->rackCables = cables(t.rackCables);
    std::vector<ModCable> mods;
    for (auto& m : t.modCables) {
        if (!to.count(m.from) || !to.count(m.to)) continue;
        ModCable k = m;
        k.from = to[m.from].get();
        k.to = to[m.to].get();
        k.state = std::make_shared<ModState>();  // (its own working values; the knob's base as the original's)
        k.state->base = m.state->base.load();
        mods.push_back(k);
    }
    c->modCables = mods;
    std::vector<AutomationPtr> lanes;
    for (auto& a : t.automation) {
        if (!a->slot || !to.count(a->slot.get())) continue;
        auto l = std::make_shared<Automation>();
        l->slot = to[a->slot.get()];
        l->param = a->param;
        l->points = a->points;
        l->height = a->height;
        lanes.push_back(l);
    }
    c->automation = lanes;
    return c;
}

namespace {
bool digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char ch) { return std::isdigit((unsigned char)ch); });
}
}  // namespace

std::string copyName(Project& p, const Track& t) {
    // a new track's own name ("Audio 3", "MIDI 12", "Return B"): the next one, as Project::addTrack names them
    std::string prefix = t.kind == Kind::Midi ? "MIDI " : t.kind == Kind::Return ? "Return " : "Audio ";
    std::string rest = t.name.rfind(prefix, 0) == 0 ? t.name.substr(prefix.size()) : "";
    bool letter = t.kind == Kind::Return && rest.size() == 1 && rest[0] >= 'A' && rest[0] <= 'Z';
    if (digits(rest) || letter) {
        int n = ++p.tracksMade[kindName(t.kind)];
        if (t.kind == Kind::Return && n <= 26) return prefix + char('A' + n - 1);
        return prefix + std::to_string(n);
    }
    // anything else: "<name without its number> N", the first N from 2 no track has
    std::string base = t.name;
    size_t sp = base.find_last_of(' ');
    if (sp != std::string::npos && sp > 0 && digits(base.substr(sp + 1))) base = base.substr(0, sp);
    std::set<std::string> taken;
    for (auto& x : p.tracks) taken.insert(x->name);
    int n = 2;
    while (taken.count(base + " " + std::to_string(n))) n++;
    return base + " " + std::to_string(n);
}

void add(Project& p, const TrackPtr& original, const TrackPtr& copy, bool withClips) {
    copy->name = copyName(p, *original);
    patch::ensureState(*copy);
    modulation::resolve(*copy);  // (its modulation cables find their knobs: its plugins are loaded by now)
    std::vector<TrackPtr> v = p.tracks.copy();
    int at = p.indexOf(original);
    v.insert(at >= 0 ? v.begin() + at + 1 : v.end(), copy);
    p.tracks = v;
    p.gatherGroups();
    size_t clips = 0;
    if (withClips) {
        std::vector<ClipPtr> all = p.clips.copy();
        for (auto& c : p.clipsOn(original)) {
            ClipPtr k = copyClip(c);  // (a linked MIDI clip's copy keeps its link)
            std::atomic_store(&k->track, copy);
            all.push_back(k);
            clips++;
        }
        if (clips) p.clips = all;
    }
    util::debugf("track_ops: %s duplicated as %s (%zu plugins, %zu clips, %zu automation lanes)\n",
                 original->name.c_str(), copy->name.c_str(), slotsOf(*copy).size(), clips, copy->automation.size());
}

TrackPtr duplicate(Project& p, const TrackPtr& t, bool withClips,
                   const std::function<void(const std::vector<SlotPtr>&)>& load) {
    TrackPtr c = copyOf(*t);
    if (load) load(slotsOf(*c));
    add(p, t, c, withClips);
    return c;
}

// --- moving ---

namespace {

// one track one lane on (see MOVE); false if it can't (the end, the returns' border, a neighbour that can't move)
bool stepOne(std::vector<TrackPtr>& v, const TrackPtr& t, int d, const std::set<Track*>& stuck) {
    int n = (int)v.size();
    int i = (int)(std::find(v.begin(), v.end(), t) - v.begin()), j = i + d;
    if (i >= n) return false;
    bool edge = j < 0 || j >= n;
    // (nested groups: is u inside g, at any depth; the group of u's right inside g, or its outermost: g null)
    auto within = [](const Track& u, const Group* g) {
        int depth = 0;
        for (Group* x = u.group.get(); x && depth < 1000; x = std::atomic_load(&x->parent).get(), depth++)
            if (x == g) return true;
        return !g;
    };
    auto childOf = [](const Track& u, const Group* g) {
        int depth = 0;
        for (GroupPtr x = u.group; x && depth < 1000; x = std::atomic_load(&x->parent), depth++)
            if (std::atomic_load(&x->parent).get() == g) return x;
        return GroupPtr();
    };
    if (t->group && (edge || !within(*v[(size_t)j], t->group.get()))) {
        // at its group's edge: out of it, into the group around it (it stays where it is: above or below the group)
        t->group = std::atomic_load(&t->group->parent);
        return true;
    }
    if (edge) return false;
    const TrackPtr nb = v[(size_t)j];
    if ((t->kind == Kind::Return) != (nb->kind == Kind::Return) || stuck.count(nb.get())) return false;
    if (GroupPtr inner = nb->group != t->group ? childOf(*nb, t->group.get()) : nullptr) {
        if (!inner->folded) {  // into the group, at that end
            t->group = inner;
            return true;
        }
        int k = j;  // over the whole folded group
        while (k + d >= 0 && k + d < n && within(*v[(size_t)(k + d)], inner.get())) k += d;
        v.erase(v.begin() + i);
        v.insert(v.begin() + k, t);
        return true;
    }
    std::swap(v[(size_t)i], v[(size_t)j]);
    return true;
}

}  // namespace

bool step(Project& p, const std::vector<TrackPtr>& tracks, int dir) {
    if (dir == 0 || tracks.empty()) return false;
    int d = dir < 0 ? -1 : 1;
    std::vector<TrackPtr> v = p.tracks.copy();
    std::vector<TrackPtr> order;  // the first in the way it moves first
    for (auto& t : v)
        if (std::find(tracks.begin(), tracks.end(), t) != tracks.end()) order.push_back(t);
    if (d > 0) std::reverse(order.begin(), order.end());
    std::set<Track*> stuck;
    int moved = 0;
    for (auto& t : order) {
        if (stepOne(v, t, d, stuck)) moved++;
        else stuck.insert(t.get());
    }
    if (!moved) return false;
    p.tracks = v;
    p.gatherGroups();
    routing::fixLoops(p);  // (a track moved into a group whose bus plays into it)
    util::debugf("track_ops: %d of %zu track(s) moved %s\n", moved, order.size(), d < 0 ? "up" : "down");
    return true;
}

bool moveTo(Project& p, const std::vector<TrackPtr>& tracks, const TrackPtr& target, bool below) {
    if (!target || tracks.empty() || std::find(tracks.begin(), tracks.end(), target) != tracks.end()) return false;
    std::vector<TrackPtr> v, moving;
    for (auto& t : p.tracks) {
        if (std::find(tracks.begin(), tracks.end(), t) != tracks.end()) moving.push_back(t);
        else v.push_back(t);
    }
    auto at = std::find(v.begin(), v.end(), target);
    if (moving.empty() || at == v.end()) return false;
    std::vector<TrackPtr> before = p.tracks.copy();
    std::vector<GroupPtr> groups;
    for (auto& t : moving) groups.push_back(t->group);
    v.insert(at + (below ? 1 : 0), moving.begin(), moving.end());
    for (auto& t : moving) t->group = t->kind == Kind::Return ? nullptr : target->group;
    p.tracks = v;
    p.gatherGroups();
    routing::fixLoops(p);
    bool same = *p.tracks == before;
    for (size_t i = 0; i < moving.size(); i++) same = same && moving[i]->group == groups[i];
    if (!same)
        util::debugf("track_ops: %zu track(s) moved %s %s\n", moving.size(), below ? "below" : "above",
                     target->name.c_str());
    return !same;
}

std::vector<Cable> wiredAtEnd(const std::vector<Cable>& cables, PluginSlot* slot) {
    std::vector<Cable> out;
    for (auto& c : cables) out.emplace_back(c.first, c.second.type == End::Out ? End::of(slot) : c.second);
    out.emplace_back(End::of(slot), End::out());
    return out;
}

}  // namespace track_ops
