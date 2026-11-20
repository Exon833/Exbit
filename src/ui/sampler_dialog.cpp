// The built-in sampler's window (sampler_dialog.py).
#include "ui/sampler_dialog.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

#include "audio/file_io.h"
#include "audio/sampler.h"
#include "audio/warp.h"
#include "effects/svf.h"
#include "core/util.h"
#include "ui/theme.h"
#include "ui/toolkit/layout.h"

namespace sampler_dialog {

namespace {
Host gHost;
std::map<const Track*, SamplerDialog*> gWindows;  // one per track

std::string fixed(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}
}  // namespace

void setHost(const Host& host) { gHost = host; }
const Host& host() { return gHost; }

void renderTrackClips(const TrackPtr& track) {
    if (!gHost.project || !gHost.engine || !track) return;
    int sr = gHost.engine->sr;
    for (const ClipPtr& clip : gHost.project->clipsOn(track)) clip->prepare(sr);
    for (const ClipPtr& take : track->takes)
        if (take->kind == Kind::Midi) take->prepare(sr);
    if (gHost.relayout) gHost.relayout();
}

std::string noteName(int pitch) { return sampler::noteName(pitch); }
int rootFromName(const std::string& name) { return sampler::rootFromName(name); }

ClipPtr chooseSample(ui::Window* parent, const std::string& startDir) {
    std::string patterns;
    for (const std::string& e : file_io::AUDIO_EXTENSIONS) patterns += (patterns.empty() ? "*" : " *") + e;
    std::string path = ui::files::openFile(parent, "Sample for the built-in sampler", startDir,
                                           "Audio files (" + patterns + ")");
    if (path.empty()) return nullptr;
    AudioPtr data;
    int sr = 0;
    std::string error;
    if (!file_io::loadAudio(path, data, sr, error)) {
        ui::msg::warning(parent, "Cannot load file", path + "\n\n" + error);
        return nullptr;
    }
    auto clip = std::make_shared<Clip>();
    clip->kind = Kind::Audio;
    clip->name = util::baseName(path);
    clip->source = data;
    clip->sourceSr = sr;
    return clip;
}

SamplerPtr newSettings(const ClipPtr& sample) {
    auto s = std::make_shared<SamplerSettings>();
    s->sample = sample;
    s->root = rootFromName(util::stem(sample->name));
    return s;
}

SamplerWaveform::SamplerWaveform(ui::Widget* parent) : SampleWaveform(parent) { setStyleType("SamplerWaveform"); }

void SamplerWaveform::paint(Painter& p) {
    SampleWaveform::paint(p);
    if (!audio() || audio()->frames() == 0) return;
    if (ownParts_.refresh()) {
        Look l;
        l.outside = &ownParts_.get("outside");
        const css::Style& marker = ownParts_.get("marker");
        l.marker = panel_style::bg(marker);
        l.markerW = std::max(1, panel_style::px(marker, "width", 1));
        l.top = std::max(0, panel_style::px(ownParts_.get("caption"), "height", 14));
        look_ = l;
    }
    int w = width(), h = height(), top = look_.top, mw = look_.markerW;
    auto xOf = [&](double f) { return (int)std::lround(std::min(1.0, std::max(0.0, f)) * w); };
    int a = xOf(start), b = xOf(end);
    if (a > 0) ui::style::box(p, Rect(0, top, a, h - top), *look_.outside);
    if (b < w) ui::style::box(p, Rect(b, top, w - b, h - top), *look_.outside);
    p.fillRect(Rect(std::max(0, std::min(w - mw, a)), top, mw, h - top), look_.marker);
    p.fillRect(Rect(std::max(0, std::min(w - mw, b - mw)), top, mw, h - top), look_.marker);
    for (double f : slices)  // (the slices: a line from half way down)
        p.fillRect(Rect(std::max(0, std::min(w - mw, xOf(f))), top + (h - top) / 2, mw, (h - top) - (h - top) / 2),
                   look_.marker);
    if (loopStart >= 0) {  // (the loop's start: a dashed line)
        int x = std::max(0, std::min(w - mw, xOf(loopStart)));
        for (int y = top; y < h; y += 6) p.fillRect(Rect(x, y, mw, std::min(3, h - y)), look_.marker);
    }
    if (handles) {  // (grips at the top of the handles)
        for (int x : {a, b - 6}) p.fillRect(Rect(std::max(0, std::min(w - 6, x)), top, 6, 4), look_.marker);
        if (loopStart >= 0) p.fillRect(Rect(std::max(0, std::min(w - 6, xOf(loopStart) - 3)), h - 4, 6, 4), look_.marker);
    }
}

bool SamplerWaveform::mousePress(ui::MouseEvent& e) {
    if (!handles || e.button != plat::LeftButton || !audio()) return SampleWaveform::mousePress(e);
    int w = std::max(1, width());
    double f = (double)e.pos.x / w;
    // (the nearest handle: start, end, the loop's start)
    double gaps[3] = {std::fabs(f - start), std::fabs(f - end), loopStart >= 0 ? std::fabs(f - loopStart) : 1e9};
    dragging_ = (int)(std::min_element(gaps, gaps + 3) - gaps);
    return mouseMove(e);
}

bool SamplerWaveform::mouseMove(ui::MouseEvent& e) {
    if (dragging_ < 0) return false;
    double f = std::min(1.0, std::max(0.0, (double)e.pos.x / std::max(1, width())));
    const double GAP = 0.001;  // (start and end kept apart, as the boxes keep them)
    if (dragging_ == 0) start = std::min(f, end - GAP);
    else if (dragging_ == 1) end = std::max(f, start + GAP);
    else loopStart = std::min(std::max(f, start), end - GAP);
    if (loopStart >= 0) loopStart = std::min(std::max(loopStart, start), end - GAP);
    update();
    if (moved) moved();
    return true;
}

bool SamplerWaveform::mouseRelease(ui::MouseEvent& e) {
    if (dragging_ < 0) return false;
    dragging_ = -1;
    return true;
}

bool SamplerWaveform::textCells(int cols, int rows, std::vector<ui::TextCell>& cells) const {
    cells.assign((size_t)std::max(0, cols * rows), ui::TextCell());
    if (cols <= 0 || rows <= 0) return true;
    AudioPtr a = audio();
    int64_t n = a ? a->frames() : 0;
    if (n <= 0) {
        const char* none = "(no sample)";
        for (int c = 0; none[c] && c < cols; c++) cells[(size_t)c].ch = (char32_t)none[c];
        return true;
    }
    static const char32_t BLOCKS[] = {U' ', U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█'};
    float top = 0;
    std::vector<float> peak((size_t)cols, 0.0f);
    for (int c = 0; c < cols; c++) {
        int64_t i0 = n * c / cols, i1 = std::max(i0 + 1, n * (c + 1) / cols);
        for (int64_t i = i0; i < i1 && i < n; i++)
            peak[(size_t)c] = std::max({peak[(size_t)c], std::fabs(a->samples[(size_t)i * 2]), std::fabs(a->samples[(size_t)i * 2 + 1])});
        top = std::max(top, peak[(size_t)c]);
    }
    auto colOf = [&](double f) { return std::min(cols - 1, std::max(0, (int)(std::min(1.0, std::max(0.0, f)) * cols))); };
    int a0 = colOf(start), b0 = colOf(end - 1e-9);
    for (int c = 0; c < cols; c++) {
        ui::TextCell& cell = cells[(size_t)c];
        cell.ch = BLOCKS[top > 0 ? std::min(8, (int)std::ceil(peak[(size_t)c] / top * 8)) : 0];
        if (c < a0 || c > b0) cell.fg = 4;  // (outside the part played: blue, dim)
    }
    for (double f : slices) cells[(size_t)colOf(f)] = ui::TextCell{U'\'', 3, 9, 1};
    if (loopStart >= 0) cells[(size_t)colOf(loopStart)] = ui::TextCell{U'|', 3, 9, 1};
    cells[(size_t)a0] = ui::TextCell{U'[', 9, 9, 1};
    cells[(size_t)b0] = ui::TextCell{U']', 9, 9, 1};
    return true;
}

// =================================================================================================

ui::SpinBox* SoundControls::number(ui::Widget* owner, double low, double high, int decimals, const std::string& suffix,
                                   const std::string& tip) {
    ui::SpinBox* box = owner->add<ui::SpinBox>();
    box->setRange(low, high);
    box->setDecimals(decimals);
    box->setSuffix(suffix);
    if (!tip.empty()) box->setToolTip(tip);
    box->setKeyboardTracking(false);  // (changed once a value is typed in, not on every key)
    return box;
}

namespace {
const char* const FILTERS[] = {"Off", "Low-pass 12 dB", "Low-pass 24 dB", "High-pass 12 dB", "High-pass 24 dB",
                               "Band-pass", "Notch"};
int filterIndex(const SamplerSettings& s) {
    switch (s.filter) {
        case SamplerSettings::LowPass: return s.steep ? 2 : 1;
        case SamplerSettings::HighPass: return s.steep ? 4 : 3;
        case SamplerSettings::BandPass: return 5;
        case SamplerSettings::Notch: return 6;
        default: return 0;
    }
}
void setFilter(SamplerSettings& s, int index) {
    static const int TYPES[] = {SamplerSettings::NoFilter, SamplerSettings::LowPass,  SamplerSettings::LowPass,
                                SamplerSettings::HighPass, SamplerSettings::HighPass, SamplerSettings::BandPass,
                                SamplerSettings::Notch};
    index = std::min(6, std::max(0, index));
    s.filter = TYPES[index];
    s.steep = index == 2 || index == 4;
}
const int SLICE_GRIDS[] = {0, 4, 8, 16, 32};  // (the slicing's choices: transients, then 1/4 … 1/32)
}  // namespace

void SoundControls::row(ui::FormLayout* form, const std::string& label, ui::Widget* w, const std::string& name) {
    if (label.empty()) form->addRow(w);
    else form->addRow(label, w);
    rows.push_back({name.empty() ? label : name, w, form});
}

void SoundControls::build(ui::Widget* owner, ui::FormLayout* form, bool withRoot, const std::string& transposeLabel) {
    build(owner, Forms{form, form, form}, withRoot, transposeLabel);
}

void SoundControls::build(ui::Widget* owner, const Forms& forms, bool withRoot, const std::string& transposeLabel) {
    ui::FormLayout* form = forms.sound;
    mode = owner->add<ui::ComboBox>();
    mode->addItem("Classic: plays while the note is held");
    mode->addItem("One-shot: plays to the end of the sample");
    if (withRoot) {
        mode->addItem("Slice: a slice on each key");
        mode->setToolTip("Classic: the keys play it higher and lower, looping while held if you like. One-shot: "
                         "every note plays it to its end. Slice: cut at its hits (or a grid), a slice on each key "
                         "from the root key up");
    }
    row(form, "Mode", mode);
    if (withRoot) {
        root = owner->add<ui::SpinBox>();
        root->setRange(0, 127);
        root->setDecimals(0);
        root->textFromValue = [](double v) { int k = (int)std::lround(v); return noteName(k) + " (" + std::to_string(k) + ")"; };
        root->valueFromText = [](const std::string& text, double& v) {
            size_t open = text.rfind('(');
            std::string digits = open == std::string::npos ? text : text.substr(open + 1);
            try {
                v = std::stod(digits);
                return true;
            } catch (...) {
                return false;
            }
        };
        root->setToolTip("The key that plays the sample as it was recorded (60 = C4); in Slice mode, the first slice's");
        row(form, "Root key", root);
    }
    transpose = number(owner, -48, 48, 2, " semitones", "Tune it up or down (fractions for fine tuning)");
    row(form, transposeLabel, transpose);
    gain = number(owner, -60, 24, 1, " dB");
    row(form, "Volume", gain);
    velocity = number(owner, 0, 100, 0, " %", "How much softer quiet notes are: 0 % not at all, 100 % fully");
    row(form, "Velocity", velocity);
    start = number(owner, 0, 99.9, 1, " %", "Where in the sample it starts");
    row(form, "Start", start);
    end = number(owner, 0.1, 100, 1, " %", "Where in the sample it ends");
    row(form, "End", end);
    loop = owner->add<ui::CheckBox>("Loop while the note is held (classic)");
    row(form, "", loop, "Loop");
    attack = number(owner, 0, 5000, 0, " ms");
    row(forms.envelope, "Attack", attack);
    decay = number(owner, 0, 20000, 0, " ms", "How long it takes from full level to the sustain level");
    row(forms.envelope, "Decay", decay);
    sustain = number(owner, 0, 100, 0, " %", "The level held while the note is");
    row(forms.envelope, "Sustain", sustain);
    release = number(owner, 0, 10000, 0, " ms", "How long it fades out once the note ends");
    row(forms.envelope, "Release", release);
    filter = owner->add<ui::ComboBox>();
    for (const char* f : FILTERS) filter->addItem(f);
    filter->setToolTip("A filter on each note: low-pass (darker), high-pass (thinner), band-pass or notch");
    row(forms.filter, "Filter", filter);
    cutoff = number(owner, 20, 20000, 0, " Hz", "Where the filter starts cutting");
    cutoff->setSingleStep(25);
    row(forms.filter, "Cutoff", cutoff);
    resonance = number(owner, 0, 100, 0, " %", "How much the filter rings at its cutoff");
    row(forms.filter, "Resonance", resonance);
}

void SoundControls::show(const SamplerSettings& s) {
    const std::pair<ui::SpinBox*, double> values[] = {
        {root, (double)s.root}, {transpose, s.transpose}, {gain, s.gainDb}, {velocity, s.velocity * 100},
        {attack, s.attack * 1000}, {decay, s.decay * 1000}, {sustain, s.sustain * 100}, {release, s.release * 1000},
        {start, s.start * 100}, {end, s.end * 100}, {cutoff, s.cutoff}, {resonance, s.resonance * 100}};
    for (auto& bv : values)
        if (bv.first) bv.first->setValueSilently(bv.second);
    mode->setCurrentIndexSilently(s.slice && mode->count() > 2 ? 2 : s.oneShot ? 1 : 0);
    loop->setCheckedSilently(s.loop);
    loop->setEnabled(!s.oneShot && !s.slice);
    filter->setCurrentIndexSilently(filterIndex(s));
}

void SoundControls::read(SamplerSettings& s) const {
    double a = start->value() / 100, b = end->value() / 100;
    if (root) s.root = root->intValue();
    s.transpose = transpose->value();
    s.gainDb = gain->value();
    s.velocity = velocity->value() / 100;
    s.attack = attack->value() / 1000;
    s.decay = decay->value() / 1000;
    s.sustain = sustain->value() / 100;
    s.release = release->value() / 1000;
    s.oneShot = mode->currentIndex() == 1;
    if (mode->count() > 2) s.slice = mode->currentIndex() == 2;
    s.loop = loop->isChecked();
    s.start = std::min(a, b - 0.001);
    s.end = std::max(b, a + 0.001);
    setFilter(s, filter->currentIndex());
    s.cutoff = cutoff->value();
    s.resonance = resonance->value() / 100;
}

void SoundControls::connect(std::function<void()> changed) {
    for (ui::SpinBox* box : {root, transpose, gain, velocity, attack, decay, sustain, release, start, end, cutoff, resonance})
        if (box) box->valueChanged.connect([changed](double) { changed(); });
    for (ui::ComboBox* box : {mode, filter}) box->currentIndexChanged.connect([changed](int) { changed(); });
    loop->toggled.connect([changed](bool) { changed(); });
}

// =================================================================================================

std::string ControlPage::valueText(ui::Widget* w) {
    if (auto* b = dynamic_cast<ui::SpinBox*>(w)) return b->text();
    if (auto* c = dynamic_cast<ui::ComboBox*>(w)) return c->currentText();
    if (auto* k = dynamic_cast<ui::CheckBox*>(w)) return k->isChecked() ? "[x]" : "[ ]";
    return "";
}

bool ControlPage::listRows(std::vector<std::string>& out, int& current) const {
    constexpr size_t NAME_W = 18;  // (the names' column, as the parameter window has it)
    const ui::Window* win = window();
    const ui::Widget* focus = win ? win->focusWidget() : nullptr;
    current = -1;
    for (size_t i = 0; i < rows.size(); i++) {
        std::string name = rows[i].first;
        if (name.size() < NAME_W) name += std::string(NAME_W - name.size(), ' ');
        std::string value = valueText(rows[i].second);
        out.push_back(name + " " + (rows[i].second->isEnabled() ? value : "(" + value + ")"));
        for (const ui::Widget* f = focus; f && current < 0; f = f->parent())
            if (f == rows[i].second) current = (int)i;
    }
    return true;
}

// =================================================================================================

SamplerDialog::SamplerDialog(ui::Window* window, TrackPtr track_)
    : ui::Dialog(window, "Built-in sampler \xe2\x80\x94 " + track_->name, 1000, 640), track(std::move(track_)) {
    deleteOnClose = true;
    setStyleType("SamplerDialog");
    ui::VBox* col = ui::styleRoom(ui::vbox(this, 9, 6), computedStyle());
    auto row = std::make_unique<ui::HBox>();
    sampleName = add<ui::Label>();
    sampleName->addStyleClass("heading");  // (default.css: Label.heading, bold)
    row->addWidget(sampleName, 1);
    ui::Button* chooseButton = ui::pushButton(this, "Choose\xe2\x80\xa6", [this] { choose(); });
    chooseButton->setToolTip("Play another sample (the settings stay)");
    row->addWidget(chooseButton);
    col->addLayout(std::move(row));
    waveform = add<SamplerWaveform>();
    waveform->setFixedHeight(theme::metric("sample-waveform-height", SampleWaveform::HEIGHT));
    waveform->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
    waveform->setCursor(ui::Cursor::SizeHor);
    waveform->handles = true;
    waveform->setToolTip("Drag the start, the end and (looping) the loop's start");
    col->addWidget(waveform);
    // the page: four columns, each a heading over a form (each placed in its parent before it is filled: a form's
    // labels need their owner)
    page = add<ControlPage>();
    col->addWidget(page, 1);
    ui::HBox* columns = ui::hbox(page, 0, 18);
    ui::FormLayout* forms[4];
    const char* headings[4] = {"Sample", "Envelope and glide", "Filter", "LFO and warp"};
    for (int i = 0; i < 4; i++) {
        auto made = std::make_unique<ui::VBox>();
        ui::VBox* column = made.get();
        columns->addLayout(std::move(made), 1);
        ui::Label* heading = page->add<ui::Label>(headings[i]);
        heading->addStyleClass("heading");
        column->addWidget(heading);
        auto form = std::make_unique<ui::FormLayout>();
        forms[i] = form.get();
        column->addLayout(std::move(form));
        column->addStretch(1);
    }
    controls.build(page, SoundControls::Forms{forms[0], forms[1], forms[2]}, true);
    auto number = [this](double lo, double hi, int decimals, const std::string& suffix, const std::string& tip) {
        return SoundControls::number(page, lo, hi, decimals, suffix, tip);
    };
    auto combo = [this](const std::vector<std::string>& items, const std::string& tip) {
        ui::ComboBox* c = page->add<ui::ComboBox>();
        for (const std::string& i : items) c->addItem(i);
        c->setToolTip(tip);
        return c;
    };
    auto check = [this](const std::string& text, const std::string& tip) {
        ui::CheckBox* c = page->add<ui::CheckBox>(text);
        c->setToolTip(tip);
        return c;
    };
    // the sample
    loopStart = number(0, 99.9, 1, " %", "Where the loop goes back to (from the start, at most the end)");
    controls.row(forms[0], "Loop start", loopStart);
    reverse = check("Reverse", "Play the sample backwards");
    controls.row(forms[0], "", reverse, "Reverse");
    normalise = check("Normalise", "Bring its loudest sample to full level (0 dB)");
    controls.row(forms[0], "", normalise, "Normalise");
    sliceGrid = combo({"At its hits", "1/4 notes", "1/8 notes", "1/16 notes", "1/32 notes"},
                      "Slice mode: where it is cut, at its hits (transients) or on a grid at its tempo (BPM)");
    controls.row(forms[0], "Slice", sliceGrid);
    sensitivity = number(0, 100, 0, " %", "Slice mode: how quiet a hit may be and still cut a slice");
    controls.row(forms[0], "Sensitivity", sensitivity);
    // the envelope (the controls' four) and glide
    glide = number(0, 5000, 0, " ms", "Each note slides to its pitch from the note before it this long (0: off)");
    controls.row(forms[1], "Glide", glide);
    voices = combo({"Poly", "Mono"}, "Mono: a note played while another sounds takes it over (one voice)");
    controls.row(forms[1], "Voices", voices);
    // the filter's envelope
    filterEnv = number(-8, 8, 2, " oct", "How far the filter's envelope moves the cutoff, in octaves");
    controls.row(forms[2], "Envelope amount", filterEnv);
    keyTrack = number(0, 100, 0, " %", "How far the cutoff follows the key (100 %: an octave higher an octave up)");
    controls.row(forms[2], "Key tracking", keyTrack);
    fAttack = number(0, 5000, 0, " ms", "");
    controls.row(forms[2], "Filter attack", fAttack);
    fDecay = number(0, 20000, 0, " ms", "");
    controls.row(forms[2], "Filter decay", fDecay);
    fSustain = number(0, 100, 0, " %", "");
    controls.row(forms[2], "Filter sustain", fSustain);
    fRelease = number(0, 10000, 0, " ms", "");
    controls.row(forms[2], "Filter release", fRelease);
    // the LFO
    lfoShape = combo(effects::LFO_SHAPES, "The LFO's wave (it starts again with each note)");
    controls.row(forms[3], "LFO shape", lfoShape);
    lfoTarget = combo({"Pitch", "Filter", "Volume", "Pan"}, "What the LFO moves");
    controls.row(forms[3], "LFO to", lfoTarget);
    lfoDepth = number(0, 100, 0, " %", "How far: pitch ±12 semitones, the filter ±4 octaves, the volume, the pan (0: off)");
    controls.row(forms[3], "LFO depth", lfoDepth);
    lfoSync = check("Sync to the song's tempo", "Its rate a length in beats, at the song's tempo");
    controls.row(forms[3], "", lfoSync, "LFO sync");
    lfoRate = number(0.01, 40, 2, " Hz", "");
    lfoRate->setSingleStep(0.1);
    controls.row(forms[3], "LFO rate", lfoRate);
    lfoBeats = combo(effects::SYNC_NAMES, "");
    controls.row(forms[3], "LFO rate (synced)", lfoBeats);
    // warp
    warp = check("Warp to the song's tempo", "The sample plays at the song's tempo, the keys changing its pitch only");
    controls.row(forms[3], "", warp, "Warp");
    bpm = number(20, 999, 2, " BPM", "The sample's own tempo");
    controls.row(forms[3], "Sample tempo", bpm);
    warpMode = combo({warp::modeName(warp::Mode::Beats), warp::modeName(warp::Mode::Tones), warp::modeName(warp::Mode::Complex)},
                     "Beats: drums, loops. Tones: an instrument, a voice. Complex: a whole mix");
    controls.row(forms[3], "Warp mode", warpMode);
    for (ui::FormLayout* form : forms)  // (the list: column by column)
        for (const SoundControls::Row& r : controls.rows)
            if (r.form == form) page->rows.emplace_back(r.name, r.control);
    for (auto& r : page->rows) r.second->raise();  // (Tab goes from row to row in the list's order)
    root = controls.root, transpose = controls.transpose, gain = controls.gain, velocity = controls.velocity;
    attack = controls.attack, decay = controls.decay, sustain = controls.sustain, release = controls.release;
    start = controls.start, end = controls.end, mode = controls.mode, loop = controls.loop;
    auto buttons = std::make_unique<ui::HBox>();
    ui::Button* play = ui::pushButton(this, "Play the root key", [this] { playRoot(); });
    play->setToolTip("Hear it on the track (also: the MIDI editor's keys, or a MIDI keyboard)");
    buttons->addWidget(play);
    buttons->addStretch(1);
    buttons->addWidget(ui::pushButton(this, "Close", [this] { close(); }));
    col->addLayout(std::move(buttons));
    if (track->sampler) showSettings(*track->sampler);
    controls.connect([this] { changed(); });
    for (ui::SpinBox* b : {loopStart, sensitivity, filterEnv, keyTrack, fAttack, fDecay, fSustain, fRelease, lfoDepth,
                           lfoRate, glide, bpm})
        b->valueChanged.connect([this](double) { changed(); });
    for (ui::ComboBox* c : {sliceGrid, lfoShape, lfoTarget, lfoBeats, voices, warpMode})
        c->currentIndexChanged.connect([this](int) { changed(); });
    for (ui::CheckBox* c : {reverse, normalise, lfoSync, warp}) c->toggled.connect([this](bool) { changed(); });
    waveform->moved = [this] {  // (a handle dragged: the boxes follow, and it is a change)
        start->setValueSilently(waveform->start * 100);
        end->setValueSilently(waveform->end * 100);
        if (waveform->loopStart >= 0) loopStart->setValueSilently(waveform->loopStart * 100);
        changed();
    };
}

SamplerDialog::~SamplerDialog() {
    if (renderTimer_ >= 0) ui::App::get().cancelTimer(renderTimer_);
    auto it = gWindows.find(track.get());
    if (it != gWindows.end() && it->second == this) gWindows.erase(it);
}

void SamplerDialog::enableControls(const SamplerSettings& s) {
    bool classic = !s.oneShot && !s.slice, filtered = s.filter != SamplerSettings::NoFilter;
    loop->setEnabled(classic);
    loopStart->setEnabled(classic && s.loop);
    sliceGrid->setEnabled(s.slice);
    sensitivity->setEnabled(s.slice && s.sliceGrid == 0);
    for (ui::Widget* w : std::initializer_list<ui::Widget*>{controls.cutoff, controls.resonance, filterEnv, keyTrack,
                                                            fAttack, fDecay, fSustain, fRelease})
        w->setEnabled(filtered);
    lfoRate->setEnabled(!s.lfoSync);
    lfoBeats->setEnabled(s.lfoSync);
    glide->setEnabled(!s.slice);
    voices->setEnabled(!s.slice);
    bpm->setEnabled(s.warp || (s.slice && s.sliceGrid > 0));
    warpMode->setEnabled(s.warp);
}

void SamplerDialog::showWaveform(const SamplerSettings& s) {
    // (as it plays: reversed, normalised; not warped, which would take a while)
    SamplerSettings shown = s;
    shown.warp = false;
    int sr = s.sample && s.sample->sourceSr > 0 ? s.sample->sourceSr : 48000;
    AudioPtr sound = s.sample ? sampler::soundAt(shown, sr) : nullptr;
    waveform->setAudio(sound, s.sample ? s.sample->name : std::string(), sr);
    waveform->start = s.start;
    waveform->end = s.end;
    waveform->loopStart = s.loop && !s.oneShot && !s.slice ? std::max(s.start, s.loopStart) : -1;
    waveform->slices.clear();
    if (s.slice && sound && sound->frames() > 0) {
        int engineSr = gHost.engine ? gHost.engine->sr : sr;
        AudioPtr cut = sampler::soundAt(s, engineSr, s.transpose);
        double frames = cut ? (double)std::max<int64_t>(1, cut->frames()) : 1.0;
        for (int64_t f : sampler::sliceStarts(s, engineSr)) waveform->slices.push_back((double)f / frames);
    }
    waveform->update();
}

void SamplerDialog::showSettings(const SamplerSettings& s) {
    double seconds = s.sample && s.sample->source && s.sample->sourceSr
                         ? (double)s.sample->source->frames() / s.sample->sourceSr : 0.0;
    std::string name = s.sample ? s.sample->name : std::string();
    sampleName->setText(name + "  (" + fixed(seconds, 2) + " s)");
    controls.show(s);
    const std::pair<ui::SpinBox*, double> values[] = {
        {loopStart, std::max(s.start, s.loopStart) * 100}, {sensitivity, s.sensitivity * 100}, {filterEnv, s.filterEnv},
        {keyTrack, s.keyTrack * 100}, {fAttack, s.fAttack * 1000}, {fDecay, s.fDecay * 1000},
        {fSustain, s.fSustain * 100}, {fRelease, s.fRelease * 1000}, {lfoDepth, s.lfoDepth * 100},
        {lfoRate, s.lfoRate}, {glide, s.glide * 1000}, {bpm, s.bpm}};
    for (auto& bv : values) bv.first->setValueSilently(bv.second);
    int grid = 0;
    for (int i = 0; i < 5; i++)
        if (SLICE_GRIDS[i] == s.sliceGrid) grid = i;
    sliceGrid->setCurrentIndexSilently(grid);
    lfoShape->setCurrentIndexSilently(std::min((int)effects::LFO_SHAPES.size() - 1, std::max(0, s.lfoShape)));
    lfoTarget->setCurrentIndexSilently(std::min(3, std::max(0, s.lfoTarget)));
    lfoBeats->setCurrentIndexSilently(std::min(effects::SYNC_COUNT - 1, std::max(0, s.lfoBeats)));
    voices->setCurrentIndexSilently(s.mono ? 1 : 0);
    warpMode->setCurrentIndexSilently(std::min(2, std::max(0, s.warpMode)));
    reverse->setCheckedSilently(s.reverse);
    normalise->setCheckedSilently(s.normalise);
    lfoSync->setCheckedSilently(s.lfoSync);
    warp->setCheckedSilently(s.warp);
    enableControls(s);
    showWaveform(s);
}

SamplerPtr SamplerDialog::settings() const {
    auto s = std::make_shared<SamplerSettings>(track->sampler ? *track->sampler : SamplerSettings());
    controls.read(*s);
    // (the loop's start where it was, unless it was moved: an old sampler's 0 stays 0, playing as it did)
    double at = loopStart->value() / 100;
    if (std::fabs(at - std::max(s->start, s->loopStart)) > 1e-9) s->loopStart = std::min(at, s->end - 0.001);
    s->sliceGrid = SLICE_GRIDS[std::min(4, std::max(0, sliceGrid->currentIndex()))];
    s->sensitivity = sensitivity->value() / 100;
    s->reverse = reverse->isChecked();
    s->normalise = normalise->isChecked();
    s->filterEnv = filterEnv->value();
    s->keyTrack = keyTrack->value() / 100;
    s->fAttack = fAttack->value() / 1000;
    s->fDecay = fDecay->value() / 1000;
    s->fSustain = fSustain->value() / 100;
    s->fRelease = fRelease->value() / 1000;
    s->lfoShape = lfoShape->currentIndex();
    s->lfoTarget = lfoTarget->currentIndex();
    s->lfoDepth = lfoDepth->value() / 100;
    s->lfoSync = lfoSync->isChecked();
    s->lfoRate = lfoRate->value();
    s->lfoBeats = lfoBeats->currentIndex();
    s->glide = glide->value() / 1000;
    s->mono = voices->currentIndex() == 1;
    s->warp = warp->isChecked();
    s->bpm = bpm->value();
    s->warpMode = warpMode->currentIndex();
    return s;
}

void SamplerDialog::reload() {
    if (!track->sampler) return;
    reloading_ = true;
    showSettings(*track->sampler);
    reloading_ = false;
}

void SamplerDialog::changed() {
    if (!track->sampler || reloading_) return;  // (it plays something else now; or the window only shows it again)
    SamplerPtr before = track->sampler;
    std::atomic_store(&track->sampler, settings());  // replaced whole: live notes use it at once
    const SamplerSettings& s = *track->sampler;
    util::debugf("sampler window: %s: %s, filter %d at %.0f Hz, LFO %.0f %% to %d, glide %.3f s%s%s%s%s",
                 track->name.c_str(), s.slice ? "slice" : s.oneShot ? "one-shot" : "classic", s.filter, s.cutoff,
                 s.lfoDepth * 100, s.lfoTarget, s.glide, s.mono ? ", mono" : "", s.warp ? ", warped" : "",
                 s.reverse ? ", reversed" : "", s.normalise ? ", normalised" : "");
    enableControls(s);
    loopStart->setValueSilently(std::max(s.start, s.loopStart) * 100);
    bool sound = !before || before->reverse != s.reverse || before->normalise != s.normalise ||
                 before->slice != s.slice || before->sliceGrid != s.sliceGrid || before->sensitivity != s.sensitivity ||
                 before->bpm != s.bpm || before->loop != s.loop || before->oneShot != s.oneShot;
    if (sound) {
        showWaveform(s);
    } else {
        waveform->start = s.start;
        waveform->end = s.end;
        if (waveform->loopStart >= 0) waveform->loopStart = std::max(s.start, s.loopStart);
        if (s.slice && (before->start != s.start || before->end != s.end || before->transpose != s.transpose))
            showWaveform(s);  // (the slices are cut within the part played)
        waveform->update();
    }
    if (renderTimer_ >= 0) ui::App::get().cancelTimer(renderTimer_);
    renderTimer_ = ui::App::get().singleShot(RENDER_DELAY_MS, [this] {
        renderTimer_ = -1;
        renderClips();
    });
}

void SamplerDialog::choose() {
    ClipPtr sample = chooseSample(this);
    if (sample && track->sampler) {
        auto s = std::make_shared<SamplerSettings>(*track->sampler);
        s->sample = sample;
        std::atomic_store(&track->sampler, SamplerPtr(s));
        showSettings(*track->sampler);
        renderClips();
    }
}

void SamplerDialog::renderClips() { renderTrackClips(track); }

void SamplerDialog::playRoot() {
    if (!track->sampler || !gHost.engine) return;
    live::NoteRender render = sampler::noteRenderer(*track);
    gHost.engine->liveNotes(track.get())->noteOn(track->sampler->root, 100, gHost.engine->sr, true, 1.0,
                                                 render.render ? &render : nullptr);
}

void SamplerDialog::done(int result) {
    if (renderTimer_ >= 0) {  // (a change not rendered yet: now)
        ui::App::get().cancelTimer(renderTimer_);
        renderTimer_ = -1;
        renderClips();
    }
    auto it = gWindows.find(track.get());
    if (it != gWindows.end() && it->second == this) gWindows.erase(it);
    ui::Dialog::done(result);
}

SamplerDialog* openFor(const Track* track) {
    auto it = gWindows.find(track);
    return it == gWindows.end() ? nullptr : it->second;
}

SamplerDialog* openSampler(const TrackPtr& track) {
    if (!track || !track->sampler) return nullptr;
    SamplerDialog* dialog = openFor(track.get());
    if (!dialog) {
        dialog = new SamplerDialog(gHost.window, track);
        gWindows[track.get()] = dialog;
    }
    dialog->open();
    dialog->raiseWindow();
    return dialog;
}

void reloadAll() {
    for (auto& kv : gWindows) kv.second->reload();
}

void closeGone(const Project& project) {
    std::vector<SamplerDialog*> gone;
    for (auto& kv : gWindows)
        if (!project.tracks.contains(kv.second->track) || !kv.second->track->sampler) gone.push_back(kv.second);
    for (SamplerDialog* d : gone) d->close();
}

}  // namespace sampler_dialog
