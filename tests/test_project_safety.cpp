// Project safety, worst cases: a project of 5,000 samples with half of them gone (moved to another folder tree,
// renamed, deleted) found again by one search; Collect and save (copies, never moves; identical files once; same
// names apart) and the project opening with its original sample folder renamed away; archives round trip, a
// damaged archive and zip-slip names refused with nothing left behind; parallel loading the same as serial,
// bit for bit, and faster (the speed-up printed).
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "audio/file_io.h"
#include "audio/sound_file.h"
#include "audio/workers.h"
#include "check.h"
#include "core/util.h"
#include "importers/zip.h"
#include "model/collect.h"
#include "model/project_io.h"
#include "model/relink.h"

using namespace project_io;

static const std::string kDir = util::absolutePath("build/test-tmp/project-safety");
static const int SR = 48000;

static void clearDir(const std::string& dir) {
    if (!util::isDir(dir)) return;
    for (auto& e : util::listDir(dir)) {
        std::string p = util::joinPath(dir, e.name);
        if (e.isDir) {
            clearDir(p);
            rmdir(p.c_str());
        } else {
            util::removeFile(p);
        }
    }
}

// a short sound of its own: `frames` long, its level `level` (each different, so no two are the same audio)
static AudioPtr sound(int64_t frames, float level) {
    auto b = std::make_shared<AudioBuffer>();
    b->samples.resize((size_t)frames * 2);
    for (int64_t i = 0; i < frames; i++) {
        b->samples[(size_t)(2 * i)] = level * (float)std::sin(0.01 * (double)i);
        b->samples[(size_t)(2 * i + 1)] = -level;
    }
    return b;
}

static int missingCount(const Project& p) {
    int n = 0;
    for (auto& c : allAudioHolders(p)) n += c->missing != nullptr;
    return n;
}

