// The built-in synth's patch (audio/synth_patch.h), at its worst: 128 voices of unison 8 with every
// modulation at once, aliasing of a high saw, the filter at full resonance swept end to end, presets
// switched while notes are held, every shipped preset, old projects bit for bit, save, load and undo.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <set>
#include <thread>

#include "audio/fft.h"
#include "audio/live.h"
#include "audio/synth_patch.h"
#include "audio/workers.h"
#include "check.h"
#include "core/util.h"
#include "model/history.h"
#include "model/project_io.h"
#include "paths.h"

using namespace subsynth;

namespace {
constexpr int SR = 48000;
const std::string kDir = "build/tests/synth-scratch";

struct Stats {
    bool finite = true;
    double peak = 0, rms = 0;
};
Stats stats(const std::vector<float>& s) {
    Stats st;
    double sum = 0;
    for (float v : s) {
        if (!std::isfinite(v)) st.finite = false;
        st.peak = std::max(st.peak, (double)std::fabs(v));
        sum += (double)v * v;
    }
    st.rms = s.empty() ? 0 : std::sqrt(sum / s.size());
    return st;
}

Note note(double start, double end, double pitch, double velocity = 100) {
    Note n;
    n.start = start, n.end = end, n.pitch = pitch, n.velocity = velocity;
    return n;
}

// every source onto every target it can move, at depths from -1 to 1
Patch everything() {
    Patch p;
    p.v[O1_WAVE] = SAW, p.v[O2_WAVE] = PULSE, p.v[O2_LEVEL] = 1, p.v[NOISE] = 1;
    p.v[UNISON] = MAX_UNISON, p.v[SPREAD] = 100, p.v[RESO] = 1, p.v[DRIVE] = 1, p.v[KEYTRACK] = 1, p.v[F_ENV] = 1;
    p.v[DRIFT] = 1, p.v[OUTPUT] = 12, p.v[LFO_SHAPE] = 4, p.v[LFO_RATE] = 30, p.v[GLIDE] = 0.2;
    int k = 0;
    for (int s = 0; s < SOURCES; s++)
        for (int t = 0; t < PARAMS; t++)
            if (info(t).mod) p.setMod(s, t, ((k++ % 7) - 3) / 3.0);
    return p;
}

void testParams() {
    Patch init;
    CHECK(init == fromJson(toJson(init)));
    Patch p = everything();
    p.name = "All", p.category = "FX";
    Patch back = fromJson(Json::parse(toJson(p).dump(2)));
    CHECK(back == p);
    CHECK(p.mods.size() > 40);
    for (int i = 0; i < PARAMS; i++) {
        CHECK_NEAR(fromNorm(i, toNorm(i, init[i])), init[i], 1e-9 * std::max(1.0, std::fabs(init[i])));
        CHECK(!valueText(i, init[i]).empty());
    }
    // a broken file: values out of range clamped, unknown keys and NaN left out
    Json j = Json::parse(R"({"format":"exbit-synth","params":{"cutoff":1e9,"unison":99,"noise":"x","nope":3},
                             "mods":[{"source":"lfo","target":"cutoff","depth":7},{"source":"x","target":"cutoff"}]})");
    Patch b = fromJson(j);
    CHECK(b[CUTOFF] == 20000 && b[UNISON] == MAX_UNISON && b[NOISE] == 0);
    CHECK(b.mods.size() == 1 && b.mods[0].depth == 1);
    b.setMod(LFO, CUTOFF, 0);
    CHECK(b.mods.empty());
    b.setMod(LFO, O1_WAVE, 0.5);  // (not a modulation target)
    CHECK(b.mods.empty());
}

