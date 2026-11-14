#include "ui/sample_waveform.h"

#include <algorithm>
#include <cmath>

#include "ui/formatting.h"
#include "ui/theme.h"

SampleWaveform::SampleWaveform(ui::Widget* parent) : ui::Widget(parent) {
    setFixedHeight(HEIGHT);
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

void SampleWaveform::paint(Painter& p) {
    int w = width(), h = height();
    Font font = theme::uiFont();
    p.fillRect(rect(), theme::c("meter_bg"));
    if (!audio_ || audio_->frames() == 0 || w < 2) {
        p.drawText(RectF(rect()), AlignCenter, "Click a sample to see its waveform", font, theme::c("ruler_text"));
        return;
    }
    const std::vector<Lane>& ls = lanes(w);
    double laneH = (h - 14) / (double)ls.size();
    Color wave = theme::c("sample_wave"), grid = theme::c("grid");
    for (size_t i = 0; i < ls.size(); ++i) {
        double top = 14 + i * laneH;
        double middle = top + laneH / 2;
        p.fillRect(RectF(0, (float)middle, (float)w, 1), grid);
        double half = laneH / 2 - 1;
        for (int x = 0; x < w; ++x) {
            double y0 = middle - std::min(1.0, (double)ls[i].maxs[x]) * half;
            double y1 = middle - std::max(-1.0, (double)ls[i].mins[x]) * half;
            int a = (int)y0, b = (int)std::max(y1, y0 + 1);
            p.drawVLine(x, a, b + 1, wave);  // (a cosmetic pen's line covers both ends)
        }
    }
    Color text = theme::c("ruler_text");
    std::string time = fmt::time(seconds_, 2);
    float timeW = font.width(time);
    p.drawText(RectF(4, 0, (float)w - 8, 14), AlignRight | AlignVCenter, time, font, text);
    p.drawText(RectF(4, 0, std::max(0.f, (float)w - 8 - timeW - 6), 14), AlignLeft | AlignVCenter, name_, font, text,
               true);
    if (position_) {
        int x = (int)(*position_ * (w - 1));
        p.drawVLine(x, 14, h + 1, theme::c("playhead"));
    }
}

bool SampleWaveform::mousePress(ui::MouseEvent& e) {
    if (e.button == plat::LeftButton && audio_) {
        clicked.emit(std::min(1.0, std::max(0.0, (double)e.pos.x / std::max(1, width()))));
        return true;
    }
    return false;
}