// --- 5,000 samples, half of them gone: one search finds all that can be found ---
static void testRelinkMany() {
    std::string dir = kDir + "/many";
    util::makeDirs(dir);
    const int N = 5000;
    Project p;
    TrackPtr t = p.addTrack(Kind::Audio);
    std::vector<ClipPtr> all;
    for (int i = 0; i < N; i++) {  // (lengths repeat every 500: renamed files must be told apart by their sound)
        auto c = makeAudioClip(util::format("s%04d", i), sound(16 + i % 500, 0.1f + (float)i * 1e-4f), SR, SR);
        c->track = t;
        c->start = (int64_t)i * 100;
        all.push_back(c);
    }
    p.clips = all;
    // a sampler and a pad of a Pad Kit too, on a MIDI track
    TrackPtr m = p.addTrack(Kind::Midi);
    auto s = std::make_shared<SamplerSettings>();
    s->sample = makeAudioClip("keys", sound(300, 0.77f), SR, SR);
    m->sampler = s;
    auto kit = std::make_shared<PadKit>();
    kit->pads[3].sound.sample = makeAudioClip("snare", sound(200, 0.66f), SR, SR);
    m->padKit = kit;
    MasterSettings master;
    std::string err, path = dir + "/Many.json";
    double t0 = util::wallClock();
    CHECK(saveProject(path, p, SR, master, err));
    std::string audio = audioFolder(path);
    printf("saved %d samples in %.2f s\n", N + 2, util::wallClock() - t0);
    // half go: a quarter moved to another folder tree (same names), an eighth renamed elsewhere, an eighth deleted
    std::string lib = dir + "/library", moved = lib + "/moved/deep/er", renamed = lib + "/renamed";
    util::makeDirs(moved);
    util::makeDirs(renamed);
    int findable = 0, gone = 0;
    for (int i = 0; i < N; i++) {
        std::string f = util::joinPath(audio, util::format("s%04d.wav", i));
        if (i % 4 == 1) {
            CHECK(rename(f.c_str(), (moved + util::format("/s%04d.wav", i)).c_str()) == 0);
            findable++;
        } else if (i % 8 == 2) {
            CHECK(rename(f.c_str(), (renamed + util::format("/renamed %d.wav", i * 7)).c_str()) == 0);
            findable++;
        } else if (i % 8 == 6) {
            CHECK(util::removeFile(f));
            gone++;
        }
    }
    CHECK(rename((audio + "/keys.wav").c_str(), (moved + "/keys.wav").c_str()) == 0);
    CHECK(rename((audio + "/snare.wav").c_str(), (renamed + "/snare 2.wav").c_str()) == 0);
    findable += 2;
    // it opens: the missing ones silent, marked, where they belong
    Project q;
    MasterSettings m2;
    t0 = util::wallClock();
    CHECK(loadProject(path, q, SR, m2, err));
    printf("opened with %d missing in %.2f s\n", findable + gone, util::wallClock() - t0);
    CHECK(q.clips.size() == (size_t)N && missingCount(q) == findable + gone);
    CHECK(q.clips[1]->missing && q.clips[1]->start == 100 && q.clips[1]->missing->size > 0 &&
          !q.clips[1]->missing->sound.empty());
    CHECK(q.clips[0]->missing == nullptr);
    float peak = 0;
    for (float v : q.clips[1]->data->samples) peak = std::max(peak, std::fabs(v));
    CHECK(peak == 0);  // (silent)
    auto list = relink::missing(q);
    CHECK((int)list.size() == findable + gone);
    bool samplerListed = false;
    for (auto& e : list) samplerListed = samplerListed || (!e.users.empty() && e.users[0].find("sampler") != std::string::npos);
    CHECK(samplerListed);
    // saving keeps naming them where they were (and their sizes and sounds), until found
    CHECK(saveProject(dir + "/Many again.json", q, SR, m2, err));
    Project r;
    CHECK(loadProject(dir + "/Many again.json", r, SR, m2, err));
    CHECK(missingCount(r) == findable + gone && relink::missing(r)[0].path == list[0].path &&
          relink::missing(r)[0].size == list[0].size && relink::missing(r)[0].sound == list[0].sound);
    // one search: every findable one found (by name, then renamed ones by size and sound)
    relink::Found found;
    t0 = util::wallClock();
    int added = relink::search(list, {dir + "/nowhere", lib}, found);
    printf("one search found %d of %zu in %.2f s\n", added, list.size(), util::wallClock() - t0);
    CHECK(added == findable && (int)found.size() == findable);
    for (auto& [was, now] : found)  // (each the right file: its name, or for renamed ones its number)
        if (util::dirName(now) == renamed && util::baseName(was) != "snare.wav") {
            int i = atoi(util::baseName(was).c_str() + 1), j = atoi(util::baseName(now).c_str() + 8);
            CHECK(j == i * 7);
        }
    std::string why;
    CHECK(relink::apply(q, found, SR, why) == findable && why.empty());
    CHECK(missingCount(q) == gone);
    // the found ones play their own audio again: as saved
    for (int i : {1, 2, 5, 4999 - 4999 % 4 + 1}) {
        if (i >= N) continue;
        CHECK(!q.clips[(size_t)i]->missing && q.clips[(size_t)i]->source->samples == all[(size_t)i]->source->samples);
    }
    CHECK(!q.tracks[1]->sampler->sample->missing && q.tracks[1]->sampler->sample->source->samples == s->sample->source->samples);
    CHECK(!q.tracks[1]->padKit->pads[3].sound.sample->missing);
    // and a second search finds nothing more (the deleted ones can't be)
    relink::Found again;
    CHECK(relink::search(relink::missing(q), {lib}, again) == 0);
}

