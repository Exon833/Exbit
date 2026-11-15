// The master scope (see scope_view.h).
#include "ui/scope_view.h"

#include <algorithm>
#include <cmath>

#include "audio/engine.h"
#include "audio/metering.h"
#include "config.h"
#include "core/util.h"
#include "gfx/painter.h"
#include "ui/theme.h"
#include "ui/toolkit/menu.h"

namespace {

constexpr int GAP = 3;
const int CAPTURES_MS[] = {5, 10, 20, 50, 100, 200, 500, 1000, 2000};
const int CYCLES[] = {1, 2, 4, 8};
const int FFT_SIZES[] = {1024, 2048, 4096, 8192, 16384};
const scope::Window WINDOWS[] = {scope::Window::Hann, scope::Window::Hamming, scope::Window::BlackmanHarris,
                                 scope::Window::Rectangular};
struct Fall {
    const char* name;
    double dbPerSecond;
};
const Fall FALLS[] = {{"Off (at once)", 0}, {"Fast", 96}, {"Medium", 48}, {"Slow", 18}};
const double SLOPES[] = {0, 3, 4.5};
const int FLOORS[] = {-60, -90, -120};

template <class T, size_t N>
bool among(const T (&list)[N], T v) {
    return std::find(std::begin(list), std::end(list), v) != std::end(list);
}

std::string captureName(int ms) { return ms >= 1000 ? util::format("%g s", ms / 1000.0) : std::to_string(ms) + " ms"; }

// the samples a picture of these settings needs (the newest)
size_t needed(const ScopeSettings& s, int sr) {
    size_t capture = (size_t)std::lround(s.captureMs / 1000.0 * sr);
    size_t pitch = (size_t)std::ceil(sr / 30.0) * 2 + 1024 + 4;  // (scope::detectPitch's longest look)
    size_t follow = s.pitchFollow ? (size_t)std::ceil(sr / 30.0) * (size_t)(s.cycles + 1) + pitch : 0;
    size_t want = s.spectrum ? (size_t)s.fft.fftSize : std::max({capture, follow, s.pitchFollow ? pitch : 0});
    return std::min(want, (size_t)(audio::Metering::SCOPE_SECONDS * sr));
}

}  // namespace

Json ScopeSettings::toJson() const {
    Json j = Json::object();
    j["shown"] = Json(shown);
    j["mode"] = Json(std::string(spectrum ? "spectrum" : "waveform"));
    j["channels"] = Json(std::string(separate ? "separate" : "combined"));
    j["capture_ms"] = Json(captureMs);
    j["pitch_follow"] = Json(pitchFollow);
    j["cycles"] = Json(cycles);
    j["fft_size"] = Json(fft.fftSize);
    j["window"] = Json(std::string(scope::windowName(fft.window)));
    j["fall"] = Json(fft.fall);
    j["slope"] = Json(fft.slope);
    j["floor"] = Json(fft.floorDb);
    return j;
}

ScopeSettings ScopeSettings::fromJson(const Json& j) {
    ScopeSettings s;
    if (!j.isObject()) return s;
    s.shown = j["shown"].toBool(s.shown);
    s.spectrum = j["mode"].toString("waveform") == "spectrum";
    s.separate = j["channels"].toString("combined") == "separate";
    int capture = j["capture_ms"].toInt(s.captureMs);
    if (among(CAPTURES_MS, capture)) s.captureMs = capture;
    s.pitchFollow = j["pitch_follow"].toBool(s.pitchFollow);
    int cycles = j["cycles"].toInt(s.cycles);
    if (among(CYCLES, cycles)) s.cycles = cycles;
    int size = j["fft_size"].toInt(s.fft.fftSize);
    if (among(FFT_SIZES, size)) s.fft.fftSize = size;
    std::string window = j["window"].toString("");
    for (auto w : WINDOWS)
        if (window == scope::windowName(w)) s.fft.window = w;
    double fall = j["fall"].toDouble(s.fft.fall);
    for (auto& f : FALLS)
        if (f.dbPerSecond == fall) s.fft.fall = fall;
    double slope = j["slope"].toDouble(s.fft.slope);
    if (among(SLOPES, slope)) s.fft.slope = slope;
    int floor = j["floor"].toInt((int)s.fft.floorDb);
    if (among(FLOORS, floor)) s.fft.floorDb = floor;
    return s;
}