// 128 notes at once, unison 8, both oscillators and noise (2,048 oscillators), every modulation
void testWorstCase() {
    Patch p = everything();
    std::vector<Note> notes;
    for (int i = 0; i < 128; i++) notes.push_back(note(0.001 * (i % 8), 2.0, i, 1 + i % 127));
    double t0 = util::wallClock();
    AudioPtr a = render(p, notes, SR, 2.0);
    double took = util::wallClock() - t0;
    Stats st = stats(a->samples);
    double seconds = (double)a->frames() / SR;
    printf("worst case: 128 voices x unison 8 x 2 oscillators + noise, %zu modulations: %.2f s for %.2f s of audio "
           "(real-time factor %.3f, %d cores), peak %.2f\n",
           p.mods.size(), took, seconds, took / seconds, workers::availableCores(), st.peak);
    CHECK(st.finite);
    CHECK(st.peak > 0.01 && st.peak < 200);
    // the same, bit for bit, every time (and however many threads)
    workers::setChosenCores(1);
    t0 = util::wallClock();
    AudioPtr b = render(p, notes, SR, 2.0);
    printf("  on one core: real-time factor %.3f\n", (util::wallClock() - t0) / seconds);
    workers::setChosenCores(0);
    CHECK(b->samples == a->samples);
    // mono: one voice, every glide at once
    p.v[MONO] = 1;
    AudioPtr m = render(p, notes, SR, 2.0);
    CHECK(stats(m->samples).finite && stats(m->samples).peak < 200);
}

// a high saw (3.66 kHz, its harmonics on whole bins): the energy folded back below Nyquist, against a naive saw
void testAliasing() {
    const int N = 65536, bin = 5000;
    double f = (double)bin * SR / N;
    Patch p;
    p.v[O1_WAVE] = SAW, p.v[CUTOFF] = 20000, p.v[E1_A] = 0.001, p.v[E1_S] = 1;
    AudioPtr a = render(p, {note(0, 2.0, 69 + 12 * std::log2(f / 440))}, SR, 2.0);
    auto aliasDb = [&](const std::function<double(int)>& x) {
        std::vector<fft::cd> buf((size_t)N);
        for (int i = 0; i < N; i++) buf[(size_t)i] = x(i) * (0.5 - 0.5 * std::cos(2 * M_PI * i / N));
        fft::Plan(N).forward(buf.data());
        double harm = 0, rest = 0;
        for (int k = 8; k < N / 2; k++) {
            double e = std::norm(buf[(size_t)k]);
            int nearest = (int)std::lround((double)k / bin) * bin;
            (nearest > 0 && std::abs(k - nearest) <= 3 ? harm : rest) += e;
        }
        return 10 * std::log10(rest / harm);
    };
    double blep = aliasDb([&](int i) { return a->samples[(size_t)(SR / 4 + i) * 2]; });
    double naive = aliasDb([&](int i) { double ph = std::fmod(f * (SR / 4 + i) / SR, 1.0); return 2 * ph - 1; });
    printf("aliasing of a %.0f Hz saw: PolyBLEP %.1f dB, naive %.1f dB (folded energy against the harmonics)\n", f, blep,
           naive);
    CHECK(blep < -30);
    CHECK(blep < naive - 15);
    // the pulse and the triangle too
    for (int wave : {TRIANGLE, PULSE}) {
        p.v[O1_WAVE] = wave;
        AudioPtr w = render(p, {note(0, 2.0, 69 + 12 * std::log2(f / 440))}, SR, 2.0);
        double db = aliasDb([&](int i) { return w->samples[(size_t)(SR / 4 + i) * 2]; });
        printf("  %s: %.1f dB\n", wave == TRIANGLE ? "triangle" : "pulse", db);
        CHECK(db < -30);
    }
}