// --- a folder moved whole: found once, the others beside it (Locate…), and Collect and save ---
static void testCollect() {
    std::string dir = kDir + "/collect", src = dir + "/originals";
    util::makeDirs(src + "/a");
    util::makeDirs(src + "/b");
    // originals: two identical files under different names, two different files with the same name
    std::string err;
    auto write = [&](const std::string& f, const AudioPtr& a) {
        CHECK(writeWav(f, a->samples.data(), (size_t)a->frames(), 2, SR, SampleFormat::Pcm16, err));
    };
    AudioPtr same = sound(4000, 0.5f), kickA = sound(3000, 0.4f), kickB = sound(3500, 0.3f);
    write(src + "/a/loop.wav", same);
    write(src + "/b/loop copy.wav", same);
    write(src + "/a/kick.wav", kickA);
    write(src + "/b/kick.wav", kickB);
    std::vector<std::string> originals = {src + "/a/loop.wav", src + "/b/loop copy.wav", src + "/a/kick.wav", src + "/b/kick.wav"};
    std::vector<std::string> bytes;
    for (auto& f : originals) {
        std::string b;
        CHECK(util::readFile(f, b));
        bytes.push_back(b);
    }
    Project p;
    TrackPtr t = p.addTrack(Kind::Audio);
    for (auto& f : originals) {  // (as the timeline imports them)
        AudioPtr a;
        int rate = 0;
        CHECK(file_io::loadAudio(f, a, rate, err));
        auto c = makeAudioClip(util::baseName(f), a, rate, SR);
        c->track = t;
        p.addClip(c);
    }
    std::string path = dir + "/Song.json";
    auto before = collect::inFolder(path, p);
    CHECK(before.empty());
    MasterSettings master;
    int calls = 0;
    CHECK(saveProject(path, p, SR, master, err, [&](int, int) { calls++; }));
    CHECK(calls >= 4);
    collect::Summary s = collect::summary(path, p, before);
    CHECK(s.files == 3 && s.copied == 3 && s.missing == 0 && s.bytes > 0);  // (the identical pair once)
    CHECK(util::fileExists(s.folder + "/kick.wav") && util::fileExists(s.folder + "/kick (2).wav"));
    printf("%s\n", collect::describe(s).c_str());
    // collected again: nothing copied, the files there left as they are
    double mtime = util::fileMtime(s.folder + "/kick.wav");
    before = collect::inFolder(path, p);
    CHECK(before.size() == 3);
    CHECK(saveProject(path, p, SR, master, err));
    s = collect::summary(path, p, before);
    CHECK(s.files == 3 && s.copied == 0 && util::fileMtime(s.folder + "/kick.wav") == mtime);
    // the originals untouched, still there (copied, never moved)
    for (size_t i = 0; i < originals.size(); i++) {
        std::string b;
        CHECK(util::readFile(originals[i], b) && b == bytes[i]);
    }
    // the original folder renamed away: the project opens whole, as it was
    CHECK(rename(src.c_str(), (dir + "/gone").c_str()) == 0);
    Project q;
    CHECK(loadProject(path, q, SR, master, err));
    CHECK(q.clips.size() == 4 && missingCount(q) == 0);
    for (size_t i = 0; i < 4; i++) CHECK(q.clips[i]->source->samples == p.clips[i]->source->samples);
    CHECK(q.clips[0]->source == q.clips[1]->source);  // (one file: decoded once, shared)

    // the project folder moved whole elsewhere: its file names it relative, so it opens; a JSON naming the old
    // place absolutely (as a recovery file does) finds one by Locate…, the rest beside it
    util::makeDirs(dir + "/moved");
    Json data = Json::parse([&] { std::string t2; util::readFile(path, t2); return t2; }());
    Json clips = data["clips"];
    for (auto& c : clips.items()) c.set("file", Json(util::joinPath(dir, c["file"].toString())));
    data.set("clips", clips);
    CHECK(writeJson(dir + "/Absolute.json", data, err));
    CHECK(rename(s.folder.c_str(), (dir + "/moved/Song_audio").c_str()) == 0);
    Project a;
    CHECK(loadProject(dir + "/Absolute.json", a, SR, master, err));
    auto list = relink::missing(a);
    CHECK(list.size() == 3 && missingCount(a) == 4);
    relink::Found found{{list[0].path, dir + "/moved/Song_audio/" + util::baseName(list[0].path)}};
    CHECK(relink::alongside(list, found) == 2);
    CHECK(relink::apply(a, found, SR, err) == 3 && missingCount(a) == 0);
}

