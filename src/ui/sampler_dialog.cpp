// The built-in sampler's window (sampler_dialog.py).
#include "ui/sampler_dialog.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

#include "audio/file_io.h"
#include "audio/sampler.h"
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
    int a = (int)std::lround(std::min(1.0, std::max(0.0, start)) * w);
    int b = (int)std::lround(std::min(1.0, std::max(0.0, end)) * w);
    if (a > 0) ui::style::box(p, Rect(0, top, a, h - top), *look_.outside);
    if (b < w) ui::style::box(p, Rect(b, top, w - b, h - top), *look_.outside);
    p.fillRect(Rect(std::max(0, std::min(w - mw, a)), top, mw, h - top), look_.marker);
    p.fillRect(Rect(std::max(0, std::min(w - mw, b - mw)), top, mw, h - top), look_.marker);
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

void SoundControls::build(ui::Widget* owner, ui::FormLayout* form, bool withRoot, const std::string& transposeLabel) {
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
        root->setToolTip("The key that plays the sample as it was recorded (60 = C4)");
        form->addRow("Root key", root);
    }
    transpose = number(owner, -48, 48, 2, " semitones", "Tune it up or down (fractions for fine tuning)");
    form->addRow(transposeLabel, transpose);
    gain = number(owner, -60, 24, 1, " dB");
    form->addRow("Volume", gain);
    velocity = number(owner, 0, 100, 0, " %", "How much softer quiet notes are: 0 % not at all, 100 % fully");
    form->addRow("Velocity", velocity);
    attack = number(owner, 0, 5000, 0, " ms");
    form->addRow("Attack", attack);
    decay = number(owner, 0, 20000, 0, " ms", "How long it takes from full level to the sustain level");
    form->addRow("Decay", decay);
    sustain = number(owner, 0, 100, 0, " %", "The level held while the note is");
    form->addRow("Sustain", sustain);
    release = number(owner, 0, 10000, 0, " ms", "How long it fades out once the note ends");
    form->addRow("Release", release);
    mode = owner->add<ui::ComboBox>();
    mode->addItem("Classic: plays while the note is held");
    mode->addItem("One-shot: plays to the end of the sample");
    form->addRow("Mode", mode);
    loop = owner->add<ui::CheckBox>("Loop the part played while the note is held (classic)");
    form->addRow(loop);
    start = number(owner, 0, 99.9, 1, " %", "Where in the sample it starts");
    form->addRow("Start", start);
    end = number(owner, 0.1, 100, 1, " %", "Where in the sample it ends");
    form->addRow("End", end);
}

void SoundControls::show(const SamplerSettings& s) {
    const std::pair<ui::SpinBox*, double> values[] = {
        {root, (double)s.root}, {transpose, s.transpose}, {gain, s.gainDb}, {velocity, s.velocity * 100},
        {attack, s.attack * 1000}, {decay, s.decay * 1000}, {sustain, s.sustain * 100}, {release, s.release * 1000},
        {start, s.start * 100}, {end, s.end * 100}};
    for (auto& bv : values)
        if (bv.first) bv.first->setValueSilently(bv.second);
    mode->setCurrentIndexSilently(s.oneShot ? 1 : 0);
    loop->setCheckedSilently(s.loop);
    loop->setEnabled(!s.oneShot);
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
    s.loop = loop->isChecked();
    s.start = std::min(a, b - 0.001);
    s.end = std::max(b, a + 0.001);
}

void SoundControls::connect(std::function<void()> changed) {
    for (ui::SpinBox* box : {root, transpose, gain, velocity, attack, decay, sustain, release, start, end})
        if (box) box->valueChanged.connect([changed](double) { changed(); });
    mode->currentIndexChanged.connect([changed](int) { changed(); });
    loop->toggled.connect([changed](bool) { changed(); });
}

SamplerDialog::SamplerDialog(ui::Window* window, TrackPtr track_)
    : ui::Dialog(window, "Built-in sampler \xe2\x80\x94 " + track_->name, 460, 560), track(std::move(track_)) {
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
    waveform->setCursor(ui::Cursor::Arrow);
    col->addWidget(waveform);
    auto formOwner = std::make_unique<ui::FormLayout>();
    ui::FormLayout* form = formOwner.get();
    col->addLayout(std::move(formOwner));
    controls.build(this, form, true);
    root = controls.root, transpose = controls.transpose, gain = controls.gain, velocity = controls.velocity;
    attack = controls.attack, decay = controls.decay, sustain = controls.sustain, release = controls.release;
    start = controls.start, end = controls.end, mode = controls.mode, loop = controls.loop;
    col->addStretch(1);
    auto buttons = std::make_unique<ui::HBox>();
    ui::Button* play = ui::pushButton(this, "Play the root key", [this] { playRoot(); });
    play->setToolTip("Hear it on the track (also: the MIDI editor's keys, or a MIDI keyboard)");
    buttons->addWidget(play);
    buttons->addStretch(1);
    buttons->addWidget(ui::pushButton(this, "Close", [this] { close(); }));
    col->addLayout(std::move(buttons));
    if (track->sampler) showSettings(*track->sampler);
    controls.connect([this] { changed(); });
}

SamplerDialog::~SamplerDialog() {
    if (renderTimer_ >= 0) ui::App::get().cancelTimer(renderTimer_);
    auto it = gWindows.find(track.get());
    if (it != gWindows.end() && it->second == this) gWindows.erase(it);
}

void SamplerDialog::showSettings(const SamplerSettings& s) {
    double seconds = s.sample && s.sample->source && s.sample->sourceSr
                         ? (double)s.sample->source->frames() / s.sample->sourceSr : 0.0;
    std::string name = s.sample ? s.sample->name : std::string();
    sampleName->setText(name + "  (" + fixed(seconds, 2) + " s)");
    waveform->setAudio(s.sample ? s.sample->source : nullptr, name, s.sample ? s.sample->sourceSr : 48000);
    waveform->start = s.start;
    waveform->end = s.end;
    waveform->update();
    controls.show(s);
}

SamplerPtr SamplerDialog::settings() const {
    auto s = std::make_shared<SamplerSettings>(track->sampler ? *track->sampler : SamplerSettings());
    controls.read(*s);
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
    std::atomic_store(&track->sampler, settings());  // replaced whole: live notes use it at once
    loop->setEnabled(!track->sampler->oneShot);
    waveform->start = track->sampler->start;
    waveform->end = track->sampler->end;
    waveform->update();
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
