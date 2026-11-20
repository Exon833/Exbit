#include "model/routing.h"

#include <set>

#include "core/util.h"

namespace routing {

namespace {
constexpr int DEEPEST = 4096;  // (a guard: targets and parents never loop, but a damaged state must not hang)

Group* parentOf(const Group* g) { return std::atomic_load(&g->parent).get(); }

Track* busAbove(Group* g) {
    for (int depth = 0; g && depth < DEEPEST; g = parentOf(g), depth++)
        if (busOn(*g)) return g->bus.get();
    return nullptr;
}
}  // namespace

Track* target(const Track& t) {
    if (t.kind == Kind::Return) return nullptr;
    if (TrackPtr to = std::atomic_load(&t.into))
        if (to.get() != &t && to->kind != Kind::Return && !(to->busOf && !busOn(*to->busOf))) return to.get();
    return busAbove(t.busOf ? parentOf(t.busOf) : t.group.get());
}

bool reaches(const Track& from, const Track* to) {
    const Track* t = &from;
    for (int depth = 0; t && depth < DEEPEST; t = target(*t), depth++)
        if (t == to) return true;
    return false;
}

bool canRoute(const Track& t, const Track* to) {
    if (!to) return true;
    if (t.kind == Kind::Return || to->kind == Kind::Return || to == &t) return false;
    return !reaches(*to, &t);  // (its sound would come back to it)
}

bool setInto(const TrackPtr& t, const TrackPtr& to) {
    if (!t || !canRoute(*t, to.get())) {
        util::debugf("routing: %s into %s refused (a loop)", t ? t->name.c_str() : "?", to ? to->name.c_str() : "master");
        return false;
    }
    std::atomic_store(&t->into, to);
    util::debugf("routing: %s plays into %s", t->name.c_str(), to ? to->name.c_str() : "its default (group bus or master)");
    return true;
}

bool canNest(const GroupPtr& g, const GroupPtr& parent) {
    int depth = 0;
    for (Group* p = parent.get(); p && depth < DEEPEST; p = parentOf(p), depth++)
        if (p == g.get()) return false;
    return true;
}

bool setParent(Project& p, const GroupPtr& g, const GroupPtr& parent) {
    if (!g || !canNest(g, parent)) {
        util::debugf("routing: group %s into %s refused (a loop)", g ? g->name.c_str() : "?",
                     parent ? parent->name.c_str() : "the top");
        return false;
    }
    std::atomic_store(&g->parent, parent);
    util::debugf("routing: group %s now in %s", g->name.c_str(), parent ? parent->name.c_str() : "no group");
    fixLoops(p);
    p.gatherGroups();
    return true;
}

int fixLoops(Project& p) {
    // group parents first (a file could say anything): a loop of parents is cut at the group met again
    for (auto& t : p.tracks) {
        std::vector<Group*> seen;
        for (Group* g = t->group.get(); g;) {
            GroupPtr up = std::atomic_load(&g->parent);
            for (Group* s : seen)
                if (s == up.get()) {
                    util::debugf("routing: group %s was inside itself: taken out", g->name.c_str());
                    std::atomic_store(&g->parent, GroupPtr());
                    up = nullptr;
                    break;
                }
            seen.push_back(g);
            g = up.get();
        }
    }
    int cleared = 0;
    std::vector<TrackPtr> all = p.allTracks();
    for (bool again = true; again;) {
        again = false;
        for (auto& t : all) {
            // walk its targets: coming back to a track already met is a loop
            std::vector<Track*> path;
            Track* at = t.get();
            while (at && path.size() <= all.size() + 1) {
                bool met = false;
                for (Track* x : path) met = met || x == at;
                if (met) {  // the loop: from `at` round to it again; its first track routed by hand lets go
                    std::vector<Track*> loop;
                    for (Track* y = at; y && loop.size() <= all.size(); y = target(*y)) {
                        loop.push_back(y);
                        if (target(*y) == at) break;
                    }
                    for (Track* x : loop)
                        if (std::atomic_load(&x->into)) {
                            util::debugf("routing: %s played into a loop: back to its default", x->name.c_str());
                            std::atomic_store(&x->into, TrackPtr());
                            cleared++;
                            again = true;
                            break;
                        }
                    break;
                }
                path.push_back(at);
                at = target(*at);
            }
            if (again) break;
        }
    }
    return cleared;
}

bool isPlain(const Track& bus) {
    if (!bus.busOf || bus.muted || bus.soloed || bus.volumeDb != 0 || bus.pan != 0 || bus.delayMs != 0) return false;
    auto cables = bus.cables.get();
    if (cables->size() != 1 || (*cables)[0].first.type != End::In || (*cables)[0].second.type != End::Out) return false;
    if (!bus.sends.get()->empty()) return false;
    auto lanes = bus.automation.get();
    for (auto& a : *lanes)
        if (a->slot == bus.controls && !a->points.get()->empty()) return false;
    return true;
}

std::string targetName(const Track& t) {
    Track* to = target(t);
    if (!to) return "Master";
    return to->busOf ? to->busOf->name : to->name;
}

std::vector<Group*> groupsOf(const Track& t) {
    std::vector<Group*> out;
    Group* g = t.busOf ? parentOf(t.busOf) : t.group.get();
    for (int depth = 0; g && depth < DEEPEST; g = parentOf(g), depth++) out.push_back(g);
    return out;
}

std::vector<TrackPtr> inside(const Project& p, const Group* g) {
    std::vector<TrackPtr> out;
    for (auto& t : p.tracks) {
        int depth = 0;
        for (Group* x = t->group.get(); x && depth < DEEPEST; x = parentOf(x), depth++)
            if (x == g) {
                out.push_back(t);
                break;
            }
    }
    return out;
}

}  // namespace routing
