#include "audio/key_detect.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "audio/midi_effects.h"

namespace key_detect {

namespace {
using Profile = std::array<double, 12>;  // from the tonic up

// Temperley's (Kostka–Payne corpus) profiles: how often each pitch class sounds in a passage in that key.
// The minor one is the natural minor's (SCALES' "Minor"): its 7th and flat 7th swapped, as the corpus is
// mostly harmonic minor (that is a scale of its own here).
const Profile MAJOR = {0.748, 0.060, 0.488, 0.082, 0.670, 0.460, 0.096, 0.715, 0.104, 0.366, 0.057, 0.400};
const Profile MINOR = {0.712, 0.084, 0.474, 0.618, 0.049, 0.460, 0.105, 0.747, 0.404, 0.067, 0.330, 0.133};

// a scale's profile: Temperley's for major and minor, else made from its notes on the same scale (the
// tonic and its fifth as strong as theirs, then the third, then its other notes, the rest least)
Profile profileOf(const std::string& scale, const std::vector<int>& steps) {
    if (scale == "Major") return MAJOR;
    if (scale == "Minor") return MINOR;
    Profile p;
    p.fill(0.08);
    bool third = false;
    for (int s : steps) {
        p[(size_t)s] = 0.42;
        if ((s == 3 || s == 4) && !third) {
            p[(size_t)s] = 0.64;
            third = true;
        }
    }
    p[0] = 0.73;
    if (std::find(steps.begin(), steps.end(), 7) != steps.end()) p[7] = 0.73;
    return p;
}

struct Prepared {
    std::string scale;
    Profile centred;  // the profile less its mean
    double norm;      // its length
    std::array<bool, 12> in{};  // the scale's notes
};

const std::vector<Prepared>& profiles() {
    static const std::vector<Prepared> all = [] {
        std::vector<Prepared> out;
        for (auto& s : midi_effects::SCALES) {
            Profile p = profileOf(s.first, s.second);
            double mean = 0;
            for (double v : p) mean += v / 12;
            Prepared q{s.first, {}, 0, {}};
            for (int d : s.second) q.in[(size_t)d] = true;
            for (size_t i = 0; i < 12; i++) {
                q.centred[i] = p[i] - mean;
                q.norm += q.centred[i] * q.centred[i];
            }
            q.norm = std::sqrt(q.norm);
            out.push_back(q);
        }
        return out;
    }();
    return all;
}
}  // namespace

std::vector<Candidate> rank(const std::vector<Note>& notes) {
    Profile h{};
    for (const Note& n : notes) {
        if (!(n.pitch >= 0 && n.pitch < 128)) continue;
        double w = n.end - n.start;
        h[(size_t)((int)n.pitch % 12)] += std::isfinite(w) && w > 0 ? w : 1e-3;  // (no length: barely counted)
    }
    Profile raw = h;
    double total = 0, mean = 0;
    for (double v : h) total += v;
    mean = total / 12;
    double norm = 0;
    for (double& v : h) {
        v -= mean;
        norm += v * v;
    }
    std::vector<Candidate> out;
    if (!(norm > 1e-18)) return out;  // (no notes, or every pitch class as much: nothing to tell)
    norm = std::sqrt(norm);
    for (const Prepared& p : profiles()) {
        bool preferred = p.scale == "Major" || p.scale == "Minor";
        for (int key = 0; key < 12; key++) {
            double dot = 0;
            double outside = 0;
            for (int i = 0; i < 12; i++) {
                dot += h[(size_t)((key + i) % 12)] * p.centred[(size_t)i];
                if (!p.in[(size_t)i]) outside += raw[(size_t)((key + i) % 12)];
            }
            out.push_back({key, p.scale, dot / (norm * p.norm) - outside / total + (preferred ? PREFERRED : 0)});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    return out;
}

std::vector<Candidate> detect(const std::vector<Note>& notes, int count) {
    std::vector<Candidate> all = rank(notes);
    if ((int)all.size() > count) all.resize((size_t)std::max(0, count));
    return all;
}

std::string name(const Candidate& c) {
    std::string scale = c.scale == "Major" ? "major" : c.scale == "Minor" ? "minor" : c.scale;
    return std::string(midi_effects::KEYS[((c.key % 12) + 12) % 12]) + " " + scale;
}

}  // namespace key_detect
