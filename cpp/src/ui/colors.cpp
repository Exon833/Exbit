#include "ui/colors.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <tuple>

#include "ui/theme.h"

namespace colors {

const Color WHITE(255, 255, 255), INK(18, 20, 24);
bool followTrack = false;

namespace {
struct ColorF { double r, g, b; };
ColorF f(const Color& c) { return {c.r / 255.0, c.g / 255.0, c.b / 255.0}; }
Color fromF(double r, double g, double b) {
    // QColor.fromRgbF keeps 16 bits; name() rounds to 8 bits
    auto q = [](double v) { return (int)std::lround(std::clamp(v, 0.0, 1.0) * 255.0); };
    return Color(q(r), q(g), q(b));
}
std::mt19937& rng() {
    static std::mt19937 r(std::random_device{}());
    return r;
}
std::map<std::tuple<std::string, bool, std::string>, ClipColors> clipCache;
std::map<std::pair<std::string, std::string>, std::pair<Color, Color>> trackCache;
}  // namespace

double luminance(const Color& c) {
    auto ch = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    ColorF x = f(c);
    return 0.2126 * ch(x.r) + 0.7152 * ch(x.g) + 0.0722 * ch(x.b);
}

double contrast(const Color& a, const Color& b) {
    double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

Color mix(const Color& a, const Color& b, double t) {
    ColorF x = f(a), y = f(b);
    return fromF(x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t);
}

Color ensureContrast(const Color& color, const Color& against, double ratio) {
    if (contrast(color, against) >= ratio) return color;
    std::vector<Color> dirs{WHITE, INK};
    std::stable_sort(dirs.begin(), dirs.end(),
                     [&](const Color& a, const Color& b) { return contrast(a, against) > contrast(b, against); });
    for (auto& target : dirs) {
        if (contrast(target, against) < ratio) continue;
        for (int step = 1; step <= 20; step++) {
            Color c = mix(color, target, step / 20.0);
            if (contrast(c, against) >= ratio) return c;
        }
    }
    return dirs[0];
}

Color readableText(const Color& bg) { return contrast(WHITE, bg) >= contrast(INK, bg) ? WHITE : INK; }

Color trackAccent(Kind kind) {
    if (kind != Kind::Return) return theme::c(kind == Kind::Midi ? "accent_midi" : "accent_audio");
    for (auto& info : theme::colors())  // (a skin list without it yet: the successor's default)
        if (std::string(info.name) == "accent_return") return theme::c("accent_return");
    return Color::fromHex("#d0913c");
}

std::string defaultColor(Kind kind) {
    if (kind == Kind::Return) return trackAccent(kind).hexRgb();  // (return tracks have no clips)
    return theme::c(kind == Kind::Midi ? "clip_midi" : "clip_audio").hexRgb();
}

void clipColor(const Clip& clip, std::string& color, std::string& tone) {
    if (!clip.color.empty()) {
        color = clip.color;
        tone = clip.tone;
        return;
    }
    TrackPtr track = std::atomic_load(&clip.track);
    if (followTrack && track && !track->color.empty()) {
        color = track->color;
        tone = !clip.tone.empty() ? clip.tone : track->tone;
        return;
    }
    color = defaultColor(clip.kind);
    tone = clip.tone;
}

std::string randomColor(const std::vector<std::string>& taken) {
    std::vector<double> hues;
    for (auto& t : taken) {
        if (t.empty()) continue;
        float h, s, v;
        Color::fromHex(t).toHsv(h, s, v);
        if (h >= 0) hues.push_back(h / 360.0);
    }
    auto distance = [&](double h) {
        double d = 1.0;
        bool any = false;
        for (double t : hues) {
            double x = std::min(std::fabs(h - t), 1 - std::fabs(h - t));
            d = any ? std::min(d, x) : x;
            any = true;
        }
        return d;
    };
    std::uniform_real_distribution<double> u(0, 1);
    double best = u(rng()), bestD = distance(best);
    for (int i = 1; i < 24; i++) {
        double h = u(rng()), d = distance(h);
        if (d > bestD) best = h, bestD = d;
    }
    double s = 0.45 + u(rng()) * 0.2, v = 0.62 + u(rng()) * 0.18;
    return Color::fromHsv((float)(best * 360.0), (float)s, (float)v).hexRgb();
}

Color toneColor(const std::string& tone) { return tone == "dark" ? Color::fromHex("#2a2c30") : WHITE; }

Color clipFill(const std::string& hexColor, bool audible) {
    Color base = Color::fromHex(hexColor);
    if (!audible) {
        float h, s, l;
        base.toHsl(h, s, l);
        base = Color::fromHsl(std::max(h, 0.0f), (float)(s * SILENT_SATURATION), l * 0.8f);
    }
    return ensureContrast(base, theme::c("lane_alt"), MIN_FILL_VS_LANE);
}

bool toneAvailable(const std::string& hexColor, const std::string& tone, bool audible) {
    return contrast(toneColor(tone), clipFill(hexColor, audible)) >= MIN_CONTENT_VS_FILL;
}

void trackColors(const std::string& hexColor, const std::string& tone, Color& background, Color& text) {
    auto key = std::make_pair(hexColor, tone);
    auto it = trackCache.find(key);
    if (it == trackCache.end()) {
        Color fill = clipFill(hexColor);
        Color t = !tone.empty() && toneAvailable(hexColor, tone) ? toneColor(tone) : readableText(fill);
        it = trackCache.emplace(key, std::make_pair(fill, t)).first;
    }
    background = it->second.first;
    text = it->second.second;
}

ClipColors clipColors(const std::string& hexColor, bool audible, const std::string& tone) {
    auto key = std::make_tuple(hexColor, audible, tone);
    auto it = clipCache.find(key);
    if (it != clipCache.end()) return it->second;
    if (clipCache.size() > 512) clipCache.clear();
    ClipColors c;
    c.fill = clipFill(hexColor, audible);
    bool darkFill = luminance(c.fill) < 0.3;
    Color handle = luminance(c.fill) > 0.1 ? mix(c.fill, INK, 0.3) : mix(c.fill, WHITE, 0.12);
    if (!tone.empty() && toneAvailable(hexColor, tone, audible)) {
        c.content = c.text = toneColor(tone);
    } else {
        c.content = ensureContrast(mix(c.fill, darkFill ? WHITE : INK, 0.55), c.fill, MIN_CONTENT_VS_FILL);
        c.text = readableText(handle);
    }
    c.handle = ensureContrast(handle, c.text, MIN_TEXT_VS_HANDLE);
    c.border = mix(c.fill, INK, 0.45);
    c.selectedBorder = darkFill ? WHITE : INK;
    clipCache.emplace(key, c);
    return c;
}

void clearCache() {
    clipCache.clear();
    trackCache.clear();
}

}  // namespace colors