// --- archives: round trip; damaged and zip-slip ones refused, nothing left behind ---
static void testArchive() {
    std::string dir = kDir + "/archive";
    util::makeDirs(dir);
    Project p;
    TrackPtr t = p.addTrack(Kind::Audio);
    for (int i = 0; i < 6; i++) {
        auto c = makeAudioClip(util::format("take %d", i), sound(2000 + i * 10, 0.2f + 0.1f * (float)i), SR, SR);
        c->track = t;
        c->start = i * 3000;
        p.addClip(c);
    }
    p.clips[5]->source = p.clips[4]->source;  // (two clips of one sound: one file)
    MasterSettings master;
    master.volumeDb = -2;
    std::string err, zipPath = dir + "/Song.tinydaw.zip";
    CHECK(collect::isArchive(zipPath) && collect::archiveStem(zipPath) == "Song");
    CHECK(collect::saveArchive(zipPath, p, SR, master, err));
    CHECK(util::fileExists(zipPath) && !util::isDir(zipPath + ".staging") && !util::fileExists(zipPath + ".part"));
    for (auto& c : p.clips) CHECK(c->sourcePath.empty());  // (the project itself as it was)
    std::vector<std::string> names;
    CHECK(zip::list(zipPath, names, err) && names.size() == 6 && names[0] == "Song.json");
    std::string folder = collect::defaultFolder(zipPath), json;
    CHECK(folder == dir + "/Song");
    CHECK(collect::openArchive(zipPath, folder, json, err));
    CHECK(json == folder + "/Song.json" && util::fileExists(folder + "/Song_audio/take 0.wav"));
    CHECK(collect::defaultFolder(zipPath) == dir + "/Song (2)");
    Project q;
    MasterSettings m2;
    CHECK(loadProject(json, q, SR, m2, err));
    CHECK(q.clips.size() == 6 && m2.volumeDb == -2 && missingCount(q) == 0);
    for (size_t i = 0; i < 6; i++)
        CHECK(q.clips[i]->source->samples == p.clips[i]->source->samples && q.clips[i]->start == p.clips[i]->start);
    CHECK(!collect::openArchive(zipPath, folder, json, err));  // (never over a folder that is there)

    std::string zipBytes;
    CHECK(util::readFile(zipPath, zipBytes));
    auto refused = [&](const std::string& name, const std::string& bytes, const std::string& expect) {
        std::string bad = dir + "/" + name + ".tinydaw.zip", into = dir + "/" + name;
        CHECK(util::writeFile(bad, bytes));
        std::string e;
        CHECK(!collect::openArchive(bad, into, json, e));
        if (e.find(expect) == std::string::npos) printf("unexpected error for %s: %s\n", name.c_str(), e.c_str());
        CHECK(e.find(expect) != std::string::npos);
        CHECK(!util::isDir(into) && !util::isDir(into + ".part"));  // (nothing half-written left)
    };
    // cut short; a byte of a sample flipped (its check sum); random bytes; empty
    refused("cut", zipBytes.substr(0, zipBytes.size() / 2), "not a zip archive");
    std::string flipped = zipBytes;
    flipped[zipBytes.size() / 2] ^= 0x40;
    refused("flipped", flipped, "damaged");
    refused("noise", std::string(5000, '\x5a'), "not a zip archive");
    refused("empty", "", "not a zip archive");
    // zip-slip: "../" and absolute names (written as safe names of the same length, then changed)
    std::string a = dir + "/slip-src.wav";
    CHECK(util::writeFile(a, "RIFF evil"));
    for (auto [safe, evil] : std::vector<std::pair<std::string, std::string>>{
             {"aa/evil.json", "../evil.json"}, {"Xtmp/evil.json", "/tmp/evil.json"}, {"x\\y.json", "x\\y.json"}}) {
        if (safe.find('\\') != std::string::npos) {
            CHECK(!zip::safeName(safe));
            continue;
        }
        std::string z = dir + "/slip.zip";
        CHECK(zip::write(z, {{"Song.json", a}, {safe, a}}, err));
        std::string b;
        CHECK(util::readFile(z, b));
        for (size_t at = b.find(safe); at != std::string::npos; at = b.find(safe, at + 1)) b.replace(at, safe.size(), evil);
        refused("slip", b, "outside its folder");
        CHECK(!util::fileExists(dir + "/evil.json"));
    }
    for (const char* bad : {"", "/a", "../a", "a/../b", "a//b", "./a", "a/./b", "a\\b", "a\x01"}) CHECK(!zip::safeName(bad));
    for (const char* good : {"a", "a/b.wav", "Song_audio/", "x y/z (2).wav"}) CHECK(zip::safeName(good));
    // a zip bomb's claim: a file said to be far bigger than deflate can make it
    std::string bomb = zipBytes;
    size_t central = bomb.find("PK\x01\x02");
    CHECK(central != std::string::npos);
    if (central != std::string::npos) {
        bomb[central + 24] = (char)0xff;  // (its size, little-endian: 4 GB - 1)
        bomb[central + 25] = (char)0xff;
        bomb[central + 26] = (char)0xff;
        bomb[central + 27] = (char)0x7f;
        refused("bomb", bomb, "damaged");
    }
}

