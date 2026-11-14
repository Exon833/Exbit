#include "audio/patch.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>

#include "config.h"

using namespace plugin_audio;

namespace patch {

// --- DelayLine ---

void DelayLine::init(int f, int ch) {
    frames = std::max(0, f);
    channels = std::max(0, ch);
    held_.assign((size_t)frames * channels, 0.0f);
    pos_ = 0;
    quiet_ = frames;
    sounding = false;
}

void DelayLine::process(float* const* signal, int n) {
    if (!frames) return;
    for (int i = 0; i < n; i++) {
        bool loud = false;
        for (int c = 0; c < channels; c++) {
            float x = signal[c][i];
            float& h = held_[(size_t)c * frames + pos_];
            signal[c][i] = h;
            h = x;
            if (x != 0.0f) loud = true;
        }
        quiet_ = loud ? 0 : quiet_ + 1;
        if (++pos_ == frames) pos_ = 0;
    }
    sounding = quiet_ < frames;
}

void DelayLine::clear() {
    std::fill(held_.begin(), held_.end(), 0.0f);
    quiet_ = frames;
    sounding = false;
}

bool isStraight(const std::vector<Cable>& cables) {
    return cables.size() == 1 && cables[0].first == End::in() && cables[0].second == End::out();
}

// --- the plan ---

std::shared_ptr<const Plan> makePlan(const Cables::Ptr& cables) {
    auto plan = std::make_shared<Plan>();
    plan->cables = cables;
    std::map<End, std::vector<End>> sources;
    for (auto& c : *cables) sources[c.second].push_back(c.first);
    std::map<End, int> state;  // 1 open, 2 done
    std::map<End, int> index;  // node -> its place in order
    std::map<const Track*, int> leafIndex;
    auto code = [&](const End& a) -> int {
        if (a.type == End::In) return -1;
        if (a.type == End::TrackSource) {
            auto it = leafIndex.find(a.track);
            if (it != leafIndex.end()) return -2 - it->second;
            int k = (int)plan->leaves.size();
            plan->leaves.push_back(a.track);
            leafIndex[a.track] = k;
            return -2 - k;
        }
        return index.at(a);
    };
    std::function<void(const End&)> visit = [&](const End& node) {
        state[node] = 1;
        bool isOut = node.type == End::Out;
        End ends[2] = {node, End::sidechain(node.slot)};
        std::vector<int> codes[2];
        for (int e = 0; e < (isOut ? 1 : 2); e++) {
            auto it = sources.find(ends[e]);
            if (it == sources.end()) continue;
            std::vector<End> kept;
            for (const End& a : it->second) {
                if (a.type == End::In || a.type == End::TrackSource) {
                    kept.push_back(a);
                } else if (a.type == End::Slot) {
                    auto st = state.find(a);
                    if (st == state.end()) {
                        visit(a);
                        kept.push_back(a);
                    } else if (st->second == 2) {
                        kept.push_back(a);
                    }  // (else: `a` is still open, so a → node would close a loop)
                }
            }
            it->second = kept;
            for (auto& a : kept) codes[e].push_back(code(a));
        }
        state[node] = 2;
        if (isOut) {
            plan->out = std::move(codes[0]);
        } else {
            index[node] = (int)plan->order.size();
            plan->order.push_back({node.slot, std::move(codes[0]), std::move(codes[1])});
        }
    };
    visit(End::out());
    return plan;
}

// --- states ---

static std::atomic<int> blockHint_{4096};
void setBlockHint(int frames) { blockHint_ = std::max(64, frames); }
int blockHint() { return blockHint_; }

void PatchState::reset(const std::shared_ptr<const Plan>& p) {
    planned = p;
    nodes.clear();
    outLines.clear();
    if (!p) return;
    nodes.resize(p->order.size());
    for (size_t i = 0; i < p->order.size(); i++) {
        nodes[i].inLines.resize(p->order[i].in.size());
        nodes[i].sideLines.resize(p->order[i].side.size());
    }
    outLines.resize(p->out.size());
    leafLate.assign(p->leaves.size(), 0);
    leafSound.assign(p->leaves.size() * 2, nullptr);
    int keep = capacity;
    capacity = 0;
    reserve(std::max(keep, blockHint()));
}

void PatchState::reserve(int frames) {
    if (frames <= capacity) return;
    capacity = frames;
    size_t n = (size_t)frames * 2;
    for (auto& w : nodes) {
        w.in.assign(n, 0.0f), w.side.assign(n, 0.0f), w.out.assign(n, 0.0f);
        w.dry.prepare(frames, w.dry.heldBack());
    }
    inSound.assign(n, 0.0f);
    outSound.assign(n, 0.0f);
    temp.assign(n, 0.0f);
    zeros.assign(n, 0.0f);
}

void RackState::reserve(int frames) {
    if ((int)sound.size() < frames * 2) sound.assign((size_t)frames * 2, 0.0f);
    if ((int)scratch.audio.size() < frames * 2) scratch.audio.assign((size_t)frames * 2, 0.0f);
    if (scratch.notes.capacity() < 1024) scratch.notes.reserve(1024);
    if (resets.capacity() < 64) resets.reserve(64);
    if (lines.capacity() < 32) lines.reserve(32);
}

std::shared_ptr<PatchState> stateOf(std::shared_ptr<void>& field) {
    auto now = std::static_pointer_cast<PatchState>(std::atomic_load(&field));
    if (now) return now;
    auto made = std::make_shared<PatchState>();
    std::shared_ptr<void> expected;
    if (std::atomic_compare_exchange_strong(&field, &expected, std::shared_ptr<void>(made))) return made;
    return std::static_pointer_cast<PatchState>(expected);
}

std::shared_ptr<RackState> rackStateOf(std::shared_ptr<void>& field) {
    auto now = std::static_pointer_cast<RackState>(std::atomic_load(&field));
    if (now) return now;
    auto made = std::make_shared<RackState>();
    made->reserve(blockHint());
    std::shared_ptr<void> expected;
    if (std::atomic_compare_exchange_strong(&field, &expected, std::shared_ptr<void>(made))) return made;
    return std::static_pointer_cast<RackState>(expected);
}

std::shared_ptr<const Plan> planFor(const Cables::Ptr& cables, const std::shared_ptr<void>& stateField) {
    auto st = std::static_pointer_cast<PatchState>(std::atomic_load(&stateField));
    if (st) {
        auto p = std::atomic_load(&st->plan);
        if (p && p->cables == cables) return p;
    }
    auto p = makePlan(cables);
    if (st) std::atomic_store(&st->plan, p);
    return p;
}

static void ensureField(std::shared_ptr<void>& field, const Cables::Ptr& cables, bool fresh) {
    auto st = std::static_pointer_cast<PatchState>(std::atomic_load(&field));
    if (!fresh && st) {
        auto p = std::atomic_load(&st->plan);
        if (p && p->cables == cables) return;  // ready (the audio thread's own fields are never read here)
        if (isStraight(*cables)) return;
    }
    auto made = std::make_shared<PatchState>();
    if (!isStraight(*cables)) {
        auto p = makePlan(cables);
        made->plan = p;
        made->reset(p);
    }
    std::atomic_store(&field, std::shared_ptr<void>(made));
}

void ensureState(Track& track) {
    ensureField(track.patchState, track.cables.get(), false);
    if (!std::atomic_load(&track.rackState)) {
        auto r = std::make_shared<RackState>();
        r->reserve(blockHint());
        std::atomic_store(&track.rackState, std::shared_ptr<void>(r));
    }
}

void ensureMasterState(Project& project) { ensureField(project.masterPatchState, project.masterCables.get(), false); }

void ensureStates(Project& project) {
    for (auto& t : project.tracks) ensureState(*t);
    ensureMasterState(project);
}

void freshState(Track& track, bool rack) {
    ensureField(track.patchState, track.cables.get(), true);
    if (!rack) return;
    auto r = std::make_shared<RackState>();
    r->reserve(blockHint());
    std::atomic_store(&track.rackState, std::shared_ptr<void>(r));
}

void freshMasterState(Project& project) { ensureField(project.masterPatchState, project.masterCables.get(), true); }

// --- latency ---

static PluginSlot* resolveSlot(PluginSlot* p, const std::vector<SlotPtr>& effects) {
    for (auto& s : effects)
        if (s.get() == p) return p;
    return nullptr;  // not (or no longer) one of the patch's effects: never touched
}

int planLatency(const Plan& plan, const std::vector<SlotPtr>& effects, int inLate, const TrackList& tracks,
                const Seen* seen) {
    int small[128];
    std::vector<int> big;
    size_t n = plan.leaves.size() + plan.order.size();
    int* buf = n <= 128 ? small : (big.resize(n), big.data());
    int* leafLate = buf;
    int* nodeLate = buf + plan.leaves.size();
    for (size_t j = 0; j < plan.leaves.size(); j++) {
        const Track* t = plan.leaves[j];
        leafLate[j] = inList(t, tracks) ? trackLatency(*t, tracks, seen) : 0;
    }
    auto lateOf = [&](int code) { return code == -1 ? inLate : code >= 0 ? nodeLate[code] : leafLate[-2 - code]; };
    for (size_t i = 0; i < plan.order.size(); i++) {
        const Plan::Node& node = plan.order[i];
        int came = 0;
        for (int c : node.in) came = std::max(came, lateOf(c));
        for (int c : node.side) came = std::max(came, lateOf(c));
        PluginSlot* slot = resolveSlot(node.slot, effects);
        nodeLate[i] = came + (active(slot) ? latency(*slot) : 0);
    }
    int late = 0;
    for (int c : plan.out) late = std::max(late, lateOf(c));
    return late;
}

int patchLatency(const Cables::Ptr& cables, const std::vector<SlotPtr>& effects, int inLate, const TrackList& tracks,
                 const Seen* seen, const std::shared_ptr<void>& stateField) {
    if (isStraight(*cables)) return inLate;
    auto plan = planFor(cables, stateField);
    return planLatency(*plan, effects, inLate, tracks, seen);
}

// --- running a patch ---

namespace {
// what comes into an end (an effect's input, a Sidechain, OUT): its cables added up into dst
// (planar 2 x cap), each held back to be `latest` frames late
template <class SoundOf, class LateOf>
void gather(const std::vector<int>& wired, std::vector<DelayLine>& lines, float* dst, int cap, int frames, int latest,
            SoundOf soundOf, LateOf lateOf, float* temp) {
    float* d[2] = {dst, dst + cap};
    if (wired.empty()) {
        std::memset(d[0], 0, sizeof(float) * frames);
        std::memset(d[1], 0, sizeof(float) * frames);
        return;
    }
    for (size_t j = 0; j < wired.size(); j++) {
        const float* const* y = soundOf(wired[j]);
        const float* src[2] = {y[0], y[1]};
        int late = lateOf(wired[j]);
        if (latest != late) {
            DelayLine& line = lines[j];
            int want = std::max(0, latest - late);
            if (line.frames != want || line.channels != 2) line.init(want, 2);  // (rare: latency changed)
            float* t[2] = {temp, temp + cap};
            std::memcpy(t[0], src[0], sizeof(float) * frames);
            std::memcpy(t[1], src[1], sizeof(float) * frames);
            line.process(t, frames);
            src[0] = t[0], src[1] = t[1];
        }
        for (int c = 0; c < 2; c++) {
            if (j == 0)
                std::memcpy(d[c], src[c], sizeof(float) * frames);
            else
                for (int i = 0; i < frames; i++) d[c][i] += src[c][i];
        }
    }
}
}  // namespace

int runPatch(const Cables::Ptr& cables, const std::vector<SlotPtr>& effects, float* const signal[2], int channels,
             int frames, int sr, PatchState& st, const Feed* feeds, size_t feedCount, int inLate,
             const Transport& transport) {
    if (isStraight(*cables)) return inLate;
    std::shared_ptr<const Plan> plan = std::atomic_load(&st.plan);
    if (!plan || plan->cables != cables) {  // wired differently: plan afresh
        plan = makePlan(cables);
        std::atomic_store(&st.plan, plan);
    }
    if (!st.planned || st.planned->cables != cables)
        st.reset(plan);
    else
        st.planned = plan;  // (the same cables: the same plan)
    st.reserve(frames);
    const int cap = st.capacity;
    // IN: the signal as stereo
    float* inS[2] = {st.inSound.data(), st.inSound.data() + cap};
    std::memcpy(inS[0], signal[0], sizeof(float) * frames);
    std::memcpy(inS[1], channels == 2 ? signal[1] : signal[0], sizeof(float) * frames);
    for (size_t j = 0; j < plan->leaves.size(); j++) {
        const Feed* found = nullptr;
        for (size_t k = 0; k < feedCount; k++)
            if (feeds[k].track == plan->leaves[j]) found = &feeds[k];
        st.leafSound[j * 2] = found ? found->ch[0] : st.zeros.data();
        st.leafSound[j * 2 + 1] = found ? found->ch[1] : st.zeros.data();
        st.leafLate[j] = found ? found->late : 0;
    }
    const float* inSound[2] = {inS[0], inS[1]};
    auto soundOf = [&](int code) -> const float* const* {
        if (code == -1) return inSound;
        if (code >= 0) return st.nodes[code].sound;
        return &st.leafSound[(size_t)(-2 - code) * 2];
    };
    auto lateOf = [&](int code) {
        return code == -1 ? inLate : code >= 0 ? st.nodes[code].late : st.leafLate[(size_t)(-2 - code)];
    };
    for (size_t i = 0; i < plan->order.size(); i++) {
        const Plan::Node& node = plan->order[i];
        PatchState::Work& w = st.nodes[i];
        int came = 0;
        for (int c : node.in) came = std::max(came, lateOf(c));
        for (int c : node.side) came = std::max(came, lateOf(c));
        gather(node.in, w.inLines, w.in.data(), cap, frames, came, soundOf, lateOf, st.temp.data());
        w.late = came;
        w.sound[0] = w.in.data(), w.sound[1] = w.in.data() + cap;
        PluginSlot* slot = resolveSlot(node.slot, effects);
        if (active(slot)) {
            auto inst = aload(slot->instance);
            const float* x[2] = {w.in.data(), w.in.data() + cap};
            const float* sideP[2] = {w.side.data(), w.side.data() + cap};
            bool side = false;
            if (!node.side.empty() && inst && inst->hasSidechain()) {
                gather(node.side, w.sideLines, w.side.data(), cap, frames, came, soundOf, lateOf, st.temp.data());
                side = true;
            }
            float* y[2] = {w.out.data(), w.out.data() + cap};
            if (processEffect(*slot, x, side ? sideP : nullptr, y, frames, sr, transport)) {
                w.sound[0] = y[0], w.sound[1] = y[1];
                int effectLate = latency(*slot);
                w.late = came + effectLate;
                float mix = std::min(1.0f, std::max(0.0f, (float)slot->mix));
                if (effects::DryWet::active(mix)) {  // (its Mix: the input, in time with the output, blended back in)
                    w.dry.keep(x, frames);
                    w.dry.blend(y, frames, effectLate, mix);
                }
            }
        }
    }
    int latest = inLate;
    if (!plan->out.empty()) {
        latest = lateOf(plan->out[0]);
        for (int c : plan->out) latest = std::max(latest, lateOf(c));
    }
    gather(plan->out, st.outLines, st.outSound.data(), cap, frames, latest, soundOf, lateOf, st.temp.data());
    for (int c = 0; c < channels; c++) std::memcpy(signal[c], st.outSound.data() + (size_t)c * cap, sizeof(float) * frames);
    return latest;
}

// --- the instrument patch panel ---

void playInstruments(Track& track, const MidiMsg* messages, size_t count, float* const out[2], int channels,
                     int frames, int sr, RackState& st, const Transport& transport) {
    st.reserve(frames);
    if (!track.usesRack) {
        if (track.kind != Kind::Midi || track.needsInstrument) return;
        auto inst = aload(track.instrument);
        if (usable(inst)) playInstrument(*inst, messages, count, out, channels, frames, sr, st.scratch, transport);
        return;
    }
    if (track.kind != Kind::Midi || track.needsInstrument) return;
    auto rack = track.rack.get();
    auto rc = track.rackCables.get();
    st.resets.clear();
    for (size_t i = 0; i < count; i++)
        if (messages[i].type == MidiMsg::AllOff) st.resets.push_back(messages[i]);
    for (auto& slot : *rack) {
        if (usable(slot) && (!isHeard(track, slot.get(), *rc) || !isFed(slot.get(), *rc))) {
            auto inst = aload(slot->instance);
            if (inst && inst->holding()) inst->notesOff();
        }
    }
    int latest = 0;
    for (auto& slot : *rack)
        if (isHeard(track, slot.get(), *rc)) latest = std::max(latest, latency(*slot));
    const int cap = (int)st.sound.size() / 2;
    float* sound[2] = {st.sound.data(), st.sound.data() + cap};
    for (auto& slot : *rack) {
        if (!isHeard(track, slot.get(), *rc)) continue;
        std::memset(sound[0], 0, sizeof(float) * frames);
        std::memset(sound[1], 0, sizeof(float) * frames);
        if (isFed(slot.get(), *rc))
            playInstrument(*slot, messages, count, sound, channels, frames, sr, st.scratch, transport);
        else
            playInstrument(*slot, st.resets.data(), st.resets.size(), sound, channels, frames, sr, st.scratch, transport);
        int wanted = latest - latency(*slot);
        DelayLine* line = nullptr;
        for (auto& l : st.lines)
            if (l.first == slot.get()) line = &l.second;
        if (!line) {
            st.lines.emplace_back(slot.get(), DelayLine());
            line = &st.lines.back().second;
            line->init(wanted, channels);
        } else if (line->frames != wanted || line->channels != channels) {
            line->init(wanted, channels);
        }
        line->process(sound, frames);
        double gain = slot->volumeDb != 0 ? dsp::dbToGain(slot->volumeDb, config::MIN_DB) : 1.0;
        for (int c = 0; c < channels; c++)
            for (int i = 0; i < frames; i++) out[c][i] += (float)(sound[c][i] * gain);
    }
    for (size_t i = 0; i < st.lines.size();) {
        bool heard = false;
        for (auto& slot : *rack)
            if (slot.get() == st.lines[i].first && isHeard(track, slot.get(), *rc)) heard = true;
        if (!heard)
            st.lines.erase(st.lines.begin() + i);
        else
            i++;
    }
}

}  // namespace patch