// full resonance and drive, the cutoff swept across its whole range by a fast LFO and Env 2, every filter type
void testFilterStability() {
    for (int type : {LOWPASS, HIGHPASS, BANDPASS}) {
        Patch p;
        p.v[F_TYPE] = type, p.v[RESO] = 1, p.v[DRIVE] = 1, p.v[NOISE] = 1, p.v[O1_WAVE] = PULSE;
        p.v[CUTOFF] = 20, p.v[F_ENV] = 1, p.v[LFO_RATE] = 30, p.v[LFO_SHAPE] = 3, p.v[E1_R] = 0.05;
        p.setMod(LFO, CUTOFF, 1);
        p.setMod(LFO, RESO, -1);
        p.setMod(ENV2, CUTOFF, 1);
        std::vector<Note> notes;
        for (int i = 0; i < 16; i++) notes.push_back(note(i * 0.1, i * 0.1 + 0.5, 20 + i * 7));
        AudioPtr a = render(p, notes, SR, 2.0);
        Stats st = stats(a->samples);
        CHECK(st.finite);
        CHECK(st.peak < 50);
        // after the last release: silence (nothing left ringing)
        double tail = 0;
        for (size_t i = (size_t)(2.2 * SR) * 2; i < a->samples.size(); i++) tail = std::max(tail, (double)std::fabs(a->samples[i]));
        CHECK(tail == 0);
    }
}

// old projects: a track without a patch plays the classic voice, bit for bit, and is saved as it always was
void testClassicAndProjects() {
    Project project;
    TrackPtr t = project.addTrack(Kind::Midi);
    t->needsInstrument = false;
    std::vector<Note> notes = {note(0, 0.5, 60), note(0.25, 1.0, 67, 80), note(1.0, 1.2, 72, 127)};
    ClipPtr c = makeMidiClip("Notes", notes, 1.5, SR);
    c->track = t;
    project.addClip(c);
    c->prepare(SR);
    AudioPtr classic = synth::render(c->performed(SR), SR, 1.5);
    CHECK(c->data->samples == classic->samples);
    project_io::MasterSettings master;
    std::string error;
    util::makeDirs(kDir);
    std::string path = kDir + "/classic.exbit";
    CHECK(project_io::saveProject(path, project, SR, master, error));
    std::string text;
    util::readFile(path, text);
    CHECK(text.find("\"synth\"") == std::string::npos);
    Project back;
    project_io::MasterSettings m2;
    CHECK(project_io::loadProject(path, back, SR, m2, error));
    CHECK(back.tracks.size() == 1 && !back.tracks[0]->synth);
    CHECK(back.clips.size() == 1 && (*back.clips)[0]->data->samples == classic->samples);

    // with a patch: saved, loaded back the same, rendered as the patch says; undo goes back to the classic voice
    history::History hist(project, SR);
    Patch p = everything();
    p.name = "Saved";
    std::atomic_store(&t->synth, PatchPtr(std::make_shared<const Patch>(p)));
    c->prepare(SR);
    CHECK(c->data->samples != classic->samples);
    CHECK(hist.check(SR));
    CHECK(!hist.labels().empty());
    CHECK(project_io::saveProject(path, project, SR, master, error));
    Project again;
    CHECK(project_io::loadProject(path, again, SR, m2, error));
    CHECK(again.tracks[0]->synth && *again.tracks[0]->synth == p);
    CHECK((*again.clips)[0]->data->samples == c->data->samples);
    CHECK(hist.undo(SR));
    CHECK(!t->synth && c->data->samples == classic->samples);
    CHECK(hist.redo(SR));
    CHECK(t->synth && *t->synth == p);
    // another instrument playing: the patch is kept, not heard
    t->sampler = std::make_shared<SamplerSettings>();
    CHECK(!plays(*t));
    t->sampler = nullptr;
    CHECK(plays(*t));
}

