// Finding missing samples again (see relink.h).
#include "model/relink.h"

#include <algorithm>
#include <set>

#include "audio/file_io.h"
#include "core/util.h"
#include "model/project_io.h"

namespace relink {

namespace {

// every clip holding a sample (missing or not), with what it is for the list: fn(clip, label)
template <class Fn>
void eachHolder(const Project& project, Fn fn) {
    for (auto& c : project.clips)
        if (c->kind != Kind::Midi && c->source) {
            TrackPtr t = std::atomic_load(&c->track);
            fn(c, "clip \xe2\x80\x9c" + c->name + "\xe2\x80\x9d" + (t ? " (" + t->name + ")" : std::string()));
        }
    for (auto& t : project.tracks) {
        if (t->sampler && t->sampler->sample) fn(t->sampler->sample, t->name + ": sampler");
        if (t->padKit)
            for (size_t i = 0; i < t->padKit->pads.size(); i++)
                if (t->padKit->pads[i].sound.sample) fn(t->padKit->pads[i].sound.sample, t->name + ": pad " + std::to_string(i + 1));
        for (auto& take : t->takes)
            if (take->kind != Kind::Midi && take->source) fn(take, t->name + ": take \xe2\x80\x9c" + take->name + "\xe2\x80\x9d");
    }
}

std::vector<std::string> folderParts(const std::string& file) { return util::split(util::dirName(file), '/'); }

// how many folders the two paths' folders end with alike ("…/Drums/Kits/a.wav", "…/Kits/a.wav": 1)
int sharedTail(const std::string& a, const std::string& b) {
    auto x = folderParts(a), y = folderParts(b);
    size_t n = 0;
    while (n < x.size() && n < y.size() && x[x.size() - 1 - n] == y[y.size() - 1 - n]) n++;
    return (int)n;
}

std::string joined(const std::vector<std::string>& parts, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; i++) out += (i ? "/" : "") + parts[i];
    return out;
}

bool isAudio(const std::string& path) {
    const auto& e = file_io::AUDIO_EXTENSIONS;
    return std::find(e.begin(), e.end(), util::extension(path)) != e.end();
}

}  // namespace

std::vector<Missing> missing(const Project& project) {
    std::map<std::string, Missing> byPath;
    eachHolder(project, [&](const ClipPtr& c, const std::string& label) {
        if (!c->missing) return;
        Missing& m = byPath[c->missing->path];
        m.path = c->missing->path;
        if (m.size < 0) m.size = c->missing->size;
        if (m.sound.empty()) m.sound = c->missing->sound;
        if (std::find(m.users.begin(), m.users.end(), label) == m.users.end()) m.users.push_back(label);
    });
    std::vector<Missing> out;
    for (auto& kv : byPath) out.push_back(kv.second);
    return out;
}

bool fits(const Missing& m, const std::string& file) {
    return !file.empty() && util::fileExists(file) && !util::isDir(file) && (m.size < 0 || util::fileSize(file) == m.size);
}

int alongside(const std::vector<Missing>& missing, Found& found) {
    int added = 0;
    for (bool more = true; more;) {  // (files found this way give roots of their own: again until none)
        more = false;
        std::set<std::pair<std::string, std::string>> roots;  // (old root, new root)
        for (auto& [was, now] : found) {
            auto x = folderParts(was), y = folderParts(now);
            size_t tail = (size_t)sharedTail(was, now);
            std::string from = joined(x, x.size() - tail), to = joined(y, y.size() - tail);
            if (!from.empty() && from != to) roots.insert({from, to});
        }
        for (const Missing& m : missing) {
            if (found.count(m.path)) continue;
            for (auto& [from, to] : roots) {
                if (!util::startsWith(m.path, from + "/")) continue;
                std::string there = to + m.path.substr(from.size());
                if (fits(m, there)) {
                    found[m.path] = there;
                    util::debugf("relink: %s found beside the others, as %s\n", m.path.c_str(), there.c_str());
                    added++;
                    more = true;
                    break;
                }
            }
        }
    }
    return added;
}

