#include "ui/midi_editor.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "audio/midi_effects.h"
#include "audio/plugin_audio.h"
#include "config.h"
#include "core/json.h"
#include "core/util.h"
#include "model/edits.h"
#include "ui/colors.h"
#include "ui/computer_keyboard.h"
#include "ui/formatting.h"
#include "ui/midi_input.h"
#include "ui/paint_helpers.h"
#include "ui/theme.h"
#include "ui/toolkit/layout.h"
#include "ui/widgets.h"

using namespace midi_editor;

namespace midi_editor {

std::vector<CopiedNote> noteClipboard;

bool isBlackKey(int pitch) {
    int k = ((pitch % 12) + 12) % 12;
    return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}

std::string noteName(int pitch) {
    static const char* const NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int k = ((pitch % 12) + 12) % 12;
    int octave = (int)std::floor(pitch / 12.0) - 1;
    return std::string(NAMES[k]) + std::to_string(octave);
}

const char* const* const KEYS = midi_effects::KEYS;

const std::vector<std::pair<std::string, std::vector<int>>>& chords() { return midi_effects::CHORDS; }
const std::vector<std::pair<std::string, std::vector<int>>>& scales() { return midi_effects::SCALES; }
Steps scaleSteps(int root, const std::string& scale) { return midi_effects::scaleSteps(root, scale); }
int nearestInScale(int pitch, const Steps& steps) { return (int)midi_effects::nearestInScale(pitch, steps); }
std::vector<int> inScaleChord(int root, const Steps& steps, int notes) {
    return midi_effects::inScaleChord(root, steps, notes);
}

const std::vector<std::pair<std::string, std::string>>& ghostModes() {
    static const std::vector<std::pair<std::string, std::string>> m = {
        {"all", "Every track's notes"}, {"track", "This track's other clips"}, {"off", "No ghost notes"}};
    return m;
}

const std::vector<std::string>& stamps() {
    static const std::vector<std::string> s = [] {
        std::vector<std::string> v = {"Off"};
        for (auto& c : chords())
            if (c.first != "Octave down") v.push_back(c.first);
        v.push_back("In the scale: triad");
        v.push_back("In the scale: 7th");
        return v;
    }();
    return s;
}

const std::vector<std::pair<std::string, std::string>>& folds() {
    static const std::vector<std::pair<std::string, std::string>> f = {
        {"off", "Show every key"}, {"scale", "Fold to the scale"}, {"used", "Fold to the notes used"}};
    return f;
}

// --- the notes' own properties in the lane ---

const std::vector<LaneInfo>& lanes() {
    static const std::vector<LaneInfo> l = {
        {Lane::Velocity, "Velocity", "1\xe2\x80\x93" "127", 1, 127, 1, false},
        {Lane::Chance, "Chance", "0\xe2\x80\x93" "100 %", 0, 100, 1, false},
        {Lane::Repeats, "Repeats", "1\xe2\x80\x93" "8 hits", 1, note_props::MAX_REPEATS, 1, false},
        {Lane::Pan, "Pan", "L \xe2\x80\x93 R", -1, 1, 0.01, true},
        {Lane::Fine, "Fine", "\xc2\xb1" "100 cents", -100, 100, 1, true},
    };
    return l;
}

const LaneInfo& laneInfo(Lane lane) { return lanes()[std::min(lanes().size() - 1, (size_t)lane)]; }

double laneValue(const Note& n, Lane lane) {
    switch (lane) {
    case Lane::Velocity: return n.velocity;
    case Lane::Chance: return n.chance;
    case Lane::Repeats: return n.repeats;
    case Lane::Pan: return n.pan;
    case Lane::Fine: return n.fine;
    }
    return 0;
}

void setLaneValue(Note& n, Lane lane, double value) {
    const LaneInfo& info = laneInfo(lane);
    if (!std::isfinite(value)) return;
    value = std::min(info.high, std::max(info.low, std::nearbyint(value / info.step) * info.step));
    switch (lane) {
    case Lane::Velocity: n.velocity = value; break;
    case Lane::Chance: n.chance = value; break;
    case Lane::Repeats: n.repeats = (int)value; break;
    case Lane::Pan: n.pan = std::fabs(value) < 1e-9 ? 0.0 : value; break;  // (the middle exactly: a plain note)
    case Lane::Fine: n.fine = value; break;
    }
}

}  // namespace midi_editor