// --- opening on every core: the same as one after another, bit for bit, and faster ---
static void testParallel() {
    std::string dir = kDir + "/parallel";
    util::makeDirs(dir + "/flac");
    const int FILES = 96;
    std::string err;
    Project p;
    TrackPtr t = p.addTrack(Kind::Audio);
    std::vector<std::string> names;
    for (int i = 0; i < FILES; i++) {  // (FLAC: decoding takes work worth spreading)
        AudioPtr a = sound(3 * 44100, 0.1f + 0.005f * (float)i);
        std::string f = util::format("flac/f%02d.flac", i);
        CHECK(writeFlac(dir + "/" + f, a->samples.data(), (size_t)a->frames(), 2, 44100, SampleFormat::Pcm16, err));
        for (int k = 0; k < 2; k++) {  // (each used twice: decoded once)
            auto c = makeAudioClip(f, a, 44100, SR);
            c->track = t;
            c->start = (int64_t)(i * 2 + k) * SR;
            p.addClip(c);
            names.push_back(f);
        }
    }
    size_t n = 0;
    MasterSettings master;
    Json data = describe(p, SR, master, [&](const Clip&) { return names[n++ % names.size()]; });
    CHECK(writeJson(dir + "/Par.json", data, err));
    auto timed = [&](int cores, Project& into) {
        workers::setChosenCores(cores);
        double best = 1e9;
        for (int run = 0; run < 2; run++) {
            MasterSettings m;
            double t0 = util::wallClock();
            CHECK(loadProject(dir + "/Par.json", into, SR, m, err));
            best = std::min(best, util::wallClock() - t0);
        }
        return best;
    };
    Project serial, parallel;
    double one = timed(1, serial), all = timed(0, parallel);
    workers::setChosenCores(0);
    printf("opening %d FLAC files: %.3f s one after another, %.3f s on %d cores: %.1fx faster\n", FILES, one, all,
           workers::availableCores(), one / std::max(1e-9, all));
    CHECK(serial.clips.size() == (size_t)FILES * 2 && parallel.clips.size() == serial.clips.size());
    for (size_t i = 0; i < serial.clips.size(); i++) {
        CHECK(serial.clips[i]->source->samples == parallel.clips[i]->source->samples);
        CHECK(serial.clips[i]->data->samples == parallel.clips[i]->data->samples);
    }
    CHECK(parallel.clips[0]->source == parallel.clips[1]->source);  // (one file, decoded once)
    CHECK(describe(serial, SR, master, [](const Clip& c) { return c.sourcePath; }) ==
          describe(parallel, SR, master, [](const Clip& c) { return c.sourcePath; }));
    if (workers::availableCores() >= 2) CHECK(all < one);
    std::vector<std::string> files;  // (the decoding alone, without the rest of opening)
    for (int i = 0; i < FILES; i++) files.push_back(dir + util::format("/flac/f%02d.flac", i));
    double t0 = util::wallClock();
    auto alone = decodeFiles(files, 0);
    double t1 = util::wallClock();
    auto spread = decodeFiles(files, -1);
    double t2 = util::wallClock();
    printf("decoding alone: %.3f s on one core, %.3f s on all: %.1fx faster\n", t1 - t0, t2 - t1, (t1 - t0) / std::max(1e-9, t2 - t1));
    for (int i = 0; i < FILES; i++) CHECK(alone[(size_t)i].audio->samples == spread[(size_t)i].audio->samples);
    // progress: called on this thread, ending at the total
    int last = -1, total = 0;
    Project withProgress;
    MasterSettings m;
    CHECK(loadProject(dir + "/Par.json", withProgress, SR, m, err, [&](int d, int tot) {
        CHECK(d >= last);
        last = d;
        total = tot;
    }));
    CHECK(last == FILES && total == FILES);
    // a file that is there but damaged: an error, the project opened into left as it was
    CHECK(util::writeFile(dir + "/flac/f05.flac", "fLaC broken"));
    std::string e;
    CHECK(!loadProject(dir + "/Par.json", withProgress, SR, m, e) && e.find("f05.flac") != std::string::npos);
    CHECK(withProgress.clips.size() == (size_t)FILES * 2);
}

int main() {
    util::makeDirs(kDir);
    clearDir(kDir);
    double t0 = util::wallClock();
    testRelinkMany();
    testCollect();
    testArchive();
    testParallel();
    printf("(%.1f s)\n", util::wallClock() - t0);
    TEST_MAIN_END();
}
