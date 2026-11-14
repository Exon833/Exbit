// Peak meter with one horizontal bar per output channel in use (level_meter.py).
#include "ui/level_meter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>

#include "audio/dsp.h"
#include "config.h"
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
    setToolTip("Peak level of " + level_meter::outputLabel(shown_) + ", top to bottom (the thin line: the last peak).\n"
               "Loudest since Play: " + loud + (clipped_ ? "; it went over full scale (clipping)" : "") +
               ". Click to clear it.");
}

void LevelMeter::takeOver(const LevelMeter& old) {
    loudest_ = old.loudest_;
    clipped_ = old.clipped_;
    if (old.shown_ == shown_) {
        levels_ = old.levels_;
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

int LevelMeter::barsW() const { return width() >= level_meter::NUMBER_MIN_W ? width() - level_meter::NUMBER_W : width(); }

std::vector<std::pair<int, int>> LevelMeter::barRects() const {
    int n = (int)levels_.size(), w = width(), h = height();
    std::vector<std::pair<int, int>> out;
    if (n == 0) return out;
    int gap = n * (1 + level_meter::GAP) - level_meter::GAP <= h ? level_meter::GAP : 0;  // no room for gaps: bars touch
    int thickness = std::max(1, std::min((int)(w * level_meter::MAX_BAR_FRACTION), floorDiv(h - gap * (n - 1), n)));
    int total = n * thickness + gap * (n - 1);
    if (total > h) {  // not even 1 px each: spread over the height (neighbours may share a pixel)
        for (int i = 0; i < n; ++i) out.push_back({std::min(h - 1, floorDiv(i * h, n)), 1});
        return out;
    }
    int top = (h - total) / 2;
    for (int i = 0; i < n; ++i) out.push_back({top + i * (thickness + gap), thickness});
    return out;
}

void LevelMeter::paint(Painter& p) {
    int w = barsW();
    int warnW = (int)(w * level_meter::dbFraction(level_meter::WARN_DB));
    int hotW = (int)(w * level_meter::dbFraction(level_meter::HOT_DB));
    auto rects = barRects();
    for (size_t k = 0; k < rects.size() && k < levels_.size(); ++k) {
        int y = rects[k].first, thickness = rects[k].second;
        p.fillRect(Rect(0, y, w, thickness), theme::c("meter_bg"));
        int barW = (int)(w * level_meter::dbFraction(dsp::gainToDb(levels_[k])));
        // green up to -12 dB, yellow up to 0 dB, red at the right end
        struct Part { int right, left; const char* color; };
        Part parts[] = {{std::min(barW, warnW), 0, "meter_ok"},
                        {std::min(barW, hotW), warnW, "meter_warn"},
                        {barW, hotW, "meter_hot"}};
        for (auto& part : parts)
            if (part.right > part.left) p.fillRect(Rect(part.left, y, part.right - part.left, thickness), theme::c(part.color));
        double held = k < held_.size() ? held_[k] : 0.0;
        if (held > 0) {  // the held peak: a thin line in its level's colour (red at full scale, wherever drawn)
            double heldDb = dsp::gainToDb(held);
            int x = std::min(w - 2, (int)(w * level_meter::dbFraction(heldDb)));
            const char* color = heldDb >= level_meter::HOT_DB ? "meter_hot" : heldDb >= level_meter::WARN_DB ? "meter_warn" : "meter_ok";
            p.fillRect(Rect(x, y, 2, thickness), theme::c(color));
        }
    }
    if (w < width()) {  // the loudest since Play; red once it clipped
        RectF box((float)w + 2, 0, (float)level_meter::NUMBER_W - 2, (float)height());
        p.fillRect(clipBox(box), theme::c(clipped_ ? "meter_hot" : "meter_bg"));
        std::string text = "–";
        if (loudest_ > 0) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.1f", dsp::gainToDb(loudest_));
            text = buf;
            if (text[0] == '-') text = "−" + text.substr(1);  // (a real minus sign)
        }
        Font font = theme::uiFont();
        font = font.withPointSize((float)std::max(6.0, std::min(8.0, height() * 0.55)));
        p.drawText(box, AlignCenter, text, font, theme::c(clipped_ ? "accent_text" : "ruler_text"));
    }
}
