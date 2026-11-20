// Pads on their own tracks (pad_tracks.h).
#include "model/pad_tracks.h"

#include <map>
#include <set>

#include "audio/padkit.h"
#include "core/util.h"

namespace pad_tracks {

namespace {
std::shared_ptr<const std::vector<TrackPtr>> outsOf(const Track& kit) { return std::atomic_load(&kit.padOut); }
}  // namespace

TrackPtr trackOf(const Track& kit, int pad) {
    auto outs = outsOf(kit);
    return outs && pad >= 0 && pad < (int)outs->size() ? (*outs)[(size_t)pad] : nullptr;
}

uint32_t routedMask(const Track& kit) {
    auto outs = outsOf(kit);
    if (!outs || !kit.playsPadKit()) return 0;
    uint32_t mask = 0;
    for (size_t i = 0; i < outs->size() && i < 32; i++)
        if ((*outs)[i]) mask |= 1u << i;
    return mask;
}

TrackPtr kitOf(const Project& p, const Track& t, std::vector<int>* pads) {
    for (auto& kit : p.tracks) {
        auto outs = outsOf(*kit);
        if (!outs || kit.get() == &t) continue;
        std::vector<int> found;
        for (size_t i = 0; i < outs->size(); i++)
            if ((*outs)[i].get() == &t) found.push_back((int)i);
        if (found.empty()) continue;
        if (pads) *pads = found;
        return kit;
    }
    return nullptr;
}

std::string trackName(const Pad& pad, int index) {
    std::string name = padkit::padName(pad), number = "pad " + std::to_string(index + 1);
    return name.empty() ? "Pad " + std::to_string(index + 1) : name + " (" + number + ")";
}

std::string inputName(const Project& p, const Track& t) {
    std::vector<int> pads;
    TrackPtr kit = kitOf(p, t, &pads);
    if (!kit) return "";
    std::string text = kit->name + " pad";
    for (size_t i = 0; i < pads.size(); i++) text += (i ? ", " : " ") + std::to_string(pads[i] + 1);
    return text;
}

void setTrack(const TrackPtr& kit, int pad, const TrackPtr& to) {
    if (!kit || pad < 0 || pad >= PadKit::MAX_PADS || to == kit) return;
    auto outs = outsOf(*kit);
    std::vector<TrackPtr> made = outs ? *outs : std::vector<TrackPtr>();
    made.resize(PadKit::MAX_PADS);
    made[(size_t)pad] = to;
    bool any = false;
    for (auto& t : made) any = any || t;
    std::atomic_store(&kit->padOut, any ? std::make_shared<const std::vector<TrackPtr>>(made)
                                        : std::shared_ptr<const std::vector<TrackPtr>>());
    util::debugf("pad_tracks: %s pad %d plays on %s\n", kit->name.c_str(), pad + 1, to ? to->name.c_str() : "the kit");
}

std::vector<TrackPtr> backToKit(const Project& p, const Track& t) {
    std::vector<TrackPtr> kits;
    std::vector<int> pads;
    for (TrackPtr kit; (kit = kitOf(p, t, &pads));) {
        for (int pad : pads) setTrack(kit, pad, nullptr);
        kits.push_back(kit);
    }
    return kits;
}

GroupPtr kitGroup(Project& p, const TrackPtr& kit) {
    if (kit->group && kit->group->name == kit->name) return kit->group;
    GroupPtr g = p.groupTracks({kit});  // (inside the group the kit was in, if any)
    g->name = kit->name;
    g->bus->name = kit->name;
    util::debugf("pad_tracks: a group for %s's pads\n", kit->name.c_str());
    return g;
}

TrackPtr giveTrack(Project& p, const TrackPtr& kit, int pad) {
    if (!kit || pad < 0 || pad >= PadKit::MAX_PADS) return nullptr;
    if (TrackPtr has = trackOf(*kit, pad)) return has;
    PadKitPtr k = std::atomic_load(&kit->padKit);
    if (!k) return nullptr;
    GroupPtr g = kitGroup(p, kit);
    TrackPtr after = kit;  // (below the kit and its other pad tracks)
    for (auto& t : p.tracks) {
        TrackPtr from = kitOf(p, *t);
        if (from == kit && t->group == g && p.indexOf(t) > p.indexOf(after)) after = t;
    }
    TrackPtr track = p.addTrack(Kind::Audio, g, after);
    track->name = trackName(k->pads[(size_t)pad], pad);
    setTrack(kit, pad, track);
    return track;
}

std::vector<TrackPtr> padsToTracks(Project& p, const TrackPtr& kit) {
    std::vector<TrackPtr> made;
    PadKitPtr k = kit ? std::atomic_load(&kit->padKit) : nullptr;
    if (!k) return made;
    for (int i = 0; i < PadKit::MAX_PADS; i++)
        if (!k->pads[(size_t)i].empty() && !trackOf(*kit, i)) made.push_back(giveTrack(p, kit, i));
    util::debugf("pad_tracks: Pads to tracks: %zu new tracks for %s\n", made.size(), kit->name.c_str());
    return made;
}

int forgetGone(Project& p) {
    std::set<const Track*> present;
    std::map<const Track*, const Track*> claimed;  // (a pad track: the kit playing into it)
    for (auto& t : p.tracks) present.insert(t.get());
    int cleared = 0;
    for (auto& kit : p.tracks) {
        auto outs = outsOf(*kit);
        if (!outs) continue;
        std::vector<TrackPtr> made((size_t)PadKit::MAX_PADS);
        int bad = 0;
        bool any = false;
        for (size_t i = 0; i < outs->size(); i++) {
            const TrackPtr& to = (*outs)[i];
            if (!to) continue;
            auto other = claimed.find(to.get());
            if (i >= made.size() || !present.count(to.get()) || to == kit || to->kind == Kind::Return || to->busOf ||
                (other != claimed.end() && other->second != kit.get())) {
                bad++;
                continue;
            }
            made[i] = to;
            claimed[to.get()] = kit.get();
            any = true;
        }
        if (!bad && outs->size() == made.size()) continue;
        std::atomic_store(&kit->padOut, any ? std::make_shared<const std::vector<TrackPtr>>(made)
                                            : std::shared_ptr<const std::vector<TrackPtr>>());
        cleared += bad;
    }
    if (cleared) util::debugf("pad_tracks: %d pad track reference(s) cleared (gone, or not a track a pad can play on)\n", cleared);
    return cleared;
}

}  // namespace pad_tracks
