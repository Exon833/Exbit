// Commit to notes (midi_commit.h).
#include "ui/midi_commit.h"

#include <algorithm>
#include <set>

#include "audio/midi_effects.h"
#include "audio/workers.h"
#include "core/util.h"
#include "model/track_ops.h"
#include "ui/song_scale.h"

namespace midi_commit {

std::vector<Note> playedNotes(const Clip& c, int sr) {
    std::vector<Note> notes = c.performed(sr);  // (as Clip::prepare: chances and repeats, then the effects)
    TrackPtr t = std::atomic_load(&c.track);
    if (t && clip_hooks::midiEffects && !c.effectsCommitted)
        notes = clip_hooks::midiEffects(*t, notes, (double)(c.start - c.offset) / sr);
    return notes;
}

namespace {
// the clip's notes made what it plays: no loop any more (its notes repeat already), as long as it played
void writeInto(Clip& c, std::vector<Note> notes, int sr) {
    double len = c.loop && c.length >= 0 ? std::max(c.lengthS, (double)(c.offset + c.length) / sr) : c.lengthS;
    c.notes = std::make_shared<const std::vector<Note>>(std::move(notes));
    c.loop = false;
    c.lengthS = len;
}
}  // namespace

Result commit(Project& p, int sr, const TrackPtr& t, std::vector<ClipPtr> clips, bool newTrack, const Loader& load) {
    Result r;
    if (!t || t->kind != Kind::Midi || !p.tracks.contains(t)) {
        r.error = "Choose a MIDI track";
        return r;
    }
    if (std::atomic_load(&t->frozen)) {
        r.error = t->name + " is frozen: unfreeze it first";
        return r;
    }
    std::vector<ClipPtr> all;
    for (auto& c : p.clipsOn(t))
        if (c->kind == Kind::Midi) all.push_back(c);
    std::set<ClipPtr> wanted(clips.begin(), clips.end());
    if (clips.empty()) wanted.insert(all.begin(), all.end());
    std::vector<ClipPtr> todo;  // (in the project's order)
    for (auto& c : all)
        if (wanted.count(c)) todo.push_back(c);
    if (todo.empty()) {
        r.error = clips.empty() ? t->name + " has no MIDI clips" : "Select a MIDI clip of " + t->name + " first";
        return r;
    }
    bool whole = todo.size() == all.size();  // (every clip of the track: its effects can be bypassed)

    // the notes each plays, on every core (the workers read the tempo map: published first)
    p.timing.publish();
    struct Work {
        std::vector<ClipPtr>* todo;
        std::vector<std::vector<Note>> out;
        int sr;
    } work{&todo, std::vector<std::vector<Note>>(todo.size()), sr};
    auto job = [](void* ctx, int i) {
        Work& w = *static_cast<Work*>(ctx);
        w.out[(size_t)i] = playedNotes(*(*w.todo)[(size_t)i], w.sr);
    };
    int threads = todo.size() >= 4 ? workers::workersFor(workers::chosenCores()) : 0;
    {
        workers::Pool pool(threads, false);  // (as a render's: never outranking playback)
        pool.run((int)todo.size(), job, &work);
    }

    if (newTrack) {
        TrackPtr copy = track_ops::copyOf(*t);
        copy->midiEffects = std::vector<MidiEffect>{};  // (its notes hold them now)
        copy->swing = midi_effects::swings(*t) ? 0 : t->swing;
        if (load) {
            std::vector<SlotPtr> slots = track_ops::slotsOf(*copy);
            if (!slots.empty()) load(slots);
        }
        if (!p.tracks.contains(t)) {  // (removed while its plugins loaded)
            r.error = "The track was removed";
            return r;
        }
        track_ops::add(p, t, copy, false);
        copy->name = t->name + " (notes)";
        std::vector<ClipPtr> clipsNow = p.clips.copy();
        for (size_t i = 0; i < todo.size(); i++) {
            ClipPtr k = links::plainCopy(todo[i]);
            k->track = copy;
            k->effectsCommitted = false;
            k->mutedNotes = nullptr;  // (they belonged to the notes as written)
            writeInto(*k, std::move(work.out[i]), sr);
            clipsNow.push_back(k);
            todo[i]->disabled = true;  // (kept, silent: the new one plays it)
        }
        p.clips = clipsNow;  // (in one assignment: the audio thread sees them all at once)
        song_scale::reprepare(p, sr, {copy});
        r.track = copy;
    } else {
        links::makeUnique(p, todo);
        for (size_t i = 0; i < todo.size(); i++) {
            writeInto(*todo[i], std::move(work.out[i]), sr);
            todo[i]->mutedNotes = nullptr;  // (they belonged to the notes as written)
            todo[i]->effectsCommitted = !whole;
        }
        if (whole) {
            std::vector<MidiEffect> effects = t->midiEffects.copy();
            for (auto& e : effects)
                if (!e.bypass) e.bypass = true, r.bypassed++;
            if (r.bypassed) t->midiEffects = effects;  // (replaced whole: undo and the audio thread see whole lists)
            if (midi_effects::swings(*t)) t->swing = 0;  // (written into the notes too)
            for (auto& c : todo) c->effectsCommitted = false;  // (the track itself plays straight now)
        }
        song_scale::reprepare(p, sr, {t});
    }
    r.clips = (int)todo.size();
    util::debugf("midi_commit: %d clip(s) of \"%s\" committed to notes %s; %d effect(s) bypassed\n", r.clips,
                 t->name.c_str(), newTrack ? ("on the new track \"" + r.track->name + "\"").c_str() : "in place",
                 r.bypassed);
    return r;
}

}  // namespace midi_commit