ScopeView::ScopeView(audio::Engine& engine) : engine_(engine) {
    setFixedHeight(config::SCOPE_H);
    hPolicy = ui::SizePolicy::Expanding;
    setToolTip("What the master plays. Right-click: waveform or spectrum, channels, and their options");
    frameId_ = ui::App::get().onFrame([this] { frame(); });
}

ScopeView::~ScopeView() {
    if (frameId_ >= 0) ui::App::get().removeFrame(frameId_);
    if (auto m = engine_.metering) m->scopeOn = false;
}

void ScopeView::frame() {
    auto metering = engine_.metering;  // (a device opened since: a new one)
    if (!metering) return;
    bool on = settings.shown && isShown();
    metering->scopeOn = on;  // (the audio thread copies the master for it only while shown)
    if (!on) {
        wasOn_ = false;
        return;
    }
    if (!wasOn_) {  // (shown again: what is kept from before is old)
        metering->clearScope();
        wasOn_ = true;
        lastHeard_ = 0;
        lastTime_ = -1;
    }
    sr_ = metering->sr;
    uint64_t heard = metering->scopeLatest(needed(settings, sr_), left_, right_);
    if (heard == lastHeard_ && lastTime_ >= 0) return;  // (nothing new: the picture stands)
    lastHeard_ = heard;
    double now = ui::App::get().now();
    double dt = lastTime_ < 0 ? 0 : now - lastTime_;
    lastTime_ = now;
    analyse(left_, right_, sr_, dt);
    update();
}

void ScopeView::showAudio(const std::vector<float>& left, const std::vector<float>& right, int sr, double dt) {
    sr_ = sr;
    analyse(left, right, sr, dt);
    update();
}

void ScopeView::analyse(const std::vector<float>& left, const std::vector<float>& right, int sr, double dt) {
    int w = std::max(1, width());
    if (settings.separate) {
        analysePane(panes_[0], left, sr, dt, w / 2);
        analysePane(panes_[1], right, sr, dt, w / 2);
        return;
    }
    mixed_.resize(std::min(left.size(), right.size()));
    for (size_t i = 0; i < mixed_.size(); i++) mixed_[i] = 0.5f * (left[i] + right[i]);
    analysePane(panes_[0], mixed_, sr, dt, w);
}

void ScopeView::analysePane(Pane& pane, const std::vector<float>& samples, int sr, double dt, int width) {
    if (settings.spectrum) {
        pane.spectrum.analyse(samples.data(), samples.size(), sr, settings.fft, std::max(8, width / 2), dt);
        return;
    }
    size_t n = samples.size();
    size_t length = std::min(n, (size_t)std::lround(settings.captureMs / 1000.0 * sr));
    size_t start = n - length;
    pane.pitch = settings.pitchFollow ? scope::detectPitch(samples.data(), n, sr) : scope::Pitch{};
    if (pane.pitch.hz > 0) {  // (whole cycles, standing still)
        length = std::min(n, (size_t)std::lround(settings.cycles * pane.pitch.period));
        start = scope::followStart(samples.data(), n, length, pane.pitch.period, pane.previous);
    }
    pane.shown.assign(samples.begin() + (long)start, samples.begin() + (long)(start + length));
    pane.previous = pane.shown;
}

std::string ScopeView::label(int channel) const {
    const Pane& p = panes_[std::clamp(channel, 0, 1)];
    if (settings.spectrum) return "FFT " + std::to_string(settings.fft.fftSize);
    if (p.pitch.hz > 0) return scope::noteName(p.pitch.hz) + util::format(" %.0f Hz", p.pitch.hz);
    return captureName(settings.captureMs);
}

