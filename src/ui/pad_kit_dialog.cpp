// The Pad Kit's window, and the slice window (pad_kit_dialog.h).
#include "ui/pad_kit_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "audio/file_io.h"
#include "audio/padkit.h"
#include "audio/sampler.h"
#include "core/util.h"
#include "model/pad_tracks.h"
#include "ui/theme.h"
#include "ui/toolkit/layout.h"

namespace pad_kit_dialog {

namespace {
std::map<const Track*, PadKitDialog*> gWindows;  // one per track

std::string padNumber(int pad) { return "Pad " + std::to_string(pad + 1); }

// a new layout placed in `parent` (owned by it), to be filled afterwards
template <class L>
L* nest(ui::BoxLayout* parent, int stretch = 0) {
    auto made = std::make_unique<L>();
    L* raw = made.get();
    parent->addLayout(std::move(made), stretch);
    return raw;
}
}  // namespace

namespace {
// a choke group's colour as `style` gives it (its --choke-1 … --choke-8), else the program's own
Color chokeIn(const css::Style& style, int group) {
    static const Color colors[PadKit::CHOKE_GROUPS] = {
        Color(232, 112, 64), Color(80, 170, 230), Color(120, 200, 90), Color(220, 90, 170),
        Color(240, 200, 60), Color(150, 120, 230), Color(60, 200, 190), Color(200, 200, 200)};
    if (group < 1 || group > PadKit::CHOKE_GROUPS) return Color(0, 0, 0, 0);
    css::Rgba c;
    std::optional<std::string> v = style.variable("--choke-" + std::to_string(group));
    if (v && css::parseColor(*v, c)) return ui::style::toColor(c);
    return colors[group - 1];
}
}  // namespace

Color chokeColor(int group) {
    static int generation = -1;
    static css::Style style;
    const css::Engine& engine = theme::styles();
    if (engine.generation() != generation) {  // (a pad as the stylesheets see one, computed again when they change)
        style = engine.compute(css::SimpleNode("PadView"));
        generation = engine.generation();
    }
    return chokeIn(style, group);
}

std::string chokeName(int group) { return group <= 0 ? "None" : std::to_string(group); }

// =================================================================================================

PadView::PadView(PadKitDialog* dialog, int slot_) : slot(slot_), dialog_(dialog) {
    setStyleType("PadView");
    setMinimumWidth(theme::metric("pad-width", PAD_W));
    setCursor(ui::Cursor::Hand);
    setFocusPolicy(ui::FocusPolicy::Tab);  // (its keys: keyPress)
}

bool PadView::keyPress(ui::KeyEvent& e) {
    if (e.mods & (plat::ModCtrl | plat::ModAlt)) return false;
    int i = pad();
    int col = slot % 4, row = slot / 4;
    auto go = [&](int c, int r) {
        if (c < 0 || c > 3 || r < 0 || r > 3) return;
        int to = dialog_->bank * PadKit::BANK + r * 4 + c;
        dialog_->select(to, false);
        if (to % PadKit::BANK < (int)dialog_->pads.size()) dialog_->pads[(size_t)(to % PadKit::BANK)]->setFocus();
    };
    switch (e.key) {
    case plat::Key_Left: go(col - 1, row); return true;
    case plat::Key_Right: go(col + 1, row); return true;
    case plat::Key_Up: go(col, row - 1); return true;
    case plat::Key_Down: go(col, row + 1); return true;
    case plat::Key_Return:
    case plat::Key_Enter:
    case plat::Key_Space: dialog_->select(i, true); return true;
    case 'L': dialog_->loadSample(i); return true;
    case plat::Key_Delete: dialog_->clearPad(i); return true;
    case plat::Key_F2: {
        ui::Menu menu;
        dialog_->fillPadMenu(menu, i);
        menu.exec(mapToGlobal(Point(width() / 2, height() / 2)), this);
        return true;
    }
    default: return false;
    }
}

bool PadView::listRows(std::vector<std::string>& rows, int& current) const {
    int i = pad();
    PadKitPtr kit = dialog_->kit();
    const Pad& p = kit->pads[(size_t)i];
    rows.push_back(std::to_string(i + 1) + " " + (p.empty() || !p.sound.sample ? "empty" : p.sound.sample->name));
    TrackPtr own = pad_tracks::trackOf(*dialog_->track, i);
    rows.push_back("  " + sampler_dialog::noteName(p.note) + (p.choke ? "  choke " + std::to_string(p.choke) : "") +
                   (own ? "  -> " + own->name : ""));
    current = dialog_->selected == i ? 0 : -1;
    return true;
}

int PadView::pad() const { return dialog_->bank * PadKit::BANK + slot; }

void PadView::refresh() {
    PadKitPtr kit = dialog_->kit();
    int i = pad();
    const Pad& p = kit->pads[(size_t)i];
    AudioPtr audio = p.empty() ? nullptr : p.sound.sample->source;
    if (audio != this->audio()) setAudio(audio, padkit::padName(p), p.empty() ? 48000 : p.sound.sample->sourceSr);
    std::string tip = padNumber(i) + " on " + sampler::noteName(p.note) +
                      (p.empty() ? ": empty" : ": " + p.sound.sample->name) +
                      (p.choke ? ", choke group " + std::to_string(p.choke) : std::string()) +
                      (pad_tracks::trackOf(*dialog_->track, i) ? "\nPlays on its own track: " + pad_tracks::trackOf(*dialog_->track, i)->name
                                                               : std::string()) +
                      "\nClick to hear it and see its settings; drop sound files on it; right-click for more";
    setToolTip(tip);
    update();
}

void PadView::paint(Painter& p) {
    int w = width(), h = height();
    PadKitPtr kit = dialog_->kit();
    int i = pad();
    const Pad& pad = kit->pads[(size_t)i];
    bool selected = dialog_->selected == i;
    const css::Style& st = computedStyle();  // (in its states: :selected, :dropping)
    Font font = ui::style::font(st, theme::uiFont());
    ui::style::box(p, rect(), st);
    Color group = chokeIn(st, pad.choke);
    if (pad.choke) {
        double tint = computedStyle("choke-tint").number("opacity", 40 / 255.0);
        p.fillRect(Rect(0, 0, w, h), group.withAlpha((int)std::lround(std::min(1.0, std::max(0.0, tint)) * 255)));
    }
    // the waveform, under the name strip
    if (!pad.empty() && w > 8) {
        const std::vector<Lane>& ls = lanes(w - 8);
        double top = 16, laneH = (h - top - 4) / (double)std::max<size_t>(1, ls.size());
        Color wave = ui::style::color(computedStyle("wave"), "color");
        for (size_t k = 0; k < ls.size(); k++) {
            double middle = top + k * laneH + laneH / 2, half = laneH / 2 - 1;
            for (int x = 0; x < (int)ls[k].maxs.size(); x++) {
                double y0 = middle - std::min(1.0, (double)ls[k].maxs[(size_t)x]) * half;
                double y1 = middle - std::max(-1.0, (double)ls[k].mins[(size_t)x]) * half;
                p.drawVLine(4 + x, (int)y0, (int)std::max(y1, y0 + 1) + 1, wave);
            }
        }
    } else {
        const css::Style& empty = computedStyle("empty");
        p.drawText(RectF(0, 16, (float)w, (float)h - 16), AlignCenter, dropping_ ? "Drop to load" : "Drop a sound",
                   ui::style::font(empty, font), ui::style::foreground(empty));
    }
    // its name (left) and key (right), and its choke group
    const css::Style& name = computedStyle("name");
    Color text = ui::style::foreground(name);
    std::string keyText = (pad_tracks::trackOf(*dialog_->track, i) ? "\xe2\x86\x92 " : "") +  // (→: on its own track)
                          sampler::noteName(pad.note) + (pad.choke ? "  \xe2\x9c\x95" + std::to_string(pad.choke) : "");
    float keyW = font.width(keyText);
    p.drawText(RectF((float)w - keyW - 6, 1, keyW + 2, 14), AlignRight | AlignVCenter, keyText, font,
               pad.choke ? group : text);
    std::string label = pad.empty() ? std::to_string(i + 1) : std::to_string(i + 1) + " " + padkit::padName(pad);
    p.drawText(RectF(5, 1, std::max(0.f, (float)w - keyW - 14), 14), AlignLeft | AlignVCenter, label,
               ui::style::font(name, font.withBold(selected)), text, true);
    if (pad.choke) p.fillRect(Rect(0, 0, ui::style::px(computedStyle("stripe"), "width", 3), h), group);
    const css::Style& frame = computedStyle("frame");
    p.drawRect(rect(), ui::style::color(frame, "border-top-color"), ui::style::px(frame, "border-top-width", 1));
}

void PadView::styleStates(std::vector<std::string>& out) const {
    SampleWaveform::styleStates(out);
    if (dialog_->selected == pad()) out.push_back("selected");
    if (dropping_) out.push_back("dropping");
}

bool PadView::mousePress(ui::MouseEvent& e) {
    if (e.button == plat::LeftButton) {
        dialog_->select(pad(), true);
        return true;
    }
    if (e.button == plat::RightButton) {
        dialog_->select(pad(), false);
        return true;
    }
    return false;
}

bool PadView::contextMenu(ui::MouseEvent& e) {
    ui::Menu menu;
    dialog_->fillPadMenu(menu, pad());
    menu.exec(e.globalPos, this);
    return true;
}

void PadView::dragEnter(ui::DragEvent& e) {
    if (e.data && !e.data->files.empty()) {
        e.accepted = true;
        if (!dropping_) dropping_ = true, update();
    }
}

void PadView::dragLeave() {
    if (dropping_) dropping_ = false, update();
}

void PadView::drop(ui::DragEvent& e) {
    dropping_ = false;
    if (!e.data || e.data->files.empty()) return;
    std::vector<std::string> paths = e.data->files;
    dialog_->dropFiles(pad(), paths);
    e.accepted = true;
}

// =================================================================================================

PadKitDialog::PadKitDialog(ui::Window* window, TrackPtr track_)
    : ui::Dialog(window, "Pad Kit \xe2\x80\x94 " + track_->name, 900, 680), track(std::move(track_)) {
    deleteOnClose = true;
    setStyleType("PadKitDialog");
    // (each layout is placed in its parent before it is filled: a form's labels need their owner)
    ui::VBox* col = ui::styleRoom(ui::vbox(this, 9, 8), computedStyle());
    auto* columns = nest<ui::HBox>(col, 1);
    columns->setSpacing(14);
    ui::styleRoom(columns, computedStyle("columns"));
    // the pads (left)
    auto* left = nest<ui::VBox>(columns, 3);
    left->setSpacing(6);
    ui::styleRoom(left, computedStyle("pads"));
    auto* banks = nest<ui::HBox>(left);
    bankA = ui::pushButton(this, "Pads 1\xe2\x80\x93" "16", [this] { showBank(0); });
    bankB = ui::pushButton(this, "Pads 17\xe2\x80\x93" "32", [this] { showBank(1); });
    for (ui::Button* b : {bankA, bankB}) {
        b->setCheckable(true);
        banks->addWidget(b);
    }
    bankA->setToolTip("The first 16 pads (keys " + sampler::noteName(PadKit::FIRST_NOTE) + " up)");
    bankB->setToolTip("Pads 17 to 32");
    banks->addStretch(1);
    auto* grid = nest<ui::GridLayout>(left);
    for (int k = 0; k < PadKit::BANK; k++) {
        PadView* v = add<PadView>(this, k);
        pads.push_back(v);
        // (bottom row first, as on hardware pads: pad 1 bottom left)
        grid->addWidget(v, PadKit::BANK / COLUMNS - 1 - k / COLUMNS, k % COLUMNS);
    }
    hint = add<ui::Label>("Click a pad to hear it. Drop sound files on a pad (several fill the pads after it); "
                          "right-click a pad for more.");
    hint->setWordWrap(true);
    hint->setProperty("hint", "true");  // (the skin's hint colour)
    left->addWidget(hint);
    left->addStretch(1);
    // the selected pad's settings (right)
    auto* right = nest<ui::VBox>(columns, 2);
    right->setSpacing(6);
    ui::styleRoom(right, computedStyle("settings"));
    auto* title = nest<ui::HBox>(right);
    padTitle = add<ui::Label>();
    padTitle->addStyleClass("heading");  // (default.css: Label.heading, bold)
    padTitle->setElide(true);
    title->addWidget(padTitle, 1);
    loadButton = ui::pushButton(this, "Load sample\xe2\x80\xa6", [this] { loadSample(selected); });
    loadButton->setToolTip("Another sound on this pad (its key and choke group stay)");
    title->addWidget(loadButton);
    waveform = add<sampler_dialog::SamplerWaveform>();
    waveform->setFixedHeight(theme::metric("sample-waveform-height", SampleWaveform::HEIGHT));
    waveform->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
    waveform->setCursor(ui::Cursor::Arrow);
    right->addWidget(waveform);
    auto* form = nest<ui::FormLayout>(right);
    key = add<ui::SpinBox>();
    key->setRange(0, 127);
    key->setDecimals(0);
    key->textFromValue = [](double v) { int k = (int)std::lround(v); return sampler::noteName(k) + " (" + std::to_string(k) + ")"; };
    key->valueFromText = [](const std::string& text, double& v) {
        size_t open = text.rfind('(');
        std::string digits = open == std::string::npos ? text : text.substr(open + 1);
        try {
            v = std::stod(digits);
            return true;
        } catch (...) {
            return false;
        }
    };
    key->setKeyboardTracking(false);
    key->setToolTip("The key that plays this pad (its sample at its own pitch)");
    form->addRow("Key", key);
    choke = add<ui::ComboBox>();
    for (int g = 0; g <= PadKit::CHOKE_GROUPS; g++) choke->addItem(g ? "Group " + std::to_string(g) : std::string("None"));
    choke->setToolTip("A pad cuts off every other pad of its choke group still sounding (open and closed hi-hats)");
    form->addRow("Choke group", choke);
    controls.build(this, form, false, "Tune");
    right->addStretch(1);
    auto buttons = std::make_unique<ui::HBox>();
    ui::Button* playButton = ui::pushButton(this, "Play the pad", [this] { play(selected); });
    playButton->setToolTip("Hear it on the track (also: the MIDI editor's keys, or a MIDI keyboard)");
    buttons->addWidget(playButton);
    ui::Button* toTracks = ui::pushButton(this, "Pads to tracks", [this] { padsToTracks(track); });
    toTracks->setToolTip("Every used pad on a track of its own (its own effects, fader, pan, sends and meter), "
                         "below this one, grouped with it");
    buttons->addWidget(toTracks);
    buttons->addStretch(1);
    buttons->addWidget(ui::pushButton(this, "Close", [this] { close(); }));
    col->addLayout(std::move(buttons));
    key->valueChanged.connect([this](double) { changed(); });
    choke->currentIndexChanged.connect([this](int) { changed(); });
    controls.connect([this] { changed(); });
    showBank(0);
}

PadKitDialog::~PadKitDialog() {
    if (renderTimer_ >= 0) ui::App::get().cancelTimer(renderTimer_);
    auto it = gWindows.find(track.get());
    if (it != gWindows.end() && it->second == this) gWindows.erase(it);
}

PadKitPtr PadKitDialog::kit() const {
    PadKitPtr k = std::atomic_load(&track->padKit);
    if (k) return k;
    static const PadKitPtr none = std::make_shared<PadKit>();
    return none;
}

void PadKitDialog::scheduleRender() {
    if (renderTimer_ >= 0) ui::App::get().cancelTimer(renderTimer_);
    renderTimer_ = ui::App::get().singleShot(sampler_dialog::RENDER_DELAY_MS, [this] {
        renderTimer_ = -1;
        renderClips();
    });
}

void PadKitDialog::setKit(const PadKitPtr& made) {
    if (!made || !track->padKit) return;  // (the track plays something else now)
    std::atomic_store(&track->padKit, made);  // replaced whole: live notes use it at once
    for (PadView* v : pads) v->refresh();
    scheduleRender();
}

void PadKitDialog::showBank(int b) {
    bank = std::min(1, std::max(0, b));
    bankA->setCheckedSilently(bank == 0);
    bankB->setCheckedSilently(bank == 1);
    for (PadView* v : pads) v->refresh();
    showPad();
}

void PadKitDialog::select(int pad, bool hear) {
    if (pad < 0 || pad >= PadKit::MAX_PADS) return;
    selected = pad;
    if (pad / PadKit::BANK != bank) showBank(pad / PadKit::BANK);
    for (PadView* v : pads) v->update();
    showPad();
    if (hear) play(pad);
}

void PadKitDialog::showPad() {
    PadKitPtr k = kit();
    const Pad& p = k->pads[(size_t)selected];
    reloading_ = true;
    double seconds = p.empty() || !p.sound.sample->sourceSr ? 0.0
                                                             : (double)p.sound.sample->source->frames() / p.sound.sample->sourceSr;
    char length[32];
    std::snprintf(length, sizeof length, "%.2f", seconds);
    padTitle->setText(padNumber(selected) + ": " + (p.empty() ? std::string("empty") : p.sound.sample->name + "  (" + length + " s)"));
    waveform->setAudio(p.empty() ? nullptr : p.sound.sample->source, padkit::padName(p),
                       p.empty() ? 48000 : p.sound.sample->sourceSr);
    waveform->start = p.sound.start;
    waveform->end = p.sound.end;
    waveform->update();
    key->setValueSilently(p.note);
    choke->setCurrentIndexSilently(std::min(PadKit::CHOKE_GROUPS, std::max(0, p.choke)));
    controls.show(p.sound);
    reloading_ = false;
}

void PadKitDialog::changed() {
    if (reloading_ || !track->padKit) return;
    PadKitPtr k = kit();
    Pad p = k->pads[(size_t)selected];
    p.note = key->intValue();
    p.choke = choke->currentIndex();
    controls.read(p.sound);
    controls.loop->setEnabled(!p.sound.oneShot);
    waveform->start = p.sound.start;
    waveform->end = p.sound.end;
    waveform->update();
    setKit(padkit::withPad(*k, selected, p));
}

void PadKitDialog::play(int pad) {
    const sampler_dialog::Host& host = sampler_dialog::host();
    if (!host.engine || !track->playsPadKit() || pad < 0 || pad >= PadKit::MAX_PADS) return;
    const Pad& p = kit()->pads[(size_t)pad];
    if (p.empty()) return;
    live::NoteRender render = sampler::noteRenderer(*track);
    host.engine->liveNotes(track.get())->noteOn(p.note, 100, host.engine->sr, true, 1.0,
                                                render.render ? &render : nullptr);
}

void PadKitDialog::reload() {
    if (!track->padKit) return;
    for (PadView* v : pads) v->refresh();
    showPad();
}

void PadKitDialog::dropFiles(int pad, const std::vector<std::string>& paths) {
    std::vector<std::string> errors;
    PadKitPtr made = padkit::withFiles(*kit(), pad, paths, errors);
    if (made) {
        setKit(made);
        select(pad, false);
    }
    if (!errors.empty()) {
        std::string text = "Couldn't load " + errors[0];
        if (errors.size() > 1) text += " (and " + std::to_string(errors.size() - 1) + " more)";
        hint->setText(text);
    }
}

void PadKitDialog::loadSample(int pad) {
    std::string patterns;
    for (const std::string& e : file_io::AUDIO_EXTENSIONS) patterns += (patterns.empty() ? "*" : " *") + e;
    std::string path = ui::files::openFile(this, "Sample for " + padNumber(pad), "", "Audio files (" + patterns + ")");
    if (path.empty()) return;
    dropFiles(pad, {path});
}

void PadKitDialog::clearPad(int pad) {
    PadKitPtr k = kit();
    if (pad < 0 || pad >= (int)k->pads.size()) return;
    Pad p = PadKit().pads[(size_t)pad];
    p.note = k->pads[(size_t)pad].note;  // (its key and choke group stay)
    p.choke = k->pads[(size_t)pad].choke;
    setKit(padkit::withPad(*k, pad, p));
    if (pad == selected) showPad();
}

void PadKitDialog::setChoke(int pad, int group) {
    PadKitPtr k = kit();
    if (pad < 0 || pad >= (int)k->pads.size()) return;
    Pad p = k->pads[(size_t)pad];
    p.choke = std::min(PadKit::CHOKE_GROUPS, std::max(0, group));
    setKit(padkit::withPad(*k, pad, p));
    if (pad == selected) showPad();
}

void PadKitDialog::fillPadMenu(ui::Menu& menu, int pad) {
    const Pad& p = kit()->pads[(size_t)pad];
    menu.addAction("Load sample\xe2\x80\xa6", [this, pad] { loadSample(pad); });
    ui::Action* clear = menu.addAction("Clear", [this, pad] { clearPad(pad); });
    clear->setEnabled(!p.empty());
    ui::Menu* groups = menu.addMenu("Choke group");
    for (int g = 0; g <= PadKit::CHOKE_GROUPS; g++) {
        ui::Action* a = groups->addAction(chokeName(g), [this, pad, g] { setChoke(pad, g); });
        a->checkable = true;
        a->checked = p.choke == g;
    }
    // pads on their own tracks (model/pad_tracks.h)
    TrackPtr own = pad_tracks::trackOf(*track, pad);
    ui::Menu* output = menu.addMenu("Output");
    ui::Action* inKit = output->addAction("The kit", [this, pad] { setOutput(pad, false); });
    inKit->checkable = true;
    inKit->checked = !own;
    ui::Action* ownTrack = output->addAction(own ? "Its own track: " + own->name : std::string("Its own track"),
                                             [this, pad] { setOutput(pad, true); });
    ownTrack->checkable = true;
    ownTrack->checked = (bool)own;
    ownTrack->setEnabled(!p.empty() || own);
    menu.addAction("Pads to tracks", [this] { padsToTracks(track); })->tooltip =
        "Every used pad on a track of its own, below this one, grouped with it";
}

void PadKitDialog::setOutput(int pad, bool own) {
    if (setPadOutput(track, pad, own)) reload();
}

void PadKitDialog::renderClips() { sampler_dialog::renderTrackClips(track); }

void PadKitDialog::done(int result) {
    if (renderTimer_ >= 0) {  // (a change not rendered yet: now)
        ui::App::get().cancelTimer(renderTimer_);
        renderTimer_ = -1;
        renderClips();
    }
    auto it = gWindows.find(track.get());
    if (it != gWindows.end() && it->second == this) gWindows.erase(it);
    ui::Dialog::done(result);
}

PadKitDialog* openFor(const Track* track) {
    auto it = gWindows.find(track);
    return it == gWindows.end() ? nullptr : it->second;
}

PadKitDialog* openPadKit(const TrackPtr& track) {
    if (!track || !track->padKit) return nullptr;
    PadKitDialog* dialog = openFor(track.get());
    if (!dialog) {
        dialog = new PadKitDialog(sampler_dialog::host().window, track);
        gWindows[track.get()] = dialog;
    }
    dialog->open();
    dialog->raiseWindow();
    return dialog;
}

void reloadAll() {
    std::vector<PadKitDialog*> all, gone;
    for (auto& kv : gWindows) (kv.second->track->padKit ? all : gone).push_back(kv.second);
    for (PadKitDialog* d : all) d->reload();
    for (PadKitDialog* d : gone) d->close();  // (undone: the track has no kit now)
}

namespace {
// after a pad's output changed: the kits' clips rendered again (their pads kept apart, or not), the rows shown
void routed(const std::vector<TrackPtr>& kits, bool rows) {
    for (const TrackPtr& kit : kits) {
        sampler_dialog::renderTrackClips(kit);
        if (PadKitDialog* d = openFor(kit.get())) d->reload();
    }
    const sampler_dialog::Host& host = sampler_dialog::host();
    if (rows && host.tracksChanged) host.tracksChanged();
}
bool frozenKit(const TrackPtr& kit) {
    if (!aload(kit->frozen)) return false;
    const sampler_dialog::Host& host = sampler_dialog::host();
    if (host.status) host.status(kit->name + " is frozen: unfreeze it to change where its pads play");
    return true;
}
}  // namespace

std::vector<TrackPtr> padsToTracks(const TrackPtr& kit) {
    const sampler_dialog::Host& host = sampler_dialog::host();
    if (!host.project || !kit || !kit->playsPadKit() || frozenKit(kit)) return {};
    std::vector<TrackPtr> made = pad_tracks::padsToTracks(*host.project, kit);
    routed({kit}, true);
    if (host.status)
        host.status(made.empty() ? "Every used pad of " + kit->name + " has a track of its own already"
                                 : std::to_string(made.size()) + " pad" + (made.size() == 1 ? "" : "s") + " of " + kit->name +
                                       " on tracks of their own, in the group " + kit->name);
    return made;
}

bool setPadOutput(const TrackPtr& kit, int pad, bool own) {
    const sampler_dialog::Host& host = sampler_dialog::host();
    if (!host.project || !kit || frozenKit(kit)) return false;
    bool had = (bool)pad_tracks::trackOf(*kit, pad);
    if (own == had) return true;
    TrackPtr made = own ? pad_tracks::giveTrack(*host.project, kit, pad) : nullptr;
    if (!own) pad_tracks::setTrack(kit, pad, nullptr);
    if (own && !made) return false;
    routed({kit}, own);
    if (host.status)
        host.status(own ? "Pad " + std::to_string(pad + 1) + " plays on its own track: " + made->name
                        : "Pad " + std::to_string(pad + 1) + " plays in " + kit->name + " again");
    return true;
}

void backToKit(const TrackPtr& padTrack) {
    const sampler_dialog::Host& host = sampler_dialog::host();
    if (!host.project || !padTrack) return;
    TrackPtr kit = pad_tracks::kitOf(*host.project, *padTrack);
    if (!kit || frozenKit(kit)) return;
    routed(pad_tracks::backToKit(*host.project, *padTrack), true);
    if (host.status) host.status(padTrack->name + ": its pad plays in " + kit->name + " again");
}

void closeGone(const Project& project) {
    std::vector<PadKitDialog*> gone;
    for (auto& kv : gWindows)
        if (!project.tracks.contains(kv.second->track) || !kv.second->track->padKit) gone.push_back(kv.second);
    for (PadKitDialog* d : gone) d->close();
}

}  // namespace pad_kit_dialog