int search(const std::vector<Missing>& missing, const std::vector<std::string>& folders, Found& found,
           const std::function<bool(size_t seen)>& progress) {
    double t0 = util::wallClock();
    std::set<std::string> names;  // (lower case)
    std::set<int64_t> sizes;
    for (const Missing& m : missing)
        if (!found.count(m.path)) {
            names.insert(util::lower(util::baseName(m.path)));
            if (m.size >= 0) sizes.insert(m.size);
        }
    if (names.empty()) return 0;
    // one walk: the files named as a missing one, and those of a missing one's size (renamed)
    std::map<std::string, std::vector<std::pair<std::string, int64_t>>> byName;
    std::map<int64_t, std::vector<std::string>> bySize;
    std::set<std::string> seen;
    size_t looked = 0;
    bool stopped = false;
    for (const std::string& root : folders) {
        if (stopped || root.empty() || !util::isDir(root)) continue;
        util::walkFiles(util::absolutePath(root), [&](const std::string& path) {
            if (progress && ++looked % 256 == 0 && !progress(looked)) {
                stopped = true;
                return false;
            }
            if (!isAudio(path) || !seen.insert(path).second) return true;
            std::string name = util::lower(util::baseName(path));
            bool named = names.count(name) > 0;
            if (!named && sizes.empty()) return true;
            int64_t size = util::fileSize(path);
            if (named) byName[name].push_back({path, size});
            if (sizes.count(size)) bySize[size].push_back(path);
            return true;
        });
    }
    int added = 0;
    // by name (of its size when known): the one whose folders end most like the expected path's (the first
    // folder searched first, the Explorer's)
    for (const Missing& m : missing) {
        if (found.count(m.path)) continue;
        auto it = byName.find(util::lower(util::baseName(m.path)));
        if (it == byName.end()) continue;
        std::string best;
        int bestTail = -1;
        for (auto& [path, size] : it->second) {
            if (m.size >= 0 && size != m.size) continue;
            int tail = sharedTail(m.path, path);
            if (tail > bestTail) {
                best = path;
                bestTail = tail;
            }
        }
        if (!best.empty()) {
            found[m.path] = best;
            added++;
        }
    }
    // renamed: by size, then by sound (decoded, all at once); without a saved sound only a file alone of its size
    constexpr size_t MOST = 16;  // shortcut: candidates of one size looked at for each file; more: Locate… it
    std::vector<std::string> toHear;
    std::map<std::string, size_t> heard;  // candidate -> its place in toHear
    for (const Missing& m : missing) {
        if (found.count(m.path) || m.size < 0 || m.sound.empty()) continue;
        auto it = bySize.find(m.size);
        if (it == bySize.end()) continue;
        for (size_t i = 0; i < it->second.size() && i < MOST; i++)
            if (heard.emplace(it->second[i], toHear.size()).second) toHear.push_back(it->second[i]);
    }
    std::vector<project_io::Decoded> decoded = project_io::decodeFiles(toHear);
    std::map<std::string, std::string> soundOf;  // candidate -> fingerprint
    for (size_t i = 0; i < toHear.size(); i++)
        if (decoded[i].error.empty()) soundOf[toHear[i]] = project_io::fingerprint(decoded[i].audio, decoded[i].rate);
    decoded.clear();
    for (const Missing& m : missing) {
        if (found.count(m.path) || m.size < 0) continue;
        auto it = bySize.find(m.size);
        if (it == bySize.end()) continue;
        std::string match;
        if (!m.sound.empty()) {
            for (size_t i = 0; i < it->second.size() && i < MOST && match.empty(); i++) {
                auto s = soundOf.find(it->second[i]);
                if (s != soundOf.end() && s->second == m.sound) match = it->second[i];
            }
        } else if (it->second.size() == 1) {
            match = it->second[0];
        }
        if (!match.empty()) {
            found[m.path] = match;
            util::debugf("relink: %s found renamed, as %s (by its size and sound)\n", m.path.c_str(), match.c_str());
            added++;
        }
    }
    added += alongside(missing, found);
    util::debugf("relink: searched %zu folder(s), %zu audio file(s) in %.3f s: %d of %zu missing file(s) found%s\n",
                 folders.size(), seen.size(), util::wallClock() - t0, added, missing.size(), stopped ? " (stopped)" : "");
    return added;
}

