// Peak meter with one horizontal bar per output channel in use (level_meter.py).
#include "ui/level_meter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>

#include "audio/dsp.h"
#include "config.h"
#include "ui/paint_helpers.h"
#include "ui/theme.h"

namespace level_meter {

double dbFraction(double db) { return std::min(1.0, std::max(0.0, (db - config::MIN_DB) / -(double)config::MIN_DB)); }

std::string outputLabel(const std::vector<int>& channels) {
    std::vector<std::string> names;
    for (int c : channels) names.push_back(std::to_string(c + 1));
    if (names.empty()) return "";
    if (names.size() == 1) return "output " + names[0];
    std::string s = "outputs ";
    for (size_t i = 0; i + 1 < names.size(); ++i) s += (i ? ", " : "") + names[i];
    return s + " and " + names.back();
}

double testNow = -1;

double now() {
    if (testNow >= 0) return testNow;
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::vector<float> loudest(const std::vector<const std::vector<float>*>& peaks) {
    std::vector<float> out;
    for (auto* p : peaks) {
        if (!p) continue;
        if (p->size() > out.size()) out.resize(p->size(), 0.f);
        for (size_t i = 0; i < p->size(); ++i) out[i] = std::max(out[i], (*p)[i]);
    }
    return out;
}

}  // namespace level_meter

namespace {
Rect clipBox(const RectF& r) { return Rect((int)r.x, (int)r.y, (int)r.w, (int)r.h); }

int floorDiv(int a, int b) {  // Python's //
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
}  // namespace

LevelMeter::LevelMeter(int channels, double clipDb) {
    setStyleType("LevelMeter");
    clipGain_ = std::pow(10.0, clipDb / 20);
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
    setMinimumHeight(4);
    setChannels(channels);
}

void LevelMeter::setChannels(int channels) {
    channels_ = channels;
    showChannels(wanted_);
}

void LevelMeter::showChannels(const std::vector<int>& indexes) {
    wanted_ = indexes;
    std::set<int> s;
    for (int i : wanted_)
        if (i >= 0 && i < channels_) s.insert(i);
    std::vector<int> shown(s.begin(), s.end());
    if (shown.empty()) shown = {0};
    if (shown != shown_) {
        shown_ = shown;
        levels_.assign(shown_.size(), 0.0);
        power_.assign(shown_.size(), 0.0);
        rms_.assign(shown_.size(), 0.0);
        held_.assign(shown_.size(), 0.0);
        heldAt_.assign(shown_.size(), 0.0);
        showTooltip();
        update();
    }
}

void LevelMeter::showTooltip() {
    std::string loud = "nothing yet";
    if (loudest_ > 0) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f dBFS", dsp::gainToDb(loudest_));
        loud = buf;
    }
    setToolTip("Peak level of " + level_meter::outputLabel(shown_) + ", top to bottom (the thin line: the last peak; "
               "the thin bar inside: RMS, the average level).\n"
               "Loudest since Play: " + loud + (clipped_ ? "; it went over full scale (clipping)" : "") +
               ". Click to clear it.");
}

void LevelMeter::takeOver(const LevelMeter& old) {
    loudest_ = old.loudest_;
    clipped_ = old.clipped_;
    if (old.shown_ == shown_) {
        levels_ = old.levels_;
        power_ = old.power_;
        rms_ = old.rms_;
        rmsAt_ = old.rmsAt_;
        held_ = old.held_;
        heldAt_ = old.heldAt_;
    }
    showTooltip();
    update();
}

void LevelMeter::resetPeaks() {
    loudest_ = 0.0;
    clipped_ = false;
    showTooltip();
    update();
}

bool LevelMeter::mousePress(ui::MouseEvent& e) {
    resetPeaks();
    return true;
}

void LevelMeter::push(const std::vector<float>* peaks) {
    std::vector<double> raw(levels_.size(), 0.0);
    if (peaks) {
        for (size_t k = 0; k < shown_.size(); ++k) {
            size_t i = (size_t)shown_[k];
            raw[k] = i < peaks->size() ? (*peaks)[i] : 0.0;
        }
    }
    std::vector<double> next(raw.size()), held(raw.size()), heldAt = heldAt_;
    double t = level_meter::now();
    for (size_t k = 0; k < raw.size(); ++k) {
        next[k] = std::max(raw[k], levels_[k] * config::METER_DECAY);
        if (next[k] < 1e-4) next[k] = 0.0;  // let idle meters settle so they stop repainting
        bool rose = raw[k] >= held_[k];
        held[k] = rose ? raw[k] : t - heldAt_[k] > level_meter::HOLD_S ? held_[k] * config::METER_DECAY : held_[k];
        if (held[k] < 1e-4) held[k] = 0.0;
        if (rose) heldAt[k] = t;
    }
    bool changed = !(next == levels_ && held == held_);
    levels_ = next;
    held_ = held;
    heldAt_ = heldAt;
    double top = raw.empty() ? 0.0 : *std::max_element(raw.begin(), raw.end());
    if (top > loudest_ || (top > clipGain_ && !clipped_)) {
        loudest_ = std::max(loudest_, top);
        clipped_ = clipped_ || top > clipGain_;
        showTooltip();
        changed = true;
    }
    if (changed) update();
}

void LevelMeter::pushRms(const std::vector<float>* meanSquares) {
    double t = level_meter::now();
    // (an average of the last RMS_S, however often it's pushed; the first at once)
    double k = rmsAt_ < 0 ? 1.0 : 1 - std::exp(-std::min(1.0, std::max(0.0, t - rmsAt_)) / level_meter::RMS_S);
    rmsAt_ = t;
    bool changed = false;
    for (size_t b = 0; b < shown_.size() && b < power_.size(); ++b) {
        size_t i = (size_t)shown_[b];
        double ms = meanSquares && i < meanSquares->size() ? (*meanSquares)[i] : 0.0;
        power_[b] += (ms - power_[b]) * k;
        if (power_[b] < 1e-9) power_[b] = 0;  // (settles, so idle meters stop repainting)
        double r = std::sqrt(power_[b]);
        changed = changed || r != rms_[b];
        rms_[b] = r;
    }
    if (changed) update();
}

Rect LevelMeter::inner() const {
    Rect in = ui::style::inside(rect(), look().inset);  // (inside its box's border and padding: none by default)
    return Rect(in.x, in.y, std::max(0, in.w), std::max(0, in.h));
}

int LevelMeter::barsW() const {
    int w = inner().w;
    return w >= level_meter::NUMBER_MIN_W ? w - level_meter::NUMBER_W : w;
}

std::vector<std::pair<int, int>> LevelMeter::barRects() const {
    Rect in = inner();
    int n = (int)levels_.size(), w = in.w, h = in.h;
    std::vector<std::pair<int, int>> out;
    if (n == 0) return out;
    int g = look().gap;
    int gap = n * (1 + g) - g <= h ? g : 0;  // no room for gaps: bars touch
    int thickness = std::max(1, std::min((int)(w * level_meter::MAX_BAR_FRACTION), floorDiv(h - gap * (n - 1), n)));
    int total = n * thickness + gap * (n - 1);
    if (total > h) {  // not even 1 px each: spread over the height (neighbours may share a pixel)
        for (int i = 0; i < n; ++i) out.push_back({in.y + std::min(h - 1, floorDiv(i * h, n)), 1});
        return out;
    }
    int top = in.y + (h - total) / 2;
    for (int i = 0; i < n; ++i) out.push_back({top + i * (thickness + gap), thickness});
    return out;
}

const LevelMeter::Look& LevelMeter::look() const {
    if (!parts_.refresh()) return look_;
    Look l;
    l.self = &parts_.get("");
    l.inset = ui::style::contentInsets(*l.self);
    l.selfShows = paint::shows(*l.self);
    l.track = &parts_.get("track");
    l.trackInset = ui::style::contentInsets(*l.track);
    const char* ranges[] = {"ok", "warn", "hot"};
    for (int i = 0; i < 3; ++i) {
        l.barStyle[i] = &parts_.get("bar", ranges[i]);
        l.peakStyle[i] = &parts_.get("peak", ranges[i]);
        l.bar[i] = panel_style::bg(*l.barStyle[i]);
        l.peak[i] = panel_style::bg(*l.peakStyle[i]);
    }
    l.peakW = std::max(1, panel_style::px(parts_.get("peak", "ok"), "width", 2));
    l.gap = std::max(0, theme::metric("meter-gap", level_meter::GAP));
    const css::Style& number = parts_.get("number");
    const css::Style& clipped = parts_.get("number", "clipped");
    l.number = &number;
    l.numberText = panel_style::fg(number);
    l.clipped = &clipped;
    l.clippedText = panel_style::fg(clipped);
    l.font = ui::style::font(number, theme::uiFont());
    l.clippedFont = ui::style::font(clipped, theme::uiFont());
    look_ = l;
    return look_;
}

void LevelMeter::paint(Painter& p) {
    const Look& look = this->look();
    if (look.selfShows) ui::style::box(p, rect(), *look.self);  // (its own box, around it all: none by default)
    Rect all = inner();
    int left = all.x, w = barsW();
    auto rects = barRects();
    for (size_t k = 0; k < rects.size() && k < levels_.size(); ++k) {
        int y = rects[k].first, thickness = rects[k].second;
        Rect track(left, y, w, thickness);
        ui::style::box(p, track, *look.track);
        // the bar inside the track's border and padding (none by default: the track's whole rect)
        Rect in = ui::style::inside(track, look.trackInset);
        if (in.w <= 0 || in.h <= 0) continue;
        int bw = in.w;
        int warnW = (int)(bw * level_meter::dbFraction(level_meter::WARN_DB));
        int hotW = (int)(bw * level_meter::dbFraction(level_meter::HOT_DB));
        int barW = (int)(bw * level_meter::dbFraction(dsp::gainToDb(levels_[k])));
        // green up to -12 dB, yellow up to 0 dB, red at the right end
        struct Part { int right, left, range; };
        Part parts[] = {{std::min(barW, warnW), 0, 0}, {std::min(barW, hotW), warnW, 1}, {barW, hotW, 2}};
        for (auto& part : parts)
            if (part.right > part.left)
                panel_style::fillPart(p, RectF(Rect(in.x + part.left, in.y, part.right - part.left, in.h)),
                                      look.barStyle[part.range], look.bar[part.range]);
        double rms = k < rms_.size() ? rms_[k] : 0.0;
        if (rms > 0) {  // the RMS: a thin bar along the middle of the peak bar
            double rmsDb = dsp::gainToDb(rms);
            int rw = (int)(bw * level_meter::dbFraction(rmsDb));
            int rh = std::max(1, in.h / 3);
            int range = rmsDb >= level_meter::HOT_DB ? 2 : rmsDb >= level_meter::WARN_DB ? 1 : 0;
            if (rw > 0)
                panel_style::fillPart(p, RectF(Rect(in.x, in.y + (in.h - rh) / 2, rw, rh)), look.peakStyle[range],
                                      look.peak[range]);
        }
        double held = k < held_.size() ? held_[k] : 0.0;
        if (held > 0) {  // the held peak: a thin line in its level's colour (red at full scale, wherever drawn)
            double heldDb = dsp::gainToDb(held);
            int x = std::min(bw - look.peakW, (int)(bw * level_meter::dbFraction(heldDb)));
            int range = heldDb >= level_meter::HOT_DB ? 2 : heldDb >= level_meter::WARN_DB ? 1 : 0;
            panel_style::fillPart(p, RectF(Rect(in.x + x, in.y, look.peakW, in.h)), look.peakStyle[range],
                                  look.peak[range]);
        }
    }
    if (w < all.w) {  // the loudest since Play; red once it clipped
        RectF box((float)(left + w + 2), (float)all.y, (float)level_meter::NUMBER_W - 2, (float)all.h);
        ui::style::box(p, clipBox(box), clipped_ ? *look.clipped : *look.number);
        std::string text = "–";
        if (loudest_ > 0) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.1f", dsp::gainToDb(loudest_));
            text = buf;
            if (text[0] == '-') text = "−" + text.substr(1);  // (a real minus sign)
        }
        float size = (float)std::max(6.0, std::min(8.0, all.h * 0.55));
        Font font = (clipped_ ? look.clippedFont : look.font).withPointSize(size);
        p.drawText(box, AlignCenter, text, font, clipped_ ? look.clippedText : look.numberText);
    }
}