namespace {

// Python's round(): halves to even
double pyRound(double v) { return std::nearbyint(v); }

// a QRectF filled without anti-aliasing (as QPainter::fillRect does): the pixels whose centres are inside
Rect aliased(double x, double y, double w, double h) {
    int x0 = (int)std::lround(x), y0 = (int)std::lround(y);
    int x1 = (int)std::lround(x + w), y1 = (int)std::lround(y + h);
    return Rect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

const Color& grid(int strength) {
    static const char* const NAMES[] = {"grid_fine", "grid", "grid_bar"};
    return theme::c(NAMES[std::max(0, std::min(2, strength))]);
}

bool startsWith(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// check one action of an exclusive group (by its key), unchecking the others
void checkOnly(std::map<std::string, ui::Action*>& actions, const std::string& key) {
    for (auto& kv : actions) kv.second->checked = kv.first == key;
}

// a QPen DashLine box of width 1 (dashes of 4, gaps of 2)
void dashedBox(Painter& p, const RectF& r, Color c) {
    p.drawDashedLine(PointF(r.x, r.y + 0.5f), PointF(r.right(), r.y + 0.5f), c, 1, 4, 2);
    p.drawDashedLine(PointF(r.right() - 0.5f, r.y), PointF(r.right() - 0.5f, r.bottom()), c, 1, 4, 2);
    p.drawDashedLine(PointF(r.right(), r.bottom() - 0.5f), PointF(r.x, r.bottom() - 0.5f), c, 1, 4, 2);
    p.drawDashedLine(PointF(r.x + 0.5f, r.bottom()), PointF(r.x + 0.5f, r.y), c, 1, 4, 2);
}

// px tall the hint is, word-wrapped at `width` (the toolkit's Label has no height-for-width)
int wrappedHeight(const std::string& text, const Font& f, float width) {
    int lines = 1;
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find(' ', i);
        std::string word = text.substr(i, j == std::string::npos ? std::string::npos : j - i);
        std::string tried = line.empty() ? word : line + " " + word;
        if (!line.empty() && f.width(tried) > width) {
            ++lines;
            line = word;
        } else {
            line = tried;
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return (int)std::ceil((lines - 1) * f.lineSpacing() + f.ascent() + f.descent());
}

// the hint under the toolbar: the piano roll's, the Steps view's
const char* const ROLL_HINT =
    "Click to add a note, drag to move, drag its right end to resize, right-click to delete. Shift+drag: select "
    "with a box; Ctrl+drag: copy; arrows: move (Shift+arrows: octave, length); Ctrl+C / X / V, Ctrl+D duplicate, "
    "Ctrl+U quantize, G slide. Alt: no snapping.";
const char* const STEPS_HINT =
    "Click a pad to add a note one step long, click a lit pad to remove it; drag along to paint (or clear) more. "
    "Ctrl+click: select; right-click: delete; + Row: another pitch; G slide. Ctrl+wheel: zoom (the grid follows).";

// A note's box (the piano roll's and the Steps view's): its velocity in the fill's lightness, its chance in
// the fill's opacity, its repeats as ticks across it; switched off: an outline.
void paintNoteBox(Painter& p, const RectF& r, const Note& n, Color color, bool chosen, bool isOff) {
    Color fill = color.lighter(100 + (int)(n.velocity / 127 * 60));
    if (!isOff) p.fillRect(r, n.chance < 100 ? fill.withAlpha(70 + (int)(std::max(0.0, n.chance) / 100 * 185)) : fill);
    if (n.repeats > 1 && r.w >= 3.0f * (float)n.repeats)
        for (int k = 1; k < n.repeats; ++k)
            p.fillRect(RectF(r.x + r.w * (float)k / (float)n.repeats - 0.5f, r.y + 2, 1, std::max(1.0f, r.h - 4)),
                       fill.darker(170));
    if (chosen) p.strokeRect(r, theme::c("selected_note"), 2);
    else p.strokeRect(r, isOff ? fill : fill.darker(160), 1);
}

// a sliding note's tail: a short line from the pitch it glides from (`from`) into its start (`to`), arrowed
void paintSlideTail(Painter& p, PointF from, PointF to, Color c) {
    p.drawLine(from, to, c, 2, LineCap::Round);
    float dx = to.x - from.x, dy = to.y - from.y, len = std::sqrt(dx * dx + dy * dy);
    if (len < 1) return;
    dx /= len, dy /= len;
    for (float side : {-1.0f, 1.0f})
        p.drawLine(to, PointF(to.x - 5 * dx + side * 3 * dy, to.y - 5 * dy - side * 3 * dx), c, 1.5f, LineCap::Round);
}

RectF normalized(Point a, Point b) {
    float x0 = (float)std::min(a.x, b.x), y0 = (float)std::min(a.y, b.y);
    return RectF(x0, y0, (float)std::abs(a.x - b.x), (float)std::abs(a.y - b.y));
}

}  // namespace

// =================================================================================================
// MidiEditor
// =================================================================================================

static plat::WindowOptions editorOptions(ui::Window* parent) {
    plat::WindowOptions o;
    o.title = "Notes";
    o.w = 960;
    o.h = 560;
    o.transientFor = parent ? parent->native() : nullptr;
    return o;
}

MidiEditor::MidiEditor(ui::Window* parent, ClipPtr clip_, Project& project_, audio::Engine& engine_,
                       std::function<void()> onChange_, TimelineLink timeline_)
    : ui::Window(editorOptions(parent)),
      clip(std::move(clip_)),
      project(project_),
      engine(engine_),
      onChange(std::move(onChange_)),
      timeline(std::move(timeline_)) {
    deleteOnClose = true;
    if (clip && clip->notes) notes = *clip->notes;  // the working copy
    takeMuted();
    {
        std::string mode = settings::get("options/ghost_notes").toString("all");
        ghostMode = "all";
        for (auto& m : ghostModes())
            if (m.first == mode) ghostMode = mode;
    }

    roll = add<PianoRoll>(this);
    steps = add<StepGrid>(this);
    steps->hide();  // (the piano roll first)
    velocity = add<VelocityLane>(this);
    ruler = add<EditorRuler>(this);
    hbar = add<ui::ScrollBar>(ui::Orientation::Horizontal);
    vbar = add<ui::ScrollBar>(ui::Orientation::Vertical);
    hbar->hPolicy = ui::SizePolicy::Expanding;
    vbar->vPolicy = ui::SizePolicy::Expanding;
    hbar->valueChanged.connect([this](int) { refresh(); });
    vbar->valueChanged.connect([this](int) { refresh(); });
    int vbarW = vbar->sizeHint().x;

    hint = add<ui::Label>(ROLL_HINT);
    hint->setProperty("hint", "true");  // the skin's hint colour
    hint->setWordWrap(true);  // (a narrow window wraps it, rather than the hint making the window wide)

    ui::VBox* col = ui::vbox(this, 0, 0);
    auto tools = std::make_unique<ui::HBox>();
    tools->setContentsMargins(4, 2, 4, 0);
    tools->spacing = 4;
    stepsButton = ui::toolButton(this, "Steps", nullptr,
                                 "Show the clip as steps: a row per pitch it uses, a column per grid step. Click a "
                                 "pad to add or remove a note one step long; again for the piano roll");
    stepsButton->setCheckable(true);
    stepsButton->toggled.connect([this](bool on) { setStepsView(on); });
    tools->addWidget(stepsButton);
    tools->addWidget(buildGhostsButton());
    tools->addWidget(buildStampButton());
    tools->addSpacing(8);
    buildScaleTools(tools.get());
    tools->addStretch(1);
    quantizeButton = ui::toolButton(this, "Quantize", [this] { quantize(1.0); },
                                    "Put the selected notes (none selected: all) on the grid at this zoom, keeping "
                                    "their lengths (Ctrl+U); \xe2\x96\xbe for a gentler strength, or to quantize "
                                    "their ends too");
    {
        ui::Menu* menu = widgets::ownedMenu(quantizeButton);
        for (int percent : {100, 75, 50, 25}) {
            double s = percent / 100.0;
            menu->addAction("Quantize " + std::to_string(percent) + " %", [this, s] { quantize(s); });
        }
        menu->addSeparator();
        quantizeEnds = menu->addAction("Quantize the notes' ends too");
        quantizeEnds->checkable = true;
        quantizeButton->setMenu(menu, true);  // MenuButtonPopup
    }
    tools->addWidget(quantizeButton);
    switchButton = ui::toolButton(this, "Switch off", [this] { toggleOff(); },
                                  "The selected notes stop playing but stay where they are; again to switch them back "
                                  "on (0)");
    tools->addWidget(switchButton);
    slideButton = ui::toolButton(this, "Slide", [this] { toggleSlide(); },
                                 "The selected notes glide from the previous note's pitch into their own (an 808 "
                                 "glide); again to stop them (G)");
    slideButton->setCheckable(true);  // (lit while every selected note slides: syncNoteTools)
    tools->addWidget(slideButton);
    tools->addWidget(add<ui::Label>("Keys"));
    struct KeyButton { const char* text; const char* tip; int step; };
    for (const KeyButton& kb : {KeyButton{"\xe2\x88\x92", "Shorter keys: more of them in view (Alt+wheel)", -2},
                                KeyButton{"+", "Taller keys (Alt+wheel)", 2}}) {
        int step = kb.step;
        tools->addWidget(ui::toolButton(this, kb.text, [this, step] { setRowHeight(rowH + step); }, kb.tip));
    }
    expandButton = ui::toolButton(this, "\xe2\xa4\xa2 Expand", nullptr,
                                  "Let the editor fill the screen (again to make it small)");
    expandButton->setCheckable(true);
    expandButton->toggled.connect([this](bool on) { setExpanded(on); });
    tools->addWidget(expandButton);
    col->addLayout(std::move(tools));
    auto hintBox = std::make_unique<ui::HBox>();
    hintBox->setContentsMargins(4, 2, 4, 2);
    hintBox->addWidget(hint, 1);
    col->addLayout(std::move(hintBox));

    auto rulerRow = std::make_unique<ui::HBox>();
    rulerRow->spacing = 0;
    rulerRow->addWidget(ruler, 1);
    rulerRow->addSpacing(vbarW);
    col->addLayout(std::move(rulerRow));
    auto top = std::make_unique<ui::HBox>();
    top->spacing = 0;
    top->addWidget(roll, 1);
    top->addWidget(steps, 1);  // (one of the two shows)
    top->addWidget(vbar);
    col->addLayout(std::move(top), 1);
    auto bottom = std::make_unique<ui::HBox>();
    bottom->spacing = 0;
    bottom->addWidget(velocity, 1);
    bottom->addSpacing(vbarW);
    col->addLayout(std::move(bottom));
    auto hrow = std::make_unique<ui::HBox>();
    hrow->setContentsMargins(KEYS_W, 0, vbarW, 0);
    hrow->addWidget(hbar, 1);
    col->addLayout(std::move(hrow));

    resized();   // (the hint's height for this width)
    relayout();  // (so fit() sees the roll's real size)
    updateTitle();
    takeUsed();
    syncScaleTools();
    fit();
    roll->setFocus();
    activated.connect([this] {
        syncScaleTools();  // (another editor may have changed the song's scale)
        refresh();
    });
    cursorTimer_ = ui::App::get().every(CURSOR_MS, [this] { followCursors(); });
}

MidiEditor::~MidiEditor() {
    if (cursorTimer_ >= 0) ui::App::get().cancelTimer(cursorTimer_);
    cursorTimer_ = -1;
    release();
}

void MidiEditor::onEvent(plat::Window* w, const plat::Event& e) {
    if (computer_keyboard::filter(this, e)) return;  // (its letters play notes while it is on)
    ui::Window::onEvent(w, e);
}

// --- time and pitch ---

double MidiEditor::originS() const { return (double)(clip->start - clip->offset) / sr(); }

void MidiEditor::windowS(double& from, double& to) const {
    from = (double)clip->offset / sr();
    to = (double)(clip->offset + clip->frames()) / sr();
}

double MidiEditor::lengthS() const {
    double from, to;
    windowS(from, to);
    double end = std::max(to, clip->lengthS);
    for (const Note& n : notes) end = std::max(end, n.end);
    return end + 2 * stepS() * 8;
}

double MidiEditor::xOf(double t) const { return KEYS_W + (t * pps - hbar->value()); }
double MidiEditor::tAt(double x) const { return (x - KEYS_W + hbar->value()) / pps; }

std::optional<Steps> MidiEditor::scale() const {
    const Timing& t = project.timing;
    for (auto& s : scales())
        if (s.first == t.scale) return scaleSteps(t.key, t.scale);
    return std::nullopt;
}

const std::vector<int>& MidiEditor::rows() const {
    const Timing& t = project.timing;
    std::vector<int64_t> key = {fold == "scale" ? 1 : fold == "used" ? 2 : 0, t.key, (int64_t)std::hash<std::string>()(t.scale)};
    for (int p : used) key.push_back(p);
    if (!rowsValid_ || key != rowsKey_) {
        std::vector<int> shown;
        std::optional<Steps> steps = scale();
        for (int p = 127; p >= 0; --p) {
            if (fold == "scale" && steps && !steps->empty() && !steps->count(p % 12)) continue;
            if (fold == "used" && !used.empty() && !used.count(p)) continue;
            shown.push_back(p);
        }
        rowsKey_ = key;
        rowsValid_ = true;
        shown_ = std::move(shown);
        rowOf_.clear();
        for (size_t i = 0; i < shown_.size(); ++i) rowOf_[shown_[i]] = (int)i;
    }
    return shown_;
}

std::optional<int> MidiEditor::rowOf(int pitch) const {
    rows();
    auto it = rowOf_.find(pitch);
    if (it == rowOf_.end()) return std::nullopt;
    return it->second;
}

int MidiEditor::rowAt(double y) const {
    return std::min((int)rows().size() - 1, std::max(0, (int)std::floor((y + vbar->value()) / rowH)));
}

std::optional<double> MidiEditor::yOf(double pitch) const {
    std::optional<int> row = rowOf((int)pitch);
    if (!row) return std::nullopt;
    return (double)*row * rowH - vbar->value();
}

int MidiEditor::pitchAt(double y) const { return rows()[rowAt(y)]; }

std::map<int, double> MidiEditor::movedPitches(const std::map<int, Note>& origin, int grabbed, int rowsDown) const {
    const std::vector<int>& rs = rows();
    int lo = 1 << 30, hi = -1;
    for (auto& kv : origin)
        if (auto r = rowOf((int)kv.second.pitch)) {
            lo = std::min(lo, *r);
            hi = std::max(hi, *r);
        }
    if (hi >= 0) rowsDown = std::min(std::max(rowsDown, -lo), (int)rs.size() - 1 - hi);
    std::optional<int> own = rowOf((int)origin.at(grabbed).pitch);
    double shift = own ? rs[*own + rowsDown] - origin.at(grabbed).pitch : -rowsDown;
    std::map<int, double> moved;
    for (auto& kv : origin) {
        std::optional<int> row = rowOf((int)kv.second.pitch);
        moved[kv.first] = row ? (double)rs[*row + rowsDown] : std::min(127.0, std::max(0.0, kv.second.pitch + shift));
    }
    return moved;
}

int MidiEditor::nearestRow(double pitch) const {
    const std::vector<int>& rs = rows();
    int best = 0;
    for (int i = 1; i < (int)rs.size(); ++i)
        if (std::fabs(rs[i] - pitch) < std::fabs(rs[best] - pitch)) best = i;
    return best;
}

void MidiEditor::visibleRows(int height, int& first, int& last) const {
    first = std::max(0, (int)std::floor((double)vbar->value() / rowH));
    last = std::min((int)rows().size(), (int)std::floor((double)(vbar->value() + height) / rowH) + 1);
}

bool MidiEditor::snapping(int mods) const { return project.timing.snaps(mods & plat::ModAlt); }

double MidiEditor::stepS(double at) const {
    const Timing& t = project.timing;
    at = originS() + std::max(0.0, at);
    return t.beats ? t.gridStep(pps, at).value() * t.beatSAt(at) : fmt::rulerStep(pps) / 4;
}

double MidiEditor::stepBeats(double at) const {
    return project.timing.gridStep(pps, originS() + std::max(0.0, at)).value();
}

double MidiEditor::shifted(double t, double beats) const {
    const Timing& timing = project.timing;
    double origin = originS();
    return timing.secondsAt(timing.beatAt(origin + t) + beats) - origin;
}

std::map<int, std::pair<double, double>> MidiEditor::movedTimes(const std::map<int, Note>& origin, int grabbed,
                                                                double dt, int mods) const {
    std::map<int, std::pair<double, double>> moved;
    if (snapping(mods)) {
        double start = origin.at(grabbed).start;
        double step = stepBeats(start);
        const Timing& timing = project.timing;
        double o = originS();
        double beats = pyRound((timing.beatAt(std::max(0.0, o + start + dt)) - timing.beatAt(o + start)) / step) * step;
        for (auto& kv : origin) moved[kv.first] = {shifted(kv.second.start, beats), shifted(kv.second.end, beats)};
    } else {
        for (auto& kv : origin) moved[kv.first] = {kv.second.start + dt, kv.second.end + dt};
    }
    double early = 1e300;
    for (auto& kv : moved) early = std::min(early, kv.second.first);
    if (early < 0)  // (nothing before the clip's start)
        for (auto& kv : moved) kv.second = {kv.second.first - early, kv.second.second - early};
    return moved;
}

double MidiEditor::snap(double t, int mods, bool floor) const {
    if (!snapping(mods)) return std::max(0.0, t);
    double origin = originS();  // (the project's grid, tempo changes and all)
    return std::max(0.0, project.timing.snap(origin + t, pps, floor) - origin);
}

int MidiEditor::noteAt(double x, double y, bool* atEnd) const {
    int pitch = pitchAt(y);
    for (int i = (int)notes.size() - 1; i >= 0; --i) {
        const Note& n = notes[i];
        double left = xOf(n.start), right = xOf(n.end);
        double width = std::max(NOTE_MIN_PX, right - left);
        if ((int)n.pitch == pitch && left <= x && x <= left + width + 1) {
            double edge = std::min<double>(EDGE_PX, (right - left) / 3);
            if (atEnd) *atEnd = edge >= 2 && right - x <= edge;
            return i;
        }
    }
    if (atEnd) *atEnd = false;
    return -1;
}

// --- view ---

void MidiEditor::fit() {
    double from, to;
    windowS(from, to);
    double width = std::max(200, view()->width() - KEYS_W - 20);
    pps = std::min(2000.0, std::max(20.0, width / std::max(0.5, to - from)));
    if (stepsView) {  // (the rows from the top)
        updateRanges();
        hbar->setValue((int)(from * pps));
        vbar->setValue(0);
        refresh();
        return;
    }
    double lo = 60, hi = 60;
    if (!notes.empty()) {
        lo = hi = notes[0].pitch;
        for (const Note& n : notes) {
            lo = std::min(lo, n.pitch);
            hi = std::max(hi, n.pitch);
        }
    }
    double middle = (hi + lo) / 2;
    updateRanges();
    hbar->setValue((int)(from * pps));
    vbar->setValue((int)(nearestRow(middle) * rowH - roll->height() / 2.0));
    refresh();
}

void MidiEditor::updateRanges() {
    int viewW = view()->width() - KEYS_W;
    hbar->setRange(0, std::max(0, (int)(lengthS() * pps) - viewW));
    hbar->setPageStep(std::max(1, viewW));
    if (stepsView) {  // (its rows, and "+ Row" under them)
        vbar->setRange(0, std::max(0, ((int)stepRows().size() + 1) * STEP_ROW_H - steps->height()));
        vbar->setPageStep(std::max(1, steps->height()));
        vbar->setSingleStep(STEP_ROW_H);
        return;
    }
    vbar->setRange(0, std::max(0, (int)rows().size() * rowH - roll->height()));
    vbar->setPageStep(std::max(1, roll->height()));
    vbar->setSingleStep(rowH);
}

void MidiEditor::zoom(double factor, double x) {
    double t = tAt(x);
    pps = std::min(4000.0, std::max(10.0, pps * factor));
    updateRanges();
    hbar->setValue((int)(t * pps - (x - KEYS_W)));
    refresh();
}

void MidiEditor::setRowHeight(int height) {
    height = std::min(MAX_ROW_H, std::max(MIN_ROW_H, height));
    if (stepsView) {  // (the piano roll's keys: the Steps view's scroll stays)
        rowH = height;
        return;
    }
    int middle = rowAt(roll->height() / 2.0);
    rowH = height;
    updateRanges();
    vbar->setValue((int)(middle * height + height / 2.0 - roll->height() / 2.0));
    refresh();
}

void MidiEditor::refold(const std::function<void()>& change) {
    if (stepsView) {  // (the piano roll's rows: the Steps view's scroll stays)
        change();
        syncScaleTools();
        refresh();
        return;
    }
    int middle = pitchAt(roll->height() / 2.0);
    change();
    updateRanges();
    vbar->setValue((int)(nearestRow(middle) * rowH + rowH / 2.0 - roll->height() / 2.0));
    syncScaleTools();
    refresh();
}

void MidiEditor::setExpanded(bool on) {
    refit_ = true;  // once it has its new size (resized()): the clip across the whole width
    if (on != expanded_) {
        expanded_ = on;
        if (native()) {
            native()->setMaximized(on);
        } else if (on) {  // (no window system: tests) as big as the screen
            normalGeometry_ = Rect(0, 0, width(), height());
            Rect scr = ui::App::get().screenAt(Point(0, 0));
            resizeWindow(scr.w, scr.h);
        } else if (!normalGeometry_.empty()) {
            resizeWindow(normalGeometry_.w, normalGeometry_.h);
        }
    }
    if (expandButton->isChecked() != on) expandButton->setChecked(on);
}

void MidiEditor::wheelAt(const ui::WheelEvent& e, double x) {
    float notches = e.dy != 0 ? e.dy : e.dx;  // (Alt may turn the wheel sideways)
    int delta = (int)std::lround(notches * 120);  // Qt's angleDelta
    if (delta == 0) return;
    if (e.mods & plat::ModAlt) {
        setRowHeight(rowH + (delta > 0 ? 2 : -2));
    } else if (e.mods & plat::ModCtrl) {
        zoom(delta > 0 ? config::ZOOM_STEP : 1 / config::ZOOM_STEP, x);
    } else if (e.mods & plat::ModShift) {
        hbar->setValue(hbar->value() - delta);
    } else {
        int half = (int)std::floor(delta / 2.0);
        vbar->setValue(vbar->value() - half);
    }
}

void MidiEditor::refresh() {
    roll->update();
    steps->update();
    velocity->update();
    ruler->update();
    syncNoteTools();
}

void MidiEditor::resized() {
    int h = wrappedHeight(hint->text(), hint->font(), (float)std::max(40, width() - 8 - 2 * hint->margin));
    if (hint->height() != h + 2 * hint->margin) hint->setFixedHeight(h + 2 * hint->margin);
    updateRanges();
    if (refit_) {
        refit_ = false;
        std::weak_ptr<int> alive = life_;
        ui::App::get().singleShot(0, [this, alive] {  // after the roll inside has its new size too
            if (alive.lock()) fit();
        });
    }
}

void MidiEditor::close() {
    if (cursorTimer_ >= 0) ui::App::get().cancelTimer(cursorTimer_);
    cursorTimer_ = -1;
    release();
    ui::Window::close();
}

void MidiEditor::updateTitle() {
    setTitle("Notes: " + clip->name + " \xe2\x80\x94 " + (clip->track ? clip->track->name : std::string()));
}

// --- the cursors ---

std::optional<double> MidiEditor::cursorX() const {
    if (!timeline.cursor) return std::nullopt;
    return xOf((double)timeline.cursor() / sr() - originS());
}

double MidiEditor::playheadX() const { return xOf((double)engine.heardPos() / sr() - originS()); }

void MidiEditor::followCursors() {
    std::optional<double> c = cursorX();
    std::pair<std::optional<int>, std::optional<int>> xs(c ? std::optional<int>((int)*c) : std::nullopt,
                                                         std::optional<int>((int)playheadX()));
    const Timing& t = project.timing;
    std::pair<int, std::string> sc(t.key, t.scale);
    std::vector<int64_t> clips = {  // (and the grid: Ctrl+1/2/4/5, the tempo, from the transport)
        (int64_t)std::llround(t.grid * 1e6), t.snapOn, t.beats, t.thirds, (int64_t)std::llround(t.bpm * 1e6),
        t.beatsPerBar, (int64_t)t.changes.size()};
    for (const ClipPtr& k : project.clips) {
        if (k->kind != Kind::Midi) continue;
        clips.insert(clips.end(), {(int64_t)(intptr_t)k.get(), k->start, k->offset, k->frames(),
                                   (int64_t)(intptr_t)k->notes.get(), k->notes ? (int64_t)k->notes->size() : 0,
                                   (int64_t)k->disabled, (int64_t)(intptr_t)k->played.get()});
    }
    bool changed = sc != seenScale_ || clips != seenClips_;
    if (sc != seenScale_) {
        seenScale_ = sc;
        syncScaleTools();
    }
    seenClips_ = std::move(clips);
    if (changed || xs != cursorXs_) {
        cursorXs_ = xs;
        refresh();
    }
}

void MidiEditor::paintCursorLines(Painter& p, int height) {
    std::optional<double> c = cursorX();
    if (c && *c >= KEYS_W) p.drawVLine((int)*c, 0, height + 1, theme::c("cursor"));
    double ph = playheadX();
    if (ph >= KEYS_W) p.drawVLine((int)ph, 0, height + 1, theme::c("playhead"));
}

void MidiEditor::moveCursor(double x, int mods) {
    if (!timeline.cursor || !timeline.setCursor) return;
    int64_t frame = std::max<int64_t>(0, (int64_t)pyRound((originS() + snap(tAt(x), mods)) * sr()));
    timeline.setCursor(frame);
    if (engine.playing && timeline.seek) timeline.seek(frame);
    followCursors();
}

// --- editing ---

void MidiEditor::commit() {
    if (!project.clips.contains(clip)) {
        close();
        return;
    }
    // by time; the selection (and the switched-off marks) stay on the same notes
    std::vector<int> order(notes.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
        if (notes[a].start != notes[b].start) return notes[a].start < notes[b].start;
        return notes[a].pitch < notes[b].pitch;
    });
    std::vector<Note> sorted;
    std::set<int> chosen, offs;
    sorted.reserve(notes.size());
    for (size_t i = 0; i < order.size(); ++i) {
        sorted.push_back(notes[order[i]]);
        if (selected.count(order[i])) chosen.insert((int)i);
        if (off.count(order[i])) offs.insert((int)i);
    }
    notes = std::move(sorted);
    selected = std::move(chosen);
    off = std::move(offs);
    std::vector<Note> playing, muted;
    for (size_t i = 0; i < notes.size(); ++i) (off.count((int)i) ? muted : playing).push_back(notes[i]);
    clip->notes = std::make_shared<const std::vector<Note>>(std::move(playing));
    clip->mutedNotes = muted.empty() ? nullptr : std::make_shared<const std::vector<Note>>(std::move(muted));
    bool grew = false;
    if (!notes.empty()) {
        double last = 0;
        for (const Note& n : notes) last = std::max(last, n.end);
        if (last > clip->lengthS)  // a note past the end: the clip grows (a loop: by whole bars, so it stays in time)
            clip->lengthS = clip->loop ? wholeBars(last) : last;
        int64_t end = (int64_t)pyRound((clip->loop ? wholeBars(last) : last) * sr());
        if (clip->length >= 0 && !clip->loop && end > clip->offset + clip->length) {  // (its window too: the note
            clip->length = end - clip->offset;                                          // is heard)
            grew = true;
        }
    }
    clip->prepare(sr());
    links::notesChanged(project, clip, sr());  // (linked MIDI clips: every one gets these notes)
    if (grew) edits::clearUnder(project, {clip});  // (as a drag of its end: it replaces what it now covers)
    takeUsed();
    updateRanges();
    refresh();
    if (onChange) onChange();
}

void MidiEditor::duplicate(int mods) {
    std::vector<int> idx;
    for (int i : selected)
        if (i < (int)notes.size()) idx.push_back(i);
    if (idx.empty()) return;
    double first = notes[idx[0]].start, last = notes[idx[0]].end;
    for (int i : idx) {
        first = std::min(first, notes[i].start);
        last = std::max(last, notes[i].end);
    }
    std::vector<Note> copies;
    if (snapping(mods)) {  // counted in beats, rounded up to the grid step: a pattern repeats in time
        const Timing& timing = project.timing;
        double o = originS(), step = stepBeats(first);
        double beats = std::max(1.0, std::ceil((timing.beatAt(o + last) - timing.beatAt(o + first)) / step - 1e-9)) * step;
        for (int i : idx) {
            Note n = notes[i];
            n.start = shifted(n.start, beats);
            n.end = shifted(n.end, beats);
            copies.push_back(n);
        }
    } else {
        for (int i : idx) {
            Note n = notes[i];
            n.start += last - first;
            n.end += last - first;
            copies.push_back(n);
        }
    }
    int from = (int)notes.size();
    for (size_t k = 0; k < idx.size(); ++k)
        if (off.count(idx[k])) off.insert(from + (int)k);  // (copies of switched-off ones too)
    notes.insert(notes.end(), copies.begin(), copies.end());
    selected.clear();
    for (int i = from; i < (int)notes.size(); ++i) selected.insert(i);
    commit();
}

void MidiEditor::quantize(double strength) {
    std::vector<int> indexes;
    for (int i : selected)
        if (i < (int)notes.size()) indexes.push_back(i);
    if (indexes.empty())
        for (int i = 0; i < (int)notes.size(); ++i) indexes.push_back(i);
    const Timing& timing = project.timing;
    double step = stepS(), origin = originS();
    auto onGrid = [&](double t) {
        return timing.beats ? timing.snap(origin + t, pps) - origin : pyRound((origin + t) / step) * step - origin;
    };
    bool ends = quantizeEnds && quantizeEnds->checked;
    for (int i : indexes) {
        Note& n = notes[i];
        double start = std::max(0.0, n.start + (onGrid(n.start) - n.start) * strength);
        double end = n.end + start - n.start;
        if (ends) end += (std::max(onGrid(end), start + stepS(start)) - end) * strength;
        n.start = start;
        n.end = std::max(end, start + 1e-3);
    }
    if (!indexes.empty()) commit();
}

void MidiEditor::reload() {
    drag = Drag();
    release();
    notes = clip->notes ? *clip->notes : std::vector<Note>();
    selected.clear();
    takeMuted();
    takeUsed();
    syncScaleTools();  // (the scale may have been undone too)
    updateRanges();
    refresh();
}

double MidiEditor::wholeBars(double t) const {
    const Timing& timing = project.timing;
    double o = originS(), at = o;
    while (at < o + t - 1e-6) at += timing.beatsPerBarAt(at) * timing.beatSAt(at);
    return at - o;
}

void MidiEditor::takeUsed() {
    used.clear();
    for (const Note& n : notes) used.insert((int)n.pitch);
}

void MidiEditor::takeMuted() {
    off.clear();
    if (!clip->mutedNotes) return;
    for (const Note& n : *clip->mutedNotes) {
        off.insert((int)notes.size());
        notes.push_back(n);
    }
}

void MidiEditor::toggleOff() {
    std::set<int> chosen;
    for (int i : selected)
        if (i < (int)notes.size()) chosen.insert(i);
    if (chosen.empty()) return;
    bool allOff = std::includes(off.begin(), off.end(), chosen.begin(), chosen.end());
    for (int i : chosen) {
        if (allOff) off.erase(i);
        else off.insert(i);
    }
    commit();
}

void MidiEditor::deleteNotes(const std::set<int>& indexes) {
    if (indexes.empty()) return;
    std::vector<Note> kept;
    std::set<int> offs;
    for (size_t i = 0; i < notes.size(); ++i) {
        if (indexes.count((int)i)) continue;
        if (off.count((int)i)) offs.insert((int)kept.size());
        kept.push_back(notes[i]);
    }
    notes = std::move(kept);
    off = std::move(offs);
    selected.clear();
    commit();
}

void MidiEditor::copyNotes(bool cut) {
    std::set<int> chosen;
    for (int i : selected)
        if (i < (int)notes.size()) chosen.insert(i);
    if (chosen.empty()) return;
    double first = 1e300;
    for (int i : chosen) first = std::min(first, notes[i].start);
    noteClipboard.clear();
    for (int i : chosen)
        noteClipboard.push_back({notes[i].start - first, notes[i].end - first, notes[i].pitch, notes[i].velocity,
                                 off.count(i) > 0, notes[i]});
    if (cut) deleteNotes(chosen);
}

void MidiEditor::pasteNotes() {
    if (noteClipboard.empty()) return;
    double lo, hi;
    windowS(lo, hi);
    double at = lo;
    bool over = false;
    if (timeline.cursor) {
        double cursor = (double)timeline.cursor() / sr() - originS();
        if (lo <= cursor && cursor < hi) {
            at = cursor;
            over = true;
        }
    }
    if (!over)
        for (const Note& n : notes) at = std::max(at, n.end);
    int start = (int)notes.size();
    for (size_t k = 0; k < noteClipboard.size(); ++k) {
        const CopiedNote& c = noteClipboard[k];
        Note n = c.note;  // (its own properties)
        n.start = c.start + at;
        n.end = c.end + at;
        n.pitch = c.pitch;
        n.velocity = c.velocity;
        notes.push_back(n);
        if (c.off) off.insert(start + (int)k);
    }
    selected.clear();
    for (int i = start; i < (int)notes.size(); ++i) selected.insert(i);
    commit();
}

std::set<int> MidiEditor::notesIn(const RectF& box) const {
    double t0 = tAt(box.x), t1 = tAt(box.right());
    std::set<int> found;
    for (int i = 0; i < (int)notes.size(); ++i) {
        const Note& n = notes[i];
        std::optional<double> y = yOf(n.pitch);
        if (y && n.start <= t1 && n.end >= t0 && *y <= box.bottom() && *y + rowH >= box.y) found.insert(i);
    }
    return found;
}

void MidiEditor::startCopyDrag(Point pos, int index) {
    std::set<int> chosen;
    for (int i : selected)
        if (i < (int)notes.size()) chosen.insert(i);
    chosen.insert(index);
    int first = (int)notes.size();
    std::map<int, int> copies;
    int k = 0;
    for (int i : chosen) copies[i] = first + k++;
    for (int i : chosen) notes.push_back(notes[i]);
    for (int i : chosen)
        if (off.count(i)) off.insert(copies[i]);
    selected.clear();
    for (auto& kv : copies) selected.insert(kv.second);
    Drag d;
    d.kind = Drag::Move;
    d.startPos = pos;
    d.index = copies[index];
    for (int i : chosen) d.origin[copies[i]] = notes[copies[i]];
    d.changed = true;
    drag = d;
    hold((int)notes[copies[index]].pitch);
}

void MidiEditor::nudge(int key, bool shift, int mods) {
    std::vector<int> chosen;
    for (int i : selected)
        if (i < (int)notes.size()) chosen.push_back(i);
    if (chosen.empty()) return;
    std::map<int, Note> before;
    for (int i : chosen) before[i] = notes[i];
    bool later = key == plat::Key_Right;
    bool sideways = key == plat::Key_Left || key == plat::Key_Right;
    if (sideways && !shift) {  // a grid step earlier or later
        double dt = stepS(before[chosen[0]].start) * (later ? 1 : -1);
        auto moved = movedTimes(before, chosen[0], dt, mods);
        bool same = true;
        double early = 1e300;
        for (auto& kv : moved) {
            early = std::min(early, kv.second.first);
            if (kv.second.first != before[kv.first].start || kv.second.second != before[kv.first].end) same = false;
        }
        if (early < 0 || same) return;
        for (auto& kv : moved) {
            notes[kv.first].start = kv.second.first;
            notes[kv.first].end = kv.second.second;
        }
    } else if (sideways) {  // shorter or longer by a grid step
        for (auto& kv : before) {
            const Note& n = kv.second;
            double step = stepS(later ? n.end : n.start);
            if (later) notes[kv.first].end = n.end + step;
            else notes[kv.first].end = std::max(n.end - step, n.start + std::min(step, n.end - n.start));  // (never shorter
        }                                                                                             // than a step)
    } else if (shift) {  // an octave (a scale's notes are the same in every octave)
        int up = key == plat::Key_Up ? 12 : -12;
        for (auto& kv : before)
            if (kv.second.pitch + up < 0 || kv.second.pitch + up > 127) return;
        for (int i : chosen) notes[i].pitch += up;
    } else {  // a semitone, or a row when folded
        auto pitches = movedPitches(before, chosen[0], key == plat::Key_Up ? -1 : 1);
        for (int i : chosen) notes[i].pitch = pitches[i];
    }
    commit();
    int top = -1;
    for (int i : selected)
        if (i < (int)notes.size() && (top < 0 || notes[i].pitch > notes[top].pitch)) top = i;
    if (top >= 0) soundBriefly((int)notes[top].pitch, (int)notes[top].velocity, NOTE_SOUND_S);  // (each press heard)
}

// --- hearing notes ---

std::shared_ptr<live::LiveNotes> MidiEditor::live() const { return engine.liveNotes(clip->track.get()); }

bool MidiEditor::builtin() const { return !clip->track || !plugin_audio::playsPlugins(*clip->track); }

void MidiEditor::soundBriefly(int pitch, int velocity, double seconds) {
    midi_input::playLive(engine, clip->track, pitch, velocity, seconds);
}

void MidiEditor::hold(int pitch) {
    if (held && *held == pitch) return;
    release();
    sounding = midi_input::playLive(engine, clip->track, pitch, lastVelocity);  // (through its MIDI effects)
    held = pitch;
}

void MidiEditor::release() {
    if (held) {
        for (int p : sounding) live()->noteOff(p);
        sounding.clear();
        held.reset();
    }
}

void MidiEditor::glide(int pitch) {
    if (held && pitch != *held) {
        int step = pitch > *held ? 1 : -1;
        for (int passed = *held + step; passed != pitch; passed += step) {
            if (!rowOf(passed)) continue;  // (folded away)
            soundBriefly(passed, lastVelocity, GLIDE_S);
        }
    }
    hold(pitch);
}

bool MidiEditor::keyPress(ui::KeyEvent& e) {
    bool ctrl = e.mods & plat::ModCtrl;
    if (e.key == plat::Key_Delete || e.key == plat::Key_Backspace) {
        deleteNotes(selected);
    } else if (e.key == 'A' && ctrl) {  // (the notes shown: not in folded rows)
        selected.clear();
        for (size_t i = 0; i < notes.size(); ++i)
            if (shows(notes[i])) selected.insert((int)i);
        refresh();
    } else if (e.key == 'G' && !(e.mods & (plat::ModCtrl | plat::ModAlt | plat::ModShift | plat::ModMeta))) {
        toggleSlide();
    } else if (e.key == 'D' && ctrl) {
        duplicate(e.mods);
    } else if (e.key == 'U' && ctrl) {
        quantize(1.0);
    } else if (e.key == '0' && !(e.mods & (plat::ModCtrl | plat::ModAlt | plat::ModShift | plat::ModMeta))) {
        toggleOff();
    } else if ((e.key == 'C' || e.key == 'X') && ctrl) {
        copyNotes(e.key == 'X');
    } else if (e.key == 'V' && ctrl) {
        pasteNotes();
    } else if (e.key == plat::Key_Left || e.key == plat::Key_Right || e.key == plat::Key_Up ||
               e.key == plat::Key_Down) {
        nudge(e.key, e.mods & plat::ModShift, e.mods);
    } else if (e.key == plat::Key_Escape) {
        selected.clear();
        refresh();
    } else {
        return false;
    }
    return true;
}

Color MidiEditor::color() const {
    std::string c, tone;
    colors::clipColor(*clip, c, tone);
    return colors::clipColors(c).fill;
}

// --- ghost notes ---

ui::Button* MidiEditor::buildGhostsButton() {
    ghostsButton = ui::toolButton(this, "Ghosts \xe2\x96\xbe", nullptr,
                                  "Show other clips' notes, faint, behind this clip's (as they play), to write against "
                                  "them. Right-click one to edit its clip");
    ui::Menu* menu = widgets::ownedMenu(ghostsButton);
    for (auto& m : ghostModes()) {
        std::string mode = m.first;
        ui::Action* a = menu->addAction(m.second, [this, mode] { setGhostMode(mode); });
        a->checkable = true;
        a->checked = mode == ghostMode;
        a->group = 1;
        ghostActions[mode] = a;
    }
    ghostsButton->setMenu(menu);
    return ghostsButton;
}

void MidiEditor::setGhostMode(const std::string& mode) {
    ghostMode = mode;
    checkOnly(ghostActions, mode);
    settings::set("options/ghost_notes", Json(mode));
    refresh();
}

const std::vector<MidiEditor::Ghost>& MidiEditor::ghosts() {
    static const std::vector<Ghost> none;
    if (ghostMode == "off") return none;
    std::vector<ClipPtr> clips;
    for (const ClipPtr& c : project.clips)
        if (c->kind == Kind::Midi && c != clip && !c->disabled && c->played &&
            (ghostMode == "all" || c->track == clip->track))
            clips.push_back(c);
    std::vector<int64_t> key = {ghostMode == "all" ? 1 : 2, clip->start, clip->offset, sr()};
    std::vector<std::string> hexes;
    for (const ClipPtr& c : clips) {
        std::string hex, tone;
        colors::clipColor(*c, hex, tone);
        hexes.push_back(hex);
        key.insert(key.end(), {(int64_t)(intptr_t)c.get(), c->start, c->offset, c->length,
                               (int64_t)(intptr_t)c->played.get(), (int64_t)std::hash<std::string>()(hex)});
    }
    if (!ghostKeyValid_ || key != ghostKey_) {
        std::vector<Ghost> found;
        double origin = originS();
        for (size_t k = 0; k < clips.size(); ++k) {
            const ClipPtr& c = clips[k];
            double lo = (double)c->offset / sr(), hi = (double)(c->offset + c->frames()) / sr();
            double shift = (double)(c->start - c->offset) / sr() - origin;
            Ghost g;
            g.clip = c;
            g.color = hexes[k];
            for (const Note& n : *c->played) {
                if (!(n.start < hi && n.end > lo)) continue;
                Note m = n;
                m.start = std::max(n.start, lo) + shift;
                m.end = std::min(n.end, hi) + shift;
                g.notes.push_back(m);
            }
            if (!g.notes.empty()) found.push_back(std::move(g));
        }
        ghostKey_ = key;
        ghostKeyValid_ = true;
        ghostList_ = std::move(found);
    }
    return ghostList_;
}

ClipPtr MidiEditor::ghostAt(double x, double y) {
    double t = tAt(x);
    int pitch = pitchAt(y);
    const std::vector<Ghost>& gs = ghosts();
    for (auto it = gs.rbegin(); it != gs.rend(); ++it)
        for (const Note& n : it->notes)
            if (n.start <= t && n.end >= t && (int)n.pitch == pitch) return it->clip;
    return nullptr;
}

void MidiEditor::editOther(const ClipPtr& other) {
    if (timeline.editNotes) timeline.editNotes(other);  // (the main window opens, or raises, its editor)
}

// --- the chord stamp ---

ui::Button* MidiEditor::buildStampButton() {
    stampButton = ui::toolButton(this, "Stamp \xe2\x96\xbe", nullptr,
                                 "Stamp: a click places a whole chord, rooted on the key clicked (Off: one note). "
                                 "\"In the scale\" chords use the song's scale (choose one under Scale)");
    ui::Menu* menu = widgets::ownedMenu(stampButton);
    for (const std::string& name : stamps()) {
        ui::Action* a = menu->addAction(name, [this, name] { setStamp(name); });
        a->checkable = true;
        a->group = 2;
        stampActions[name] = a;
        if (name == "Off") menu->addSeparator();
    }
    menu->aboutToShow = [this](ui::Menu*) { syncStamp(); };
    stampButton->setMenu(menu);
    syncStamp();
    return stampButton;
}

void MidiEditor::setStamp(const std::string& name) {
    stamp = name;
    syncStamp();
}

void MidiEditor::syncStamp() {
    bool onScale = scale().has_value();
    for (auto& kv : stampActions) kv.second->enabled = onScale || !startsWith(kv.first, "In the scale");
    if (startsWith(stamp, "In the scale") && !onScale) stamp = "Off";  // (the scale went: nothing to build those from)
    checkOnly(stampActions, stamp);
    std::string shown = startsWith(stamp, "In the scale: ") ? stamp.substr(14) : stamp;
    stampButton->setText(stamp == "Off" ? "Stamp \xe2\x96\xbe" : shown + " \xe2\x96\xbe");
}

std::vector<int> MidiEditor::stamped(int root) const {
    if (stamp == "Off") return {root};
    std::vector<int> chord;
    if (startsWith(stamp, "In the scale")) {
        std::optional<Steps> steps = scale();
        chord = steps ? inScaleChord(root, *steps, stamp.size() >= 3 && stamp.substr(stamp.size() - 3) == "7th" ? 4 : 3)
                      : std::vector<int>{root};
    } else {
        chord.push_back(root);
        for (auto& c : chords())
            if (c.first == stamp)
                for (int i : c.second)
                    if (root + i >= 0 && root + i <= 127) chord.push_back(root + i);
    }
    std::vector<int> shown;  // (rows folded away get none: a note there couldn't be seen)
    for (int p : chord)
        if (p == root || yOf(p)) shown.push_back(p);
    return shown;
}

// --- the song's scale ---

void MidiEditor::buildScaleTools(ui::HBox* tools) {
    const char* tip = "The song's key and scale (saved with the project): notes outside it are shaded";
    ui::Label* label = add<ui::Label>("Scale");
    label->setToolTip(tip);
    tools->addWidget(label);
    keyBox = add<ui::ComboBox>();
    for (int k = 0; k < 12; ++k) keyBox->addItem(KEYS[k]);
    scaleBox = add<ui::ComboBox>();
    scaleBox->addItem("Off", "");
    for (auto& s : scales()) scaleBox->addItem(s.first, s.first);
    for (ui::ComboBox* box : {keyBox, scaleBox}) {
        box->setFocusPolicy(ui::FocusPolicy::None);  // (the keys stay the roll's: Delete, Ctrl+D…)
        box->setToolTip(tip);
        box->currentIndexChanged.connect([this](int) { scaleChosen(); });
        tools->addWidget(box);
    }
    foldButton = ui::toolButton(this, "Fold \xe2\x96\xbe", nullptr,
                                "Show only some rows: the scale's notes (every row is a right one), or the pitches the "
                                "clip uses (drums)");
    ui::Menu* menu = widgets::ownedMenu(foldButton);
    for (auto& f : folds()) {
        std::string which = f.first;
        ui::Action* a = menu->addAction(f.second, [this, which] { setFold(which); });
        a->checkable = true;
        a->group = 3;
        foldActions[which] = a;
    }
    foldButton->setMenu(menu);
    tools->addWidget(foldButton);
    fitButton = ui::toolButton(this, "Fit to scale", [this] { fitToScale(); },
                               "Move the selected notes (none selected: all) to the nearest note of the scale");
    tools->addWidget(fitButton);
}

void MidiEditor::syncScaleTools() {
    const Timing& t = project.timing;
    keyBox->setCurrentIndexSilently(std::min(11, std::max(0, t.key)));
    scaleBox->setCurrentIndexSilently(std::max(0, scaleBox->findData(t.scale)));
    bool on = scale().has_value();
    if (!stampActions.empty()) syncStamp();
    keyBox->setEnabled(on);
    fitButton->setEnabled(on);
    foldActions["scale"]->enabled = on;
    if (fold == "scale" && !on) fold = "off";  // (no scale any more: nothing to fold to)
    checkOnly(foldActions, fold);
    foldButton->setText(fold == "off" ? "Fold \xe2\x96\xbe" : fold == "scale" ? "Folded: scale \xe2\x96\xbe"
                                                                               : "Folded: used \xe2\x96\xbe");
    foldButton->updateGeometry();
}

void MidiEditor::scaleChosen() {
    int key = keyBox->currentIndex();
    std::string sc = scaleBox->currentData();
    refold([this, key, sc] {
        project.timing.key = key;
        project.timing.scale = sc;
    });
}

void MidiEditor::setFold(const std::string& which) {
    refold([this, which] {
        fold = which;
        takeUsed();
    });
}

void MidiEditor::fitToScale() {
    std::optional<Steps> steps = scale();
    if (!steps) return;
    std::vector<int> indexes;
    for (int i : selected)
        if (i < (int)notes.size()) indexes.push_back(i);
    if (indexes.empty())
        for (int i = 0; i < (int)notes.size(); ++i) indexes.push_back(i);
    bool changed = false;
    for (int i : indexes) {
        double pitch = nearestInScale((int)notes[i].pitch, *steps);
        changed |= pitch != notes[i].pitch;
        notes[i].pitch = pitch;
    }
    if (changed) commit();
}

// =================================================================================================
// PianoRoll
// =================================================================================================

PianoRoll::PianoRoll(MidiEditor* editor) : ed_(editor) {
    setMouseTracking(true);
    setFocusPolicy(ui::FocusPolicy::Strong);
    setMinimumHeight(200);
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
}

bool PianoRoll::overridesShortcut(const ui::KeyEvent& e) const {
    bool ctrl = e.mods & plat::ModCtrl;
    if (e.mods & plat::ModAlt) return false;
    switch (e.key) {
    case plat::Key_Delete: case plat::Key_Backspace: case plat::Key_Left: case plat::Key_Right: case plat::Key_Up:
    case plat::Key_Down: case plat::Key_Escape:
        return !ctrl;
    case '0': case 'G':
        return !ctrl && !(e.mods & plat::ModShift);
    case 'A': case 'C': case 'D': case 'U': case 'V': case 'X':
        return ctrl && !(e.mods & plat::ModShift);
    default:
        return false;
    }
}

void PianoRoll::paint(Painter& p) {
    MidiEditor& ed = *ed_;
    int w = width(), h = height();
    const std::vector<int>& rows = ed.rows();
    std::optional<Steps> steps = ed.scale();
    int root = ed.project.timing.key;
    Color rootTint = theme::c("selected_note").withAlpha(ROOT_TINT_ALPHA);
    Color black = theme::c("lane"), white = theme::c("lane_alt"), shade = theme::c("roll_shade");
    int first, last;
    ed.visibleRows(h, first, last);
    for (int i = first; i < last; ++i) {  // rows: black keys darker, a line between octaves; outside the scale shaded
        int pitch = rows[i], y = i * ed.rowH - ed.vbar->value();
        Rect row(KEYS_W, y, w, ed.rowH);
        p.fillRect(row, isBlackKey(pitch) ? black : white);
        if (steps) {
            if (!steps->count(pitch % 12)) p.fillRect(row, shade);
            else if (pitch % 12 == root) p.fillRect(row, rootTint);
        }
        if (i > 0 && rows[i - 1] / 12 != pitch / 12) p.fillRect(Rect(KEYS_W, y, w, 1), grid(timing::BAR));
    }
    int below = (int)rows.size() * ed.rowH - ed.vbar->value();  // (folded: no rows under the last one)
    if (below < h) p.fillRect(Rect(KEYS_W, below, w, h - below), theme::c("ruler_bg"));
    paintGrid(p, std::min(h, below));
    double lo, hi;  // shade what the clip doesn't play
    ed.windowS(lo, hi);
    p.fillRect(aliased(KEYS_W, 0, std::max(0.0, ed.xOf(lo) - KEYS_W), h), shade);
    p.fillRect(aliased(ed.xOf(hi), 0, w, h), shade);
    paintGhosts(p, w);
    paintRepeats(p, w, h);
    Color color = ed.color();
    Color selectedC = theme::c("selected_note");
    for (size_t i = 0; i < ed.notes.size(); ++i) {
        const Note& n = ed.notes[i];
        std::optional<double> y = ed.yOf(n.pitch);
        if (!y) continue;  // (folded away)
        RectF r((float)ed.xOf(n.start), (float)(*y + 1), (float)std::max(NOTE_MIN_PX, (n.end - n.start) * ed.pps),
                (float)(ed.rowH - 2));
        if (r.right() < KEYS_W || r.x > w || r.bottom() < 0 || r.y > h) continue;
        paintNoteBox(p, r, n, color, ed.selected.count((int)i) > 0, ed.off.count((int)i) > 0);
    }
    std::vector<note_props::Slide> slid = note_props::slides(ed.notes);  // sliding notes: a tail into each
    for (size_t i = 0; i < ed.notes.size(); ++i) {
        const Note& n = ed.notes[i];
        std::optional<double> y = n.slide ? ed.yOf(n.pitch) : std::nullopt;
        if (!y) continue;
        float x = (float)ed.xOf(n.start), mid = (float)(*y + ed.rowH / 2.0);
        if (x < KEYS_W || x > w + 20) continue;
        float from = mid + (float)ed.rowH;  // (from below, when it has nothing to slide from)
        if (slid[i].from >= 0) {
            const Note& before = ed.notes[(size_t)slid[i].from];
            std::optional<double> yb = ed.yOf(before.pitch);
            from = yb ? (float)(*yb + ed.rowH / 2.0) : mid + (before.pitch > n.pitch ? -1.0f : 1.0f) * ed.rowH;
        }
        float most = 3.0f * (float)ed.rowH;  // (a long way: just its start)
        from = std::min(mid + most, std::max(mid - most, from));
        paintSlideTail(p, PointF(x - 14, from), PointF(x + 1, mid), color.lighter(170));
    }
    ed.paintCursorLines(p, h);
    if (ed.drag.kind == MidiEditor::Drag::Box) {  // (Shift+drag: the selection box)
        RectF box = normalized(ed.drag.startPos, ed.drag.pos);
        p.fillRect(box, selectedC.withAlpha(40));
        dashedBox(p, box, selectedC);
    }
    paintKeys(p, h);
}

void PianoRoll::paintRepeats(Painter& p, int w, int h) {
    MidiEditor& ed = *ed_;
    const Clip& clip = *ed.clip;
    if (!clip.loop || clip.lengthS <= 0) return;
    double lo, hi;
    ed.windowS(lo, hi);
    Color line = theme::c("selected_note");
    for (int k = 1; k * clip.lengthS < hi && k < 4096; ++k) {  // where each pass ends
        int x = (int)ed.xOf(k * clip.lengthS);
        if (KEYS_W <= x && x <= w) p.drawDashedLine(PointF(x + 0.5f, 0), PointF(x + 0.5f, (float)h), line, 1, 4, 2);
    }
    Color fill = ed.color().withAlpha(GHOST_ALPHA);  // the passes after the first, faintly (as written: before
    double t0 = ed.tAt(KEYS_W), t1 = ed.tAt(w);         // the track's MIDI effects)
    for (const Note& n : clip.repeated(ed.sr())) {
        if (n.start < clip.lengthS) continue;
        std::optional<double> y = ed.yOf(n.pitch);
        if (y && n.end > t0 && n.start < t1 && -ed.rowH < *y && *y < h) {
            double x = ed.xOf(n.start);
            p.fillRect(RectF((float)x, (float)(*y + 1), (float)std::max(3.0, ed.xOf(n.end) - x), (float)(ed.rowH - 2)),
                       fill);
        }
    }
}

void PianoRoll::paintGhosts(Painter& p, int w) {
    MidiEditor& ed = *ed_;
    double t0 = ed.tAt(KEYS_W), t1 = ed.tAt(w);
    for (const MidiEditor::Ghost& g : ed.ghosts()) {
        Color fill = colors::clipColors(g.color).fill.lighter(125).withAlpha(GHOST_ALPHA);
        for (const Note& n : g.notes) {
            if (!(n.end > t0 && n.start < t1)) continue;
            std::optional<double> y = ed.yOf(n.pitch);
            if (y && -ed.rowH < *y && *y < height()) {
                double x = ed.xOf(n.start);
                p.fillRect(RectF((float)x, (float)(*y + 2), (float)std::max(2.0, ed.xOf(n.end) - x),
                                 (float)std::max(1, ed.rowH - 4)),
                           fill);
            }
        }
    }
}

void PianoRoll::paintGrid(Painter& p, int h) {
    MidiEditor& ed = *ed_;
    double t0 = ed.tAt(KEYS_W), t1 = ed.tAt(width());
    const Timing& timing = ed.project.timing;
    if (h <= 0) return;
    if (timing.beats) {
        double origin = ed.originS();
        for (const GridLine& line : timing.lines(std::max(0.0, origin + t0), origin + t1, ed.pps)) {
            int x = (int)ed.xOf(line.seconds - origin);
            p.drawVLine(x, 0, h + 1, grid(line.strength));
        }
    } else {  // the ruler's steps from 0 s (as the ruler counts them), and fainter quarters of them (where
              // Quantize and new notes go: stepS)
        double step = fmt::rulerStep(ed.pps) / 4, origin = ed.originS();
        for (int k = (int)(std::max(0.0, origin + t0) / step); k < (int)((origin + t1) / step) + 2; ++k) {
            int x = (int)ed.xOf(k * step - origin);
            p.drawVLine(x, 0, h + 1, k % 4 == 0 ? theme::c("grid") : grid(0));
        }
    }
}

void PianoRoll::paintKeys(Painter& p, int h) {
    MidiEditor& ed = *ed_;
    const std::vector<int>& rows = ed.rows();
    std::optional<Steps> steps = ed.scale();
    int root = ed.project.timing.key;
    bool folded = rows.size() < 128;
    int below = (int)rows.size() * ed.rowH - ed.vbar->value();
    p.fillRect(Rect(0, 0, KEYS_W, std::min(h, below)), theme::c("keys_white"));
    if (below < h) p.fillRect(Rect(0, below, KEYS_W, h - below), theme::c("ruler_bg"));  // (folded: no keys there)
    Font font = theme::uiFont();
    int first, last;
    ed.visibleRows(h, first, last);
    for (int i = first; i < last; ++i) {
        int pitch = rows[i], y = i * ed.rowH - ed.vbar->value();
        if (isBlackKey(pitch))  // (folded, narrower: every key may be named, right of it)
            p.fillRect(aliased(0, y, KEYS_W * (folded ? 0.4 : 0.62), ed.rowH), theme::c("keys_black"));
        if (!isBlackKey(pitch) || folded)  // (folded: a line under every key, none are next to theirs)
            p.fillRect(Rect(0, y + ed.rowH - 1, KEYS_W, 1), theme::c("keys_line"));
        if (ed.held && *ed.held == pitch) p.fillRect(Rect(0, y, KEYS_W, ed.rowH), theme::c("key_sounding"));
        bool named = pitch % 12 == 0 || folded || (steps && pitch % 12 == root);
        if (named && ed.rowH >= 9)
            p.drawText(RectF(0, (float)y, KEYS_W - 4, (float)ed.rowH), AlignRight | AlignVCenter, noteName(pitch), font,
                       theme::c("keys_label"));
    }
    p.fillRect(Rect(KEYS_W - 1, 0, 1, h), theme::c("ruler_bg"));
}

bool PianoRoll::mousePress(ui::MouseEvent& e) {
    MidiEditor& ed = *ed_;
    using K = MidiEditor::Drag::Kind;
    Point pos = e.pos;
    if (pos.y + ed.vbar->value() >= (int)ed.rows().size() * ed.rowH) return true;  // (below the last row, folded)
    if (pos.x < KEYS_W) {  // a key: it sounds while held; dragging over the keys plays each one passed
        if (e.button == plat::LeftButton) {
            ed.hold(ed.pitchAt(pos.y));
            ed.drag = MidiEditor::Drag();
            ed.drag.kind = K::Keys;
            ed.refresh();
        }
        return true;
    }
    bool atEnd = false;
    int index = ed.noteAt(pos.x, pos.y, &atEnd);
    bool ctrl = e.mods & plat::ModCtrl;
    if (e.button == plat::RightButton) {
        if (index >= 0) {
            ed.deleteNotes({index});
        } else if (ClipPtr other = ed.ghostAt(pos.x, pos.y)) {  // a ghost note: its clip's editor
            ui::Menu menu;
            std::string track = other->track ? " (" + other->track->name + ")" : "";
            std::weak_ptr<Clip> keep = other;
            MidiEditor* editor = &ed;
            menu.addAction("Edit the notes of \xe2\x80\x9c" + other->name + "\xe2\x80\x9d" + track,
                           [editor, keep] { if (auto c = keep.lock()) editor->editOther(c); });
            menu.exec(e.globalPos, this);
        }
        return true;
    }
    if (e.button != plat::LeftButton) return true;
    if (index < 0 && (e.mods & plat::ModShift)) {  // Shift: a selection box (Ctrl too: adds to it)
        MidiEditor::Drag d;
        d.kind = K::Box;
        d.startPos = d.pos = pos;
        if (ctrl) d.base = ed.selected;
        ed.selected = d.base;
        ed.drag = d;
        ed.refresh();
        return true;
    }
    if (index >= 0 && ctrl) {  // Ctrl: a click selects or not, a drag copies (mouseMove)
        MidiEditor::Drag d;
        d.kind = K::Copy;
        d.startPos = pos;
        d.index = index;
        ed.drag = d;
        return true;
    }
    bool changed;
    if (index < 0) {  // an empty spot: a new note (or the stamp's chord), a grid step long; dragging lengthens it
        if (!ctrl) ed.selected.clear();
        double start = ed.snap(ed.tAt(pos.x), e.mods, true);
        std::vector<int> pitches = ed.stamped(ed.pitchAt(pos.y));
        int first = (int)ed.notes.size();
        for (int pitch : pitches) {
            Note n;
            n.start = start;
            n.end = start + ed.stepS(start);
            n.pitch = pitch;
            n.velocity = ed.lastVelocity;
            ed.notes.push_back(n);
        }
        index = first;  // (the root: the one held under the mouse)
        atEnd = true;
        for (int i = first; i < (int)ed.notes.size(); ++i) ed.selected.insert(i);
        changed = true;
    } else {
        if (!ed.selected.count(index)) ed.selected = {index};
        ed.lastVelocity = (int)ed.notes[index].velocity;
        changed = false;
    }
    MidiEditor::Drag d;
    d.kind = atEnd ? K::Resize : K::Move;
    d.startPos = pos;
    d.index = index;
    for (int i : ed.selected)
        if (i < (int)ed.notes.size()) d.origin[i] = ed.notes[i];
    d.changed = changed;
    ed.drag = d;
    ed.hold((int)ed.notes[index].pitch);  // the grabbed (or new) note sounds while held
    ed.refresh();
    return true;
}

bool PianoRoll::mouseMove(ui::MouseEvent& e) {
    MidiEditor& ed = *ed_;
    Point pos = e.pos;
    using K = MidiEditor::Drag::Kind;
    if (ed.drag.kind == K::None) {
        bool atEnd = false;
        int index = pos.x >= KEYS_W ? ed.noteAt(pos.x, pos.y, &atEnd) : -1;
        setCursor(index < 0 ? ui::Cursor::Arrow : atEnd ? ui::Cursor::SizeHor : ui::Cursor::OpenHand);
        return true;
    }
    if (ed.drag.kind == K::Keys) {
        ed.glide(ed.pitchAt(pos.y));
        ed.refresh();
        return true;
    }
    if (ed.drag.kind == K::Box) {
        ed.drag.pos = pos;
        ed.selected = ed.drag.base;
        for (int i : ed.notesIn(normalized(ed.drag.startPos, pos))) ed.selected.insert(i);
        ed.refresh();
        return true;
    }
    if (ed.drag.kind == K::Copy) {
        if (std::max(std::abs(pos.x - ed.drag.startPos.x), std::abs(pos.y - ed.drag.startPos.y)) < config::DRAG_THRESHOLD_PX)
            return true;
        ed.startCopyDrag(ed.drag.startPos, ed.drag.index);
    }
    if (ed.drag.kind != K::Move && ed.drag.kind != K::Resize) return true;
    MidiEditor::Drag& d = ed.drag;
    if (d.origin.empty() || !d.origin.count(d.index)) return true;
    if (d.kind == K::Move) {
        auto times = ed.movedTimes(d.origin, d.index, ed.tAt(pos.x) - ed.tAt(d.startPos.x), e.mods);
        auto pitches = ed.movedPitches(d.origin, d.index, ed.rowAt(pos.y) - ed.rowAt(d.startPos.y));
        for (auto& kv : d.origin) {
            if (kv.first >= (int)ed.notes.size()) continue;
            Note n = kv.second;
            n.start = times[kv.first].first;
            n.end = times[kv.first].second;
            n.pitch = pitches[kv.first];
            ed.notes[kv.first] = n;
        }
        if (d.index < (int)ed.notes.size()) ed.glide((int)ed.notes[d.index].pitch);  // hear the pitch it is moved to
    } else {
        const Note& grabbed = d.origin[d.index];
        double newEnd = ed.snap(ed.tAt(pos.x), e.mods);
        double shortest = ed.snapping(e.mods) ? ed.stepS(grabbed.start) : 0.01;
        double delta = std::max(newEnd, grabbed.start + shortest) - grabbed.end;
        for (auto& kv : d.origin) {
            if (kv.first >= (int)ed.notes.size()) continue;
            Note n = kv.second;
            n.end = std::max(n.end + delta, n.start + shortest);
            ed.notes[kv.first] = n;
        }
    }
    d.changed = true;
    ed.refresh();
    return true;
}

bool PianoRoll::mouseRelease(ui::MouseEvent& e) {
    MidiEditor& ed = *ed_;
    MidiEditor::Drag d = ed.drag;
    ed.drag = MidiEditor::Drag();
    ed.release();
    using K = MidiEditor::Drag::Kind;
    if (d.kind == K::Copy) {  // Ctrl+click without dragging: in or out of the selection
        if (ed.selected.count(d.index)) ed.selected.erase(d.index);
        else ed.selected.insert(d.index);
    } else if ((d.kind == K::Move || d.kind == K::Resize) && d.changed) {
        ed.commit();  // something changed: into the clip
    }
    if (ui::Widget::alive(this)) ed.refresh();
    return true;
}

bool PianoRoll::wheel(ui::WheelEvent& e) {
    ed_->wheelAt(e, e.pos.x);
    return true;
}

void PianoRoll::resized() { ed_->updateRanges(); }

// =================================================================================================
// VelocityLane
// =================================================================================================

VelocityLane::VelocityLane(MidiEditor* editor) : ed_(editor) {
    setFixedHeight(VELOCITY_H);
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
    chooser = ui::toolButton(this, laneInfo(Lane::Velocity).name, nullptr,
                             "What this lane shows and edits: each note's velocity, chance (how often it plays), "
                             "repeats (a roll of hits), pan or fine tuning");
    ui::Menu* menu = widgets::ownedMenu(chooser);
    for (const LaneInfo& info : lanes()) {
        Lane which = info.lane;
        ui::Action* a = menu->addAction(info.name, [this, which] { ed_->setLane(which); });
        a->checkable = true;
        a->checked = which == Lane::Velocity;
        a->group = 4;
        ed_->laneActions[(int)which] = a;
    }
    chooser->setMenu(menu);
    chooser->setFocusPolicy(ui::FocusPolicy::None);  // (the keys stay the roll's)
    context = widgets::ownedMenu(this);
    resized();
}

void VelocityLane::resized() {
    if (!chooser) return;  // (while it is being made)
    Point hint = chooser->sizeHint();
    chooser->setGeometry(Rect(2, 4, KEYS_W - 4, hint.y));
}

std::string VelocityLane::toolTipAt(Point p) const {
    if (p.x < KEYS_W) return "";
    const LaneInfo& info = laneInfo(ed_->lane);
    return std::string("Each note's ") + (info.lane == Lane::Fine ? "fine tuning" : info.name) +
           ": click or drag across the bars to set it; drag a selected note's bar to change every selected "
           "note by as much. Right-click for more";
}

int VelocityLane::velocityAt(int y) const {
    int h = height();
    return (int)std::min(127.0, std::max(1.0, pyRound((h - 3.0 - y) / (h - 6.0) * 127)));
}

double VelocityLane::valueAt(int y, bool rounded) const {
    const LaneInfo& info = laneInfo(ed_->lane);
    int h = height();
    if (info.lane == Lane::Velocity && rounded) return velocityAt(y);
    double along = (h - 3.0 - y) / (h - 6.0);
    double v = info.lane == Lane::Velocity ? along * 127 : info.low + along * (info.high - info.low);
    if (!rounded) return v;
    Note n;
    setLaneValue(n, info.lane, v);
    return laneValue(n, info.lane);
}

void VelocityLane::paint(Painter& p) {
    MidiEditor& ed = *ed_;
    const LaneInfo& info = laneInfo(ed.lane);
    int w = width(), h = height();
    p.fillRect(rect(), theme::c("meter_bg"));
    p.fillRect(Rect(0, 0, w, 1), theme::c("ruler_bg"));
    // where a value's bar reaches (0…1 of the height); velocity counts from 0, as it always has
    auto along = [&](double v) {
        return info.lane == Lane::Velocity ? v / 127 : (v - info.low) / (info.high - info.low);
    };
    if (info.lane == Lane::Velocity) {
        for (int level : {32, 64, 96, 127}) {
            int y = (int)(h - 3 - level / 127.0 * (h - 6));
            p.fillRect(Rect(KEYS_W, y, w, 1), level != 127 ? theme::c("grid") : theme::c("grid_fine"));
        }
    } else {
        for (double part : {0.25, 0.5, 0.75, 1.0}) {
            int y = (int)(h - 3 - part * (h - 6));
            p.fillRect(Rect(KEYS_W, y, w, 1),
                       part == 1.0 ? theme::c("grid_fine") : info.centred && part == 0.5 ? theme::c("grid_bar")
                                                                                          : theme::c("grid"));
        }
    }
    Color color = ed.color();
    Color light = color.lighter(130), dark = color.darker(150);
    double base = info.centred ? 0.5 : 0.0;  // (pan and fine: from the middle line, up or down)
    for (size_t i = 0; i < ed.notes.size(); ++i) {
        const Note& n = ed.notes[i];
        double x = ed.xOf(n.start);
        if (x < KEYS_W - 4 || x > w || !ed.shows(n)) continue;  // (folded away: not in the roll, nor here)
        double top = along(laneValue(n, info.lane)) * (h - 6), bottom = base * (h - 6);
        double hi = std::max(top, bottom), lo = std::min(top, bottom);
        Color fill = ed.selected.count((int)i) ? theme::c("selected_note") : !ed.off.count((int)i) ? light : dark;
        p.fillRect(aliased(x, h - 3 - hi, 4, hi - lo), fill);
        p.fillRect(aliased(x - 2, h - 5 - top, 8, 3), fill);
    }
    ed.paintCursorLines(p, h);
    p.fillRect(Rect(0, 0, KEYS_W, h), theme::c("ruler_bg"));
    p.drawText(RectF(4, (float)(chooser->y() + chooser->height() + 2), KEYS_W - 6, 16), AlignLeft | AlignTop,
               info.range, theme::uiFont(), theme::c("ruler_text"));
}

std::vector<int> VelocityLane::barsUnder(Point pos) const {
    MidiEditor& ed = *ed_;
    std::vector<std::pair<double, int>> near;
    for (size_t i = 0; i < ed.notes.size(); ++i) {
        if (!ed.shows(ed.notes[i])) continue;
        double d = std::fabs(ed.xOf(ed.notes[i].start) + 2 - pos.x);
        if (d <= 6) near.push_back({d, (int)i});
    }
    std::vector<int> out;
    if (near.empty()) return out;
    double closest = near[0].first;
    for (auto& n : near) closest = std::min(closest, n.first);
    for (auto& n : near)
        if (n.first == closest) out.push_back(n.second);
    return out;
}

bool VelocityLane::setUnder(Point pos) {
    MidiEditor& ed = *ed_;
    std::vector<int> under = barsUnder(pos);
    if (under.empty()) return false;
    double value = valueAt(pos.y);
    for (int i : under) setLaneValue(ed.notes[(size_t)i], ed.lane, value);
    if (ed.lane == Lane::Velocity) ed.lastVelocity = (int)value;
    ed.refresh();
    return true;
}

bool VelocityLane::mousePress(ui::MouseEvent& e) {
    MidiEditor& ed = *ed_;
    if (e.button == plat::RightButton) {  // the lanes, Slide, and the default value back
        context->clear();
        for (const LaneInfo& info : lanes()) {
            Lane which = info.lane;
            ui::Action* a = context->addAction(std::string("Show ") + (which == Lane::Fine ? "fine tuning" : info.name),
                                               [this, which] { ed_->setLane(which); });
            a->checkable = true;
            a->checked = which == ed.lane;
            a->group = 5;
        }
        context->addSeparator();
        bool any = false;
        for (int i : ed.selected) any = any || i < (int)ed.notes.size();
        ui::Action* slide = context->addAction("Slide the selected notes", [this] { ed_->toggleSlide(); }, "G");
        slide->enabled = any;
        ui::Action* reset = context->addAction(
            std::string("Default ") + (ed.lane == Lane::Fine ? "fine tuning" : laneInfo(ed.lane).name) +
                " for the selected notes",
            [this] { ed_->resetLane(); });
        reset->enabled = any;
        context->popup(e.globalPos, this);
        return true;
    }
    if (e.button != plat::LeftButton || e.pos.x < KEYS_W) return true;
    std::vector<int> under = barsUnder(e.pos);
    int chosen = 0;
    for (int i : ed.selected) chosen += i < (int)ed.notes.size();
    bool onSelected = false;
    for (int i : under) onSelected = onSelected || ed.selected.count(i);
    MidiEditor::Drag d;
    if (onSelected && chosen > 1) {  // one of several selected notes: all of them, by as much as the mouse moves
        d.kind = MidiEditor::Drag::LaneShift;
        d.startPos = e.pos;
        for (int i : ed.selected)
            if (i < (int)ed.notes.size()) d.origin[i] = ed.notes[(size_t)i];
    } else {
        d.kind = MidiEditor::Drag::Velocity;
        d.changed = setUnder(e.pos);
    }
    ed.drag = d;
    return true;
}

bool VelocityLane::mouseMove(ui::MouseEvent& e) {
    MidiEditor& ed = *ed_;
    if (ed.drag.kind == MidiEditor::Drag::Velocity) {
        bool changed = setUnder(e.pos);
        ed.drag.changed = ed.drag.changed || changed;
    } else if (ed.drag.kind == MidiEditor::Drag::LaneShift) {
        double by = valueAt(e.pos.y, false) - valueAt(ed.drag.startPos.y, false);
        for (auto& kv : ed.drag.origin) {
            if (kv.first >= (int)ed.notes.size()) continue;
            Note& n = ed.notes[(size_t)kv.first];
            setLaneValue(n, ed.lane, laneValue(kv.second, ed.lane) + by);
            if (laneValue(n, ed.lane) != laneValue(kv.second, ed.lane)) ed.drag.changed = true;
        }
        ed.refresh();
    }
    return true;
}

bool VelocityLane::mouseRelease(ui::MouseEvent& e) {
    MidiEditor::Drag d = ed_->drag;
    ed_->drag = MidiEditor::Drag();
    if ((d.kind == MidiEditor::Drag::Velocity || d.kind == MidiEditor::Drag::LaneShift) && d.changed) ed_->commit();
    return true;
}

bool VelocityLane::wheel(ui::WheelEvent& e) {
    ed_->wheelAt(e, e.pos.x);
    return true;
}

// =================================================================================================
// EditorRuler
// =================================================================================================

EditorRuler::EditorRuler(MidiEditor* editor) : ed_(editor) {
    setFixedHeight(RULER_H);
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
    setCursor(ui::Cursor::Hand);
    setToolTip("Click or drag to move the selection cursor (where Play starts)");
}

void EditorRuler::paint(Painter& p) {
    MidiEditor& ed = *ed_;
    int w = width(), h = height();
    p.fillRect(rect(), theme::c("ruler_bg"));
    double t0 = ed.tAt(KEYS_W), t1 = ed.tAt(w);
    const Timing& timing = ed.project.timing;
    double origin = ed.originS();
    Color text = theme::c("ruler_text");
    Font font = theme::uiFont();
    if (timing.beats) {
        for (const GridLine& line : timing.lines(std::max(0.0, origin + t0 - 1), origin + t1, ed.pps)) {
            int x = (int)ed.xOf(line.seconds - origin);
            if (x < KEYS_W - 40) continue;
            bool label = !line.label.empty();
            p.drawVLine(x, label ? 0 : h - (line.strength == timing::BAR ? 8 : 4), h + 1, text);
            if (label) p.drawText(PointF((float)x + 4, 15), line.label, font, text);
        }
    } else {
        double step = fmt::rulerStep(ed.pps);
        for (int k = (int)(std::max(0.0, origin + t0) / step); k < (int)((origin + t1) / step) + 2; ++k) {
            int x = (int)ed.xOf(k * step - origin);
            p.drawVLine(x, h - 6, h + 1, text);
            p.drawText(PointF((float)x + 4, 15), fmt::time(k * step, fmt::stepDecimals(step)), font, text);
        }
    }
    std::optional<double> c = ed.cursorX();
    if (c && *c >= KEYS_W) paint::cursor(p, (int)*c, theme::c("cursor"), h);
    double ph = ed.playheadX();
    if (ph >= KEYS_W) paint::cursor(p, (int)ph, theme::c("playhead"), h);
    p.fillRect(Rect(0, 0, KEYS_W, h), theme::c("ruler_bg"));  // (labels left of the notes area stay hidden)
}

bool EditorRuler::mousePress(ui::MouseEvent& e) {
    if (e.button == plat::LeftButton && e.pos.x >= KEYS_W) ed_->moveCursor(e.pos.x, e.mods);
    return true;
}

bool EditorRuler::mouseMove(ui::MouseEvent& e) {
    if ((e.buttons & plat::LeftButton) && e.pos.x >= KEYS_W) ed_->moveCursor(e.pos.x, e.mods);
    return true;
}

bool EditorRuler::wheel(ui::WheelEvent& e) {
    ed_->wheelAt(e, e.pos.x);
    return true;
}

// =================================================================================================
// The notes' own properties (the lane, Slide) and the Steps view
// =================================================================================================

ui::Widget* MidiEditor::view() const {
    if (stepsView) return steps;
    return roll;
}

bool MidiEditor::shows(const Note& n) const { return stepsView || yOf(n.pitch).has_value(); }

void MidiEditor::setLane(Lane which) {
    lane = which;
    for (auto& kv : laneActions) kv.second->checked = kv.first == (int)which;
    velocity->chooser->setText(laneInfo(which).name);
    velocity->update();
}

void MidiEditor::syncNoteTools() {
    if (!slideButton) return;
    bool any = false, all = true;
    for (int i : selected)
        if (i < (int)notes.size()) {
            any = true;
            all = all && notes[(size_t)i].slide;
        }
    if (slideButton->isChecked() != (any && all)) slideButton->setCheckedSilently(any && all);
}

void MidiEditor::toggleSlide() {
    std::vector<int> chosen;
    for (int i : selected)
        if (i < (int)notes.size()) chosen.push_back(i);
    if (chosen.empty()) {
        syncNoteTools();  // (a click on Slide with nothing selected lit it: not)
        return;
    }
    bool all = true;
    for (int i : chosen) all = all && notes[(size_t)i].slide;
    for (int i : chosen) notes[(size_t)i].slide = !all;
    commit();
}

void MidiEditor::resetLane() {
    Note plain;  // (the defaults; velocity: a new note's)
    double value = lane == Lane::Velocity ? DEFAULT_VELOCITY : laneValue(plain, lane);
    bool changed = false;
    for (int i : selected) {
        if (i >= (int)notes.size() || laneValue(notes[(size_t)i], lane) == value) continue;
        setLaneValue(notes[(size_t)i], lane, value);
        changed = true;
    }
    if (changed) commit();
}

void MidiEditor::eraseNotes(const std::set<int>& gone) {
    std::vector<Note> kept;
    std::set<int> offs, chosen;
    for (size_t i = 0; i < notes.size(); ++i) {
        if (gone.count((int)i)) continue;
        if (off.count((int)i)) offs.insert((int)kept.size());
        if (selected.count((int)i)) chosen.insert((int)kept.size());
        kept.push_back(notes[i]);
    }
    notes = std::move(kept);
    off = std::move(offs);
    selected = std::move(chosen);
}

void MidiEditor::setStepsView(bool on) {
    if (on != stepsView) {
        drag = Drag();
        release();
        double from = tAt(KEYS_W);
        if (on) {
            rollScroll_ = vbar->value();
            stepsView = true;
            roll->hide();
            steps->show();
            // a step STEP_CELL_PX wide: the fixed grid's, or 1/16 when the grid follows the zoom
            const Timing& t = project.timing;
            double at = std::max(0.0, from);
            if (t.beats) {
                double stepSeconds = (t.grid > 0 ? stepBeats(at) : 0.25) * t.beatSAt(originS() + at);
                if (stepSeconds > 0) pps = std::min(4000.0, std::max(10.0, STEP_CELL_PX / stepSeconds));
            }
        } else {
            stepsView = false;
            steps->hide();
            roll->show();
        }
        hint->setText(on ? STEPS_HINT : ROLL_HINT);
        relayout();  // (the view shown gets its size)
        resized();   // (the hint's height)
        updateRanges();
        hbar->setValue((int)(std::max(0.0, from) * pps));
        vbar->setValue(on ? 0 : rollScroll_);
        view()->setFocus();
    }
    if (stepsButton && stepsButton->isChecked() != on) stepsButton->setCheckedSilently(on);
    refresh();
}

std::vector<int> MidiEditor::stepRows() const {
    std::set<int> pitches(extraStepRows.begin(), extraStepRows.end());
    for (const Note& n : notes) pitches.insert((int)n.pitch);
    if (pitches.empty()) pitches.insert(STEP_FIRST_PITCH);
    return std::vector<int>(pitches.rbegin(), pitches.rend());
}

std::pair<double, double> MidiEditor::stepCell(double t) const {
    const Timing& timing = project.timing;
    double o = originS();
    if (timing.beats) {  // (the song's grid, tempo changes and all, as the piano roll's lines)
        double step = stepBeats(std::max(0.0, t));
        double k = std::floor(timing.beatAt(o + t) / step + 1e-9);
        return {timing.secondsAt(k * step) - o, timing.secondsAt((k + 1) * step) - o};
    }
    double step = stepS();
    double k = std::floor((o + t) / step + 1e-9);
    return {k * step - o, (k + 1) * step - o};
}

std::set<int> MidiEditor::notesInCell(int pitch, double t) const {
    std::pair<double, double> cell = stepCell(t);
    std::set<int> found;
    for (size_t i = 0; i < notes.size(); ++i)
        if ((int)notes[i].pitch == pitch && notes[i].start >= cell.first - 1e-9 && notes[i].start < cell.second - 1e-9)
            found.insert((int)i);
    return found;
}

bool MidiEditor::setCell(int pitch, double t, bool on) {
    std::pair<double, double> cell = stepCell(t);
    if (cell.second <= 1e-9 || pitch < 0 || pitch > 127) return false;  // (wholly before the notes' start)
    std::set<int> here = notesInCell(pitch, t);
    if (on) {
        if (!here.empty()) return false;
        Note n;
        n.start = std::max(0.0, cell.first);
        n.end = cell.second;
        n.pitch = pitch;
        n.velocity = lastVelocity;
        notes.push_back(n);
        return true;
    }
    if (here.empty()) return false;
    eraseNotes(here);
    extraStepRows.insert(pitch);  // (its row stays while the editor is open, though no note uses it now)
    return true;
}

void MidiEditor::addStepRow(int pitch) {
    if (pitch < 0 || pitch > 127) return;
    extraStepRows.insert(pitch);
    updateRanges();
    std::vector<int> rows = stepRows();
    int row = (int)(std::find(rows.begin(), rows.end(), pitch) - rows.begin());
    int y = row * STEP_ROW_H;
    if (y < vbar->value()) vbar->setValue(y);  // (scrolled to it)
    else if (y + STEP_ROW_H > vbar->value() + steps->height()) vbar->setValue(y + STEP_ROW_H - steps->height());
    soundBriefly(pitch, lastVelocity, NOTE_SOUND_S);
    refresh();
}

// --- the Steps view's widget ---

StepGrid::StepGrid(MidiEditor* editor) : ed_(editor) {
    setMouseTracking(true);
    setFocusPolicy(ui::FocusPolicy::Strong);
    setMinimumHeight(200);
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
    rowMenu = widgets::ownedMenu(this);
    rowMenu->aboutToShow = [this](ui::Menu* menu) {  // a pitch for the new row: next to the rows, or any
        menu->clear();
        std::vector<int> rows = ed_->stepRows();
        std::set<int> have(rows.begin(), rows.end());
        auto offer = [&](ui::Menu* m, const std::string& text, int pitch) {
            ui::Action* a = m->addAction(text, [this, pitch] { ed_->addStepRow(pitch); });
            a->enabled = pitch >= 0 && pitch <= 127 && !have.count(pitch);
        };
        offer(menu, "Above: " + noteName(rows.front() + 1), rows.front() + 1);
        offer(menu, "Below: " + noteName(rows.back() - 1), rows.back() - 1);
        menu->addSeparator();
        for (int octave = 9; octave >= -1; --octave) {
            int c = (octave + 1) * 12;
            ui::Menu* sub = menu->addMenu(noteName(c) + " \xe2\x80\x93 " + noteName(std::min(127, c + 11)));
            for (int p = std::min(127, c + 11); p >= c; --p) offer(sub, noteName(p), p);
        }
    };
}

int StepGrid::rowAt(int y) const {
    int r = (int)std::floor((double)(y + ed_->vbar->value()) / STEP_ROW_H);
    if (r < 0 || r > (int)ed_->stepRows().size()) return -1;
    return r;
}

std::optional<int> StepGrid::yOfRow(int row) const {
    int y = row * STEP_ROW_H - ed_->vbar->value();
    if (y + STEP_ROW_H < 0 || y > height()) return std::nullopt;
    return y;
}

void StepGrid::resized() { ed_->updateRanges(); }

bool StepGrid::overridesShortcut(const ui::KeyEvent& e) const { return ed_->roll->overridesShortcut(e); }

std::string StepGrid::toolTipAt(Point p) const {
    int r = rowAt(p.y);
    if (r < 0) return "";
    if (r == (int)ed_->stepRows().size()) return p.x < KEYS_W ? "Add a row for another pitch" : "";
    if (p.x < KEYS_W) return "Press to hear this pitch";
    return "Click to add a note one step long here (or remove the one here); drag along to paint. Ctrl+click: "
           "select its notes; right-click: delete them";
}

void StepGrid::paint(Painter& p) {
    MidiEditor& ed = *ed_;
    int w = width(), h = height();
    std::vector<int> rows = ed.stepRows();
    const Timing& timing = ed.project.timing;
    double origin = ed.originS();
    p.fillRect(rect(), theme::c("ruler_bg"));
    // the steps in view, and whether each is in an even beat (pads shaded by beats, as a drum machine's)
    std::vector<std::pair<double, double>> cells;
    std::vector<bool> even;
    for (double t = std::max(0.0, ed.tAt(KEYS_W)); t < ed.tAt(w) && cells.size() < 4096;) {
        std::pair<double, double> c = ed.stepCell(t);
        if (!(c.second > c.first)) break;
        cells.push_back(c);
        double beat = timing.beats ? std::floor(timing.beatAt(origin + std::max(0.0, c.first)) + 1e-9)
                                   : std::floor((double)cells.size() / 4);
        even.push_back(((int64_t)beat) % 2 == 0);
        t = c.second + 1e-7;
    }
    Color band = theme::c("lane"), pad = theme::c("lane_alt");
    Color padOdd = pad.darker(112);
    int rowsDown = (int)rows.size() * STEP_ROW_H - ed.vbar->value();
    for (size_t r = 0; r < rows.size(); ++r) {
        std::optional<int> y = yOfRow((int)r);
        if (!y) continue;
        p.fillRect(Rect(KEYS_W, *y, w, STEP_ROW_H), band);
        for (size_t k = 0; k < cells.size(); ++k) {
            double x0 = ed.xOf(std::max(0.0, cells[k].first)), x1 = ed.xOf(cells[k].second);
            p.fillRoundedRect(RectF((float)x0 + 1, (float)*y + 2, (float)std::max(1.0, x1 - x0 - 2), STEP_ROW_H - 4), 2,
                              even[k] ? pad : padOdd);
        }
    }
    double lo, hi;  // shade what the clip doesn't play
    ed.windowS(lo, hi);
    int bottom = std::min(h, rowsDown);
    if (bottom > 0) {
        Color shade = theme::c("roll_shade");
        p.fillRect(aliased(KEYS_W, 0, std::max(0.0, ed.xOf(lo) - KEYS_W), bottom), shade);
        p.fillRect(aliased(ed.xOf(hi), 0, w, bottom), shade);
    }
    std::map<int, int> rowOf;
    for (size_t r = 0; r < rows.size(); ++r) rowOf[rows[r]] = (int)r;
    Color color = ed.color();
    for (size_t i = 0; i < ed.notes.size(); ++i) {
        const Note& n = ed.notes[i];
        std::optional<int> y = yOfRow(rowOf[(int)n.pitch]);
        if (!y) continue;
        double x0 = ed.xOf(n.start), x1 = ed.xOf(n.end);
        if (x1 < KEYS_W || x0 > w) continue;
        RectF r((float)x0 + 1, (float)*y + 3, (float)std::max(NOTE_MIN_PX, x1 - x0 - 2), STEP_ROW_H - 6);
        paintNoteBox(p, r, n, color, ed.selected.count((int)i) > 0, ed.off.count((int)i) > 0);
        if (n.slide)  // (a tail into it, as in the piano roll)
            paintSlideTail(p, PointF((float)x0 - 9, (float)*y + STEP_ROW_H - 3), PointF((float)x0 + 2, (float)*y + STEP_ROW_H / 2.0f),
                           color.lighter(170));
    }
    ed.paintCursorLines(p, h);
    // the rows' names (the piano roll's keys), and "+ Row"
    p.fillRect(Rect(0, 0, KEYS_W, std::min(h, rowsDown)), theme::c("keys_white"));
    Font font = theme::uiFont();
    for (size_t r = 0; r <= rows.size(); ++r) {
        std::optional<int> y = yOfRow((int)r);
        if (!y) continue;
        if (r == rows.size()) {
            p.drawText(RectF(6, (float)*y, KEYS_W - 8, STEP_ROW_H), AlignLeft | AlignVCenter, "+ Row", font,
                       theme::c("ruler_text"));
            continue;
        }
        int pitch = rows[r];
        if (ed.held && *ed.held == pitch) p.fillRect(Rect(0, *y, KEYS_W, STEP_ROW_H), theme::c("key_sounding"));
        else if (isBlackKey(pitch)) p.fillRect(Rect(0, *y, 6, STEP_ROW_H), theme::c("keys_black"));
        p.fillRect(Rect(0, *y + STEP_ROW_H - 1, KEYS_W, 1), theme::c("keys_line"));
        p.drawText(RectF(0, (float)*y, KEYS_W - 4, STEP_ROW_H), AlignRight | AlignVCenter, noteName(pitch), font,
                   theme::c("keys_label"));
    }
    p.fillRect(Rect(KEYS_W - 1, 0, 1, h), theme::c("ruler_bg"));
}

bool StepGrid::mousePress(ui::MouseEvent& e) {
    MidiEditor& ed = *ed_;
    std::vector<int> rows = ed.stepRows();
    int r = rowAt(e.pos.y);
    if (r < 0) return true;
    if (r == (int)rows.size()) {  // "+ Row"
        if (e.button == plat::LeftButton && e.pos.x < KEYS_W)
            rowMenu->popup(mapToGlobal(Point(0, *yOfRow(r) + STEP_ROW_H)), this);
        return true;
    }
    int pitch = rows[(size_t)r];
    if (e.pos.x < KEYS_W) {  // a row's name: it sounds while held
        if (e.button == plat::LeftButton) {
            ed.hold(pitch);
            ed.drag = MidiEditor::Drag();
            ed.drag.kind = MidiEditor::Drag::Keys;
            ed.refresh();
        }
        return true;
    }
    double t = ed.tAt(e.pos.x);
    std::set<int> here = ed.notesInCell(pitch, t);
    if (e.button == plat::RightButton) {
        if (!here.empty()) {
            ed.extraStepRows.insert(pitch);  // (the row stays)
            ed.deleteNotes(here);
        }
        return true;
    }
    if (e.button != plat::LeftButton) return true;
    if (e.mods & plat::ModCtrl) {  // its notes in or out of the selection
        bool all = !here.empty();
        for (int i : here) all = all && ed.selected.count(i);
        for (int i : here) {
            if (all) ed.selected.erase(i);
            else ed.selected.insert(i);
        }
        ed.refresh();
        return true;
    }
    MidiEditor::Drag d;
    d.kind = MidiEditor::Drag::Cells;
    d.startPos = e.pos;
    d.lighting = here.empty();
    d.changed = ed.setCell(pitch, t, d.lighting);
    ed.drag = d;
    if (d.lighting) ed.hold(pitch);  // (a new note is heard while the mouse is held)
    ed.refresh();
    return true;
}

bool StepGrid::mouseMove(ui::MouseEvent& e) {
    MidiEditor& ed = *ed_;
    std::vector<int> rows = ed.stepRows();
    int r = rowAt(e.pos.y);
    bool onRow = r >= 0 && r < (int)rows.size();
    if (ed.drag.kind == MidiEditor::Drag::Keys) {
        if (onRow) ed.hold(rows[(size_t)r]);
        ed.refresh();
    } else if (ed.drag.kind == MidiEditor::Drag::Cells) {
        if (onRow && e.pos.x >= KEYS_W && ed.setCell(rows[(size_t)r], ed.tAt(e.pos.x), ed.drag.lighting)) {
            ed.drag.changed = true;
            if (ed.drag.lighting) ed.hold(rows[(size_t)r]);
            ed.refresh();
        }
    } else {
        setCursor(onRow && e.pos.x >= KEYS_W ? ui::Cursor::Hand : ui::Cursor::Arrow);
    }
    return true;
}

bool StepGrid::mouseRelease(ui::MouseEvent& e) {
    MidiEditor& ed = *ed_;
    MidiEditor::Drag d = ed.drag;
    ed.drag = MidiEditor::Drag();
    ed.release();
    if (d.kind == MidiEditor::Drag::Cells && d.changed) ed.commit();
    if (ui::Widget::alive(this)) ed.refresh();
    return true;
}

bool StepGrid::wheel(ui::WheelEvent& e) {
    ed_->wheelAt(e, e.pos.x);
    return true;
}