int apply(Project& project, const Found& found, int sr, std::string& error) {
    // the files to read (each once)
    std::vector<std::string> files;
    std::map<std::string, size_t> at;
    eachHolder(project, [&](const ClipPtr& c, const std::string&) {
        if (!c->missing) return;
        auto f = found.find(c->missing->path);
        if (f != found.end() && at.emplace(f->second, files.size()).second) files.push_back(f->second);
    });
    std::vector<project_io::Decoded> decoded = project_io::decodeFiles(files);
    std::set<std::string> failed;
    for (size_t i = 0; i < files.size(); i++)
        if (!decoded[i].error.empty() && failed.insert(files[i]).second)
            error += (error.empty() ? "" : "\n") + files[i] + ": " + decoded[i].error;
    // the audio of a found file for this clip, if any
    auto got = [&](const Clip& c) -> const project_io::Decoded* {
        if (!c.missing) return nullptr;
        auto f = found.find(c.missing->path);
        if (f == found.end()) return nullptr;
        const project_io::Decoded& d = decoded[at[f->second]];
        return d.error.empty() ? &d : nullptr;
    };
    auto fill = [&](Clip& c, const project_io::Decoded& d) {
        c.sourcePath = found.at(c.missing->path);
        c.source = d.audio;
        c.sourceSr = d.rate;
        c.missing = nullptr;
    };
    // prepared on a copy, then swapped in (the audio thread may be playing it)
    auto prepareLive = [sr](const ClipPtr& clip) {
        Clip ready = *clip;
        ready.track = std::atomic_load(&clip->track);
        ready.prepare(sr);
        clip->baseFrames = ready.baseFrames;
        clip->loopFrom = ready.loopFrom;
        clip->loopFor = ready.loopFor;
        clip->warpKey = ready.warpKey;
        std::atomic_store(&clip->peaks, ready.peaks);
        std::atomic_store(&clip->played, ready.played);
        std::atomic_store(&clip->events, ready.events);
        std::atomic_store(&clip->data, ready.data);
    };
    std::set<std::string> in;  // (expected paths put in)
    for (auto& c : project.clips)
        if (const project_io::Decoded* d = got(*c)) {
            in.insert(c->missing->path);
            fill(*c, *d);
            prepareLive(c);
        }
    for (auto& t : project.tracks) {
        for (auto& take : t->takes)
            if (const project_io::Decoded* d = got(*take)) {
                in.insert(take->missing->path);
                fill(*take, *d);
                prepareLive(take);
            }
        bool changed = false;
        if (SamplerPtr s = std::atomic_load(&t->sampler); s && s->sample && got(*s->sample)) {
            auto made = std::make_shared<SamplerSettings>(*s);
            made->sample = copyClip(s->sample);
            in.insert(made->sample->missing->path);
            fill(*made->sample, *got(*s->sample));
            std::atomic_store(&t->sampler, SamplerPtr(made));  // replaced whole: live notes use it at once
            changed = true;
        }
        if (PadKitPtr k = std::atomic_load(&t->padKit)) {
            std::shared_ptr<PadKit> made;
            for (size_t i = 0; i < k->pads.size(); i++) {
                const ClipPtr& sample = k->pads[i].sound.sample;
                const project_io::Decoded* d = sample ? got(*sample) : nullptr;
                if (!d) continue;
                if (!made) made = std::make_shared<PadKit>(*k);
                ClipPtr c = copyClip(sample);
                in.insert(c->missing->path);
                fill(*c, *d);
                made->pads[i].sound.sample = c;
            }
            if (made) {
                std::atomic_store(&t->padKit, PadKitPtr(made));
                changed = true;
            }
        }
        if (changed) {  // (its notes play the samples now)
            for (auto& c : project.clipsOn(t))
                if (c->kind == Kind::Midi) prepareLive(c);
            for (auto& take : t->takes)
                if (take->kind == Kind::Midi) prepareLive(take);
        }
    }
    util::debugf("relink: %zu file(s) put in (%zu read on worker threads)%s\n", in.size(), files.size(),
                 failed.empty() ? "" : ", some couldn't be read");
    return (int)in.size();
}

}  // namespace relink