// live notes while the patch is switched again and again: the audio thread playing all along
void testSwitchWhileHeld() {
    Project project;
    TrackPtr t = project.addTrack(Kind::Midi);
    t->needsInstrument = false;
    live::LiveNotes notes;
    std::atomic<bool> stop{false}, bad{false};
    std::atomic<long> buffers{0};
    std::thread audio([&] {
        std::vector<float> l(256), r(256);
        float* const sig[2] = {l.data(), r.data()};
        plugin_audio::Messages msgs;
        msgs.reserve(1024);
        while (!stop) {
            std::fill(l.begin(), l.end(), 0.0f);
            std::fill(r.begin(), r.end(), 0.0f);
            msgs.clear();
            notes.take(256, SR, msgs);
            notes.addVoices(sig, 2, 256, SR);
            for (int i = 0; i < 256; i++)
                if (!std::isfinite(l[i]) || !std::isfinite(r[i]) || std::fabs(l[i]) > 500) bad = true;
            buffers++;
        }
    });
    std::vector<PatchPtr> patches = {std::make_shared<const Patch>(everything()), std::make_shared<const Patch>()};
    for (const Preset& pr : presets()) {
        Patch p;
        std::string error;
        if (load(pr.path, p, error)) patches.push_back(std::make_shared<const Patch>(p));
    }
    for (int k = 0; k < 40; k++) {
        std::atomic_store(&t->synth, patches[(size_t)k % patches.size()]);
        live::NoteRender r = noteRenderer(*t);
        CHECK(r.render && r.channels == 2);
        notes.noteOn(48 + k % 24, 100, SR, true, -1, &r);  // (held)
        if (k % 3 == 2) notes.noteOff(48 + (k - 2) % 24);
    }
    for (int p = 0; p < 128; p++) notes.noteOff(p);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop = true;
    audio.join();
    notes.collect();
    CHECK(!bad);
    CHECK(buffers > 0);
}

void testPresets() {
    std::vector<Preset> all = presets();
    printf("%zu presets shipped\n", all.size());
    CHECK(all.size() >= 30 && all.size() <= 50);
    std::set<std::string> categories;
    for (const Preset& pr : all) {
        Patch p;
        std::string error;
        bool ok = load(pr.path, p, error);
        CHECK(ok);
        if (!ok) {
            fprintf(stderr, "%s: %s\n", pr.path.c_str(), error.c_str());
            continue;
        }
        categories.insert(pr.category);
        std::vector<Note> chord = {note(0, 1.0, 48), note(0, 1.0, 55), note(0.5, 1.5, 60), note(0.5, 1.5, 64)};
        AudioPtr a = render(p, chord, SR, 1.5);
        Stats st = stats(a->samples);
        bool good = st.finite && st.rms > 1e-3 && st.peak < 4;
        printf("  %-6s %-16s rms %.3f  peak %.3f\n", pr.category.c_str(), pr.name.c_str(), st.rms, st.peak);
        if (!good) fprintf(stderr, "%s: finite %d, rms %g, peak %g\n", pr.name.c_str(), st.finite, st.rms, st.peak);
        CHECK(good);
    }
    for (const char* c : CATEGORIES) CHECK(categories.count(c));
    // the user's own: saved, listed after the shipped ones, loaded back the same
    Patch mine = everything();
    std::string path, error;
    CHECK(save(mine, "My sound", "Lead", path, error));
    std::vector<Preset> after = presets();
    CHECK(after.size() == all.size() + 1 && after.back().user && after.back().name == "My sound");
    Patch back;
    CHECK(load(path, back, error));
    mine.name = "My sound", mine.category = "Lead";
    CHECK(back == mine);
    CHECK(!save(mine, "  ", "Lead", path, error));
}

}  // namespace

int main() {
    util::makeDirs(kDir);
    paths::setDataDir(util::absolutePath(kDir + "/data"));
    util::removeFile(kDir + "/data/presets/synth/Lead/My sound.json");
    setShippedFolder(util::absolutePath("data/presets/synth"));
    testParams();
    testWorstCase();
    testAliasing();
    testFilterStability();
    testClassicAndProjects();
    testSwitchWhileHeld();
    testPresets();
    TEST_MAIN_END();
}