double ScopeView::pitchHz(int channel) const { return panes_[std::clamp(channel, 0, 1)].pitch.hz; }

void ScopeView::paint(Painter& p) {
    Rect box(0, GAP, width(), height() - GAP);  // (a gap above it: the controls' row is SCOPE_H shorter without it)
    p.fillRect(box, theme::c("meter_bg"));
    p.drawRect(box, theme::c("frame_edge"));
    RectF all(1, box.y + 1, width() - 2, box.h - 2);
    if (settings.separate) {
        float half = (all.w - 3) / 2;
        paintPane(p, panes_[0], RectF(all.x, all.y, half, all.h), "L");
        p.drawVLine((int)(all.x + half + 1), box.y + 1, box.y + box.h - 1, theme::c("frame_edge"));
        paintPane(p, panes_[1], RectF(all.x + half + 3, all.y, half, all.h), "R");
    } else {
        paintPane(p, panes_[0], all, "");
    }
}

void ScopeView::paintPane(Painter& p, const Pane& pane, const RectF& r, const std::string& name) {
    Color line = theme::c("accent"), grid = theme::c("grid"), text = theme::c("dimmed_text");
    p.save();
    p.setClip(Rect((int)r.x, (int)r.y, (int)std::ceil(r.w), (int)std::ceil(r.h)));
    if (settings.spectrum) {
        double floor = settings.fft.floorDb;
        auto yOf = [&](double db) { return (float)(r.y + r.h * (std::min(0.0, db) / floor)); };
        for (double db = -20; db > floor; db -= 20) p.drawHLine((int)r.x, (int)(r.x + r.w), (int)yOf(db), grid);
        const auto& levels = pane.spectrum.levels();
        if (!levels.empty()) {
            double lo = std::log(pane.spectrum.bandHz(0)), hi = std::log(pane.spectrum.bandHz((int)levels.size() - 1));
            for (double hz : {100.0, 1000.0, 10000.0}) {
                float x = (float)(r.x + r.w * (std::log(hz) - lo) / std::max(hi - lo, 1e-9));
                if (x > r.x && x < r.x + r.w) p.drawVLine((int)x, (int)r.y, (int)(r.y + r.h), grid);
            }
            std::vector<PointF> top;
            for (size_t i = 0; i < levels.size(); i++)
                top.push_back({(float)(r.x + r.w * (i + 0.5) / levels.size()), yOf(levels[i])});
            std::vector<PointF> fill = top;
            fill.push_back({r.x + r.w, r.y + r.h});
            fill.push_back({r.x, r.y + r.h});
            fill.insert(fill.begin(), PointF{r.x, top.front().y});
            p.fillPolygon(fill, line.withAlpha(70));
            p.drawPolyline(top, line, 1.2f);
        }
    } else {
        float mid = r.y + r.h / 2, half = r.h / 2 - 2;
        p.drawHLine((int)r.x, (int)(r.x + r.w), (int)mid, grid);
        const auto& s = pane.shown;
        int cols = std::max(1, (int)r.w);
        if (s.size() > (size_t)cols) {  // (more samples than columns: each column's lowest to highest)
            std::vector<scope::Column> c;
            scope::columns(s.data(), s.size(), cols, c);
            for (int i = 0; i < cols; i++) {
                float y0 = mid - std::clamp(c[(size_t)i].hi, -1.0f, 1.0f) * half;
                float y1 = mid - std::clamp(c[(size_t)i].lo, -1.0f, 1.0f) * half;
                p.fillRect(RectF(r.x + i, y0, 1, std::max(1.0f, y1 - y0)), line);
            }
        } else if (s.size() >= 2) {  // (fewer: a line through them)
            std::vector<PointF> pts;
            for (size_t i = 0; i < s.size(); i++)
                pts.push_back({(float)(r.x + r.w * i / (s.size() - 1)), mid - std::clamp(s[i], -1.0f, 1.0f) * half});
            p.drawPolyline(pts, line, 1.2f);
        }
    }
    std::string caption = name.empty() ? "" : name + "  ";
    caption += settings.spectrum ? "" : (pane.pitch.hz > 0 ? scope::noteName(pane.pitch.hz) +
                                                                 util::format(" %.0f Hz", pane.pitch.hz)
                                                           : captureName(settings.captureMs));
    if (!caption.empty())
        p.drawText(RectF(r.x + 4, r.y + 1, r.w - 8, 12), AlignLeft | AlignTop, caption, theme::monoFont(7), text);
    p.restore();
}

