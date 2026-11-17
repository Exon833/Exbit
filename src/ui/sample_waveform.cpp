#include "ui/sample_waveform.h"

#include <algorithm>
#include <cmath>

#include "ui/formatting.h"
#include "ui/theme.h"

SampleWaveform::SampleWaveform(ui::Widget* parent) : ui::Widget(parent) {
    setStyleType("SampleWaveform");
    setFixedHeight(theme::metric("sample-waveform-height", HEIGHT));
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
    setCursor(ui::Cursor::Hand);
}

void SampleWaveform::setAudio(AudioPtr audio, const std::string& name, int sr) {
    audio_ = std::move(audio);
    name_ = name;
    seconds_ = audio_ ? (double)audio_->frames() / std::max(1, sr) : 0.0;
    peaks_.clear();
    peaksWidth_ = -1;
    position_.reset();
    setToolTip(audio_ ? "Click to play from there" : "");
    update();
}

void SampleWaveform::setPosition(std::optional<double> fraction) {
    if (fraction != position_) {
        position_ = fraction;
        update();
    }
}

const std::vector<SampleWaveform::Lane>& SampleWaveform::lanes(int width) {
    if (peaksWidth_ == width) return peaks_;
    peaks_.clear();
    peaksWidth_ = width;
    if (!audio_ || width < 1) return peaks_;
    const AudioBuffer& a = *audio_;
    int64_t n = a.frames();
    if (n <= 0) return peaks_;
    bool same = true;  // one lane if both channels are the same
    for (int64_t i = 0; i < n && same; ++i) same = a.samples[i * 2] == a.samples[i * 2 + 1];
    int count = same ? 1 : 2;
    // edges = linspace(0, n, width + 1)[:-1] as integers, at most n - 1; each column is
    // [edges[i], edges[i + 1]) (np.reduceat: a single sample where the next edge isn't after it)
    std::vector<int64_t> edges(width);
    for (int i = 0; i < width; ++i)
        edges[i] = std::min(n - 1, (int64_t)((double)n * i / width));
    for (int c = 0; c < count; ++c) {
        Lane lane;
        lane.mins.resize(width);
        lane.maxs.resize(width);
        for (int i = 0; i < width; ++i) {
            int64_t from = edges[i];
            int64_t to = i + 1 < width ? edges[i + 1] : n;
            if (to <= from) to = from + 1;
            float lo = a.samples[from * 2 + c], hi = lo;
            for (int64_t k = from + 1; k < to; ++k) {
                float v = a.samples[k * 2 + c];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            lane.mins[i] = lo;
            lane.maxs[i] = hi;
        }
        peaks_.push_back(std::move(lane));
    }
    return peaks_;
}

const SampleWaveform::Look& SampleWaveform::look() {
    if (!parts_.refresh()) return look_;
    Look l;
    const css::Style& self = parts_.get("");
    l.self = &self;
    l.text = ui::style::foreground(self);
    l.font = ui::style::font(self, theme::uiFont());
    const css::Style& caption = parts_.get("caption");
    l.caption = ui::style::foreground(caption);
    l.captionFont = ui::style::font(caption, theme::uiFont());
    l.captionH = std::max(0, panel_style::px(caption, "height", 14));
    l.padLeft = panel_style::px(caption, "padding-left", 4);
    l.padRight = panel_style::px(caption, "padding-right", 4);
    l.gap = panel_style::px(caption, "column-gap", 6);
    l.wave = panel_style::fg(parts_.get("wave"));
    const css::Style& midline = parts_.get("midline");
    l.midline = panel_style::bg(midline);
    l.midlineH = std::max(0, panel_style::px(midline, "height", 1));
    const css::Style& position = parts_.get("position");
    l.position = panel_style::bg(position);
    l.positionW = std::max(1, panel_style::px(position, "width", 1));
    look_ = l;
    return look_;
}

void SampleWaveform::paint(Painter& p) {
    const Look& look = this->look();
    int w = width(), h = height();
    ui::style::box(p, rect(), *look.self);
    if (!audio_ || audio_->frames() == 0 || w < 2) {
        p.drawText(RectF(rect()), AlignCenter, "Click a sample to see its waveform", look.font, look.text);
        return;
    }
    const std::vector<Lane>& ls = lanes(w);
    int head = look.captionH;
    double laneH = (h - head) / (double)ls.size();
    for (size_t i = 0; i < ls.size(); ++i) {
        double top = head + i * laneH;
        double middle = top + laneH / 2;
        p.fillRect(RectF(0, (float)middle, (float)w, (float)look.midlineH), look.midline);
        double half = laneH / 2 - 1;
        for (int x = 0; x < w; ++x) {
            double y0 = middle - std::min(1.0, (double)ls[i].maxs[x]) * half;
            double y1 = middle - std::max(-1.0, (double)ls[i].mins[x]) * half;
            int a = (int)y0, b = (int)std::max(y1, y0 + 1);
            p.drawVLine(x, a, b + 1, look.wave);  // (a cosmetic pen's line covers both ends)
        }
    }
    const Font& font = look.captionFont;
    std::string time = fmt::time(seconds_, 2);
    float timeW = font.width(time);
    float textW = (float)(w - look.padLeft - look.padRight);
    p.drawText(RectF((float)look.padLeft, 0, textW, (float)head), AlignRight | AlignVCenter, time, font, look.caption);
    p.drawText(RectF((float)look.padLeft, 0, std::max(0.f, textW - timeW - look.gap), (float)head),
               AlignLeft | AlignVCenter, name_, font, look.caption, true);
    if (position_) {
        int x = (int)(*position_ * (w - 1));
        p.fillRect(Rect(x, head, look.positionW, h + 1 - head), look.position);
    }
}

bool SampleWaveform::mousePress(ui::MouseEvent& e) {
    if (e.button == plat::LeftButton && audio_) {
        clicked.emit(std::min(1.0, std::max(0.0, (double)e.pos.x / std::max(1, width()))));
        return true;
    }
    return false;
}
