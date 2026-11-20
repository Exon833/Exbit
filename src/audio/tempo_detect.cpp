// An audio file's tempo (see tempo_detect.h)
#include "audio/tempo_detect.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/util.h"

namespace tempo_detect {

namespace {
uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
bool plausible(double bpm) { return std::isfinite(bpm) && bpm >= 20 && bpm <= 999; }
}  // namespace

double acidTempo(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    unsigned char head[12];
    double bpm = 0;
    if (std::fread(head, 1, 12, f) == 12 && !std::memcmp(head, "RIFF", 4) && !std::memcmp(head + 8, "WAVE", 4)) {
        unsigned char chunk[8];
        for (int guard = 0; guard < 4096 && std::fread(chunk, 1, 8, f) == 8; guard++) {
            uint32_t size = le32(chunk + 4);
            if (!std::memcmp(chunk, "acid", 4) && size >= 24) {
                unsigned char acid[24];
                if (std::fread(acid, 1, 24, f) != 24) break;
                uint32_t bits = le32(acid + 20);
                float tempo;
                std::memcpy(&tempo, &bits, 4);
                if (plausible(tempo)) bpm = tempo;
                break;
            }
            if (std::fseek(f, (long)size + (size & 1), SEEK_CUR) != 0) break;
        }
    }
    std::fclose(f);
    return bpm;
}

double nameTempo(const std::string& fileName) {
    std::string name = util::lower(util::baseName(fileName));
    size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    auto number = [&](size_t from, size_t to) {  // the number name[from, to), or 0
        if (from >= to) return 0.0;
        std::string s = name.substr(from, to - from);
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        return end && *end == 0 && plausible(v) ? v : 0.0;
    };
    auto isNum = [](char ch) { return std::isdigit((unsigned char)ch) || ch == '.'; };
    // "120bpm", "120 bpm", "120_bpm", "bpm120", "bpm 120"
    for (size_t at = name.find("bpm"); at != std::string::npos; at = name.find("bpm", at + 3)) {
        size_t e = at;
        while (e > 0 && (name[e - 1] == ' ' || name[e - 1] == '_' || name[e - 1] == '-')) e--;
        size_t s = e;
        while (s > 0 && isNum(name[s - 1])) s--;
        if (double v = number(s, e)) return v;
        size_t b = at + 3;
        while (b < name.size() && (name[b] == ' ' || name[b] == '_' || name[b] == '-')) b++;
        size_t c = b;
        while (c < name.size() && isNum(name[c])) c++;
        if (double v = number(b, c)) return v;
    }
    // "loop_120_": a number between separators, a likely tempo
    auto sep = [&](size_t i) { return i >= name.size() || name[i] == '_' || name[i] == ' ' || name[i] == '-'; };
    for (size_t i = 0; i < name.size(); i++) {
        if (!std::isdigit((unsigned char)name[i]) || (i > 0 && !sep(i - 1))) continue;
        size_t j = i;
        while (j < name.size() && std::isdigit((unsigned char)name[j])) j++;
        if (i > 0 && sep(j) && j - i >= 2 && j - i <= 3) {
            double v = number(i, j);
            if (v >= 60 && v <= 200) return v;
        }
        i = j;
    }
    return 0;
}

double analyse(const AudioBuffer& audio, int sr) {
    if (sr <= 0 || audio.frames() < sr) return 0;
    int64_t frames = std::min<int64_t>(audio.frames(), (int64_t)(ANALYSIS_S * sr));
    dsp::Onsets onsets(audio.samples.data(), frames, sr);
    std::vector<double> e = onsets.strength();
    const int n = (int)e.size();
    const double rate = (double)sr / dsp::ONSET_HOP;  // envelope values per second
    double mean = 0;
    for (double v : e) mean += v;
    mean /= std::max(1, n);
    for (double& v : e) v -= mean;
    const int K = 4;  // a beat and its multiples
    int maxLag = std::min(n - 1, (int)std::ceil(K * 60.0 / MIN_BPM * rate) + 2);
    if (maxLag < (int)(60.0 / MAX_BPM * rate) * 2) return 0;
    std::vector<double> ac((size_t)maxLag + 1, 0.0);  // (each lag over its overlap: unbiased)
    for (int lag = 0; lag <= maxLag; lag++) {
        double s = 0;
        for (int i = 0; i + lag < n; i++) s += e[(size_t)i] * e[(size_t)(i + lag)];
        ac[(size_t)lag] = s / (n - lag);
    }
    if (ac[0] <= 0) return 0;
    auto acAt = [&](double lag) {
        int i = (int)lag;
        if (i < 0 || i + 1 > maxLag) return 0.0;
        double t = lag - i;
        return ac[(size_t)i] * (1 - t) + ac[(size_t)i + 1] * t;
    };
    auto weight = [](double bpm) {
        if (bpm >= LIKELY_LO && bpm <= LIKELY_HI) return 1.0;
        double x = std::log2(bpm / (bpm < LIKELY_LO ? LIKELY_LO : LIKELY_HI)) / 0.25;
        return std::exp(-0.5 * x * x);
    };
    auto score = [&](double bpm) {
        // (and half a beat: the off-beats line up with a tempo, not with 2/3 or 3/2 of it)
        double lag = 60.0 / bpm * rate, s = acAt(lag / 2);
        int used = 1;
        for (int k = 1; k <= K && k * lag < maxLag; k++, used++) s += acAt(k * lag);
        return used > 1 ? s / used * weight(bpm) : -1e300;
    };
    double best = 0, bestScore = -1e300;
    for (double bpm = MIN_BPM; bpm <= MAX_BPM + 1e-9; bpm += 0.02) {
        double s = score(bpm);
        if (s > bestScore) bestScore = s, best = bpm;
    }
    if (bestScore <= 0) return 0;
    // refined: where each multiple's peak really is (a parabola through the autocorrelation around it)
    double lag = 60.0 / best * rate, sum = 0, weights = 0;
    for (int k = 1; k <= K && k * lag + 1 < maxLag; k++) {
        int i = (int)std::lround(k * lag);
        for (int step = 0; step < 3; step++) {  // (climb to the local peak nearby)
            if (i + 1 <= maxLag && ac[(size_t)i + 1] > ac[(size_t)i]) i++;
            else if (i - 1 >= 1 && ac[(size_t)i - 1] > ac[(size_t)i]) i--;
        }
        if (i < 1 || i + 1 > maxLag) continue;
        double a = ac[(size_t)i - 1], b = ac[(size_t)i], c = ac[(size_t)i + 1], den = a - 2 * b + c;
        double peak = i + (den < 0 ? 0.5 * (a - c) / den : 0.0);
        if (std::fabs(peak - k * lag) > 0.6 * k + 1) continue;
        sum += peak * k;  // (weighted by k: a longer lag says more precisely how long a beat is)
        weights += k * k;
    }
    double bpm = weights > 0 ? 60.0 / (sum / weights) * rate : best;
    return bpm >= MIN_BPM - 1 && bpm <= MAX_BPM + 1 ? bpm : best;
}

Found detect(const std::string& path, const AudioPtr& audio, int sr, const std::string& name) {
    Found f;
    if (!path.empty() && (f.bpm = acidTempo(path)) > 0) f.from = "acid chunk";
    if (f.bpm <= 0 && (f.bpm = nameTempo(name.empty() ? path : name)) > 0) f.from = "file name";
    if (f.bpm <= 0 && audio) {
        f.bpm = analyse(*audio, sr);
        if (f.bpm > 0) f.from = "analysis";
    }
    util::debugf("tempo: %s: %.2f BPM from %s\n", path.empty() ? "(audio)" : util::baseName(path).c_str(), f.bpm,
                 f.from.empty() ? "nothing" : f.from.c_str());
    return f;
}

}  // namespace tempo_detect