void ScopeView::changed() {
    for (auto& pane : panes_) {
        pane.spectrum.reset();
        pane.previous.clear();
        pane.shown.clear();
        pane.pitch = {};
    }
    lastHeard_ = 0;
    lastTime_ = -1;
    update();
    if (settingsChanged) settingsChanged();
}

void ScopeView::fillMenu(ui::Menu& menu) {
    auto radio = [&](ui::Menu& m, const std::string& text, bool on, int group, std::function<void()> fn) {
        ui::Action* a = m.addCheckable(text, on, [this, fn](bool) {
            fn();
            changed();
        });
        a->group = group;
    };
    radio(menu, "&Waveform", !settings.spectrum, 1, [this] { settings.spectrum = false; });
    radio(menu, "&Spectrum", settings.spectrum, 1, [this] { settings.spectrum = true; });
    menu.addSeparator();
    radio(menu, "&Combined channels", !settings.separate, 2, [this] { settings.separate = false; });
    radio(menu, "Separate channels (left and right side by side)", settings.separate, 2,
          [this] { settings.separate = true; });
    menu.addSection("Waveform");
    ui::Menu* capture = menu.addMenu("Capture &window");
    for (int ms : CAPTURES_MS)
        radio(*capture, captureName(ms), settings.captureMs == ms, 3, [this, ms] { settings.captureMs = ms; });
    menu.addCheckable("&Pitch follow (whole cycles of the note, standing still)", settings.pitchFollow, [this](bool on) {
        settings.pitchFollow = on;
        changed();
    });
    ui::Menu* cycles = menu.addMenu("C&ycles shown (pitch follow)");
    for (int c : CYCLES)
        radio(*cycles, std::to_string(c), settings.cycles == c, 4, [this, c] { settings.cycles = c; });
    menu.addSection("Spectrum");
    ui::Menu* size = menu.addMenu("&FFT size");
    for (int n : FFT_SIZES)
        radio(*size, std::to_string(n), settings.fft.fftSize == n, 5, [this, n] { settings.fft.fftSize = n; });
    ui::Menu* window = menu.addMenu("Window &function");
    for (auto w : WINDOWS)
        radio(*window, scope::windowName(w), settings.fft.window == w, 6, [this, w] { settings.fft.window = w; });
    ui::Menu* fall = menu.addMenu("Fall &back");
    for (auto& f : FALLS)
        radio(*fall, f.dbPerSecond > 0 ? util::format("%s (%g dB/s)", f.name, f.dbPerSecond) : std::string(f.name),
              settings.fft.fall == f.dbPerSecond, 7, [this, d = f.dbPerSecond] { settings.fft.fall = d; });
    ui::Menu* slope = menu.addMenu("S&lope");
    for (double s : SLOPES)
        radio(*slope, util::format("%g dB per octave", s), settings.fft.slope == s, 8, [this, s] { settings.fft.slope = s; });
    ui::Menu* range = menu.addMenu("&Range");
    for (int f : FLOORS)
        radio(*range, util::format("%d dB", -f), (int)settings.fft.floorDb == f, 9, [this, f] { settings.fft.floorDb = f; });
    menu.addSeparator();
    menu.addAction("&Hide the scope", [this] {
        if (hideRequested) hideRequested();
    });
}

bool ScopeView::contextMenu(ui::MouseEvent& e) {
    ui::Menu menu;
    fillMenu(menu);
    menu.exec(e.globalPos, this);
    return true;
}
