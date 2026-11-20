#include "ui/midi_editor.h"
#include "ui/song_scale.h"

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
#include "ui/note_tools_panel.h"
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

namespace {
// the layout's sizes, read again only when the stylesheets change (asked for every note)
struct Sizes {
    int generation = -1;
    int keysW = KEYS_W, velocityH = VELOCITY_H, rulerH = RULER_H, stepRowH = STEP_ROW_H;
};
const Sizes& sizes() {
    static Sizes s;
    int gen = theme::styles().generation();
    if (gen != s.generation) {
        s.keysW = std::max(1, theme::metric("piano-keys-width", KEYS_W));
        s.velocityH = std::max(8, theme::metric("velocity-lane-height", VELOCITY_H));
        s.rulerH = std::max(1, theme::metric("editor-ruler-height", RULER_H));
        s.stepRowH = std::max(4, theme::metric("step-row-height", STEP_ROW_H));
        s.generation = gen;
    }
    return s;
}
}  // namespace

int keysW() { return sizes().keysW; }
int velocityH() { return sizes().velocityH; }
int rulerH() { return sizes().rulerH; }
int stepRowH() { return sizes().stepRowH; }

void NoteLook::read(panel_style::Parts& parts) {
    const css::Style& note = parts.get("note");
    const css::Style& chosen = parts.get("note", "selected");
    selected = panel_style::color(chosen, "border-top-color");
    selectedW = panel_style::lengthF(chosen, "border-top-width", 2);
    edgeW = panel_style::lengthF(note, "border-top-width", 1);
    slideW = panel_style::lengthF(parts.get("slide"), "stroke-width", 2);
    headW = panel_style::lengthF(parts.get("slide-head"), "stroke-width", 1.5);
    const char* strengths[] = {"fine", "beat", "bar"};
    for (int i = 0; i < 3; ++i) grid[i] = panel_style::fg(parts.get("grid", strengths[i]));
    gridW = std::max(1, panel_style::px(parts.get("grid", "fine"), "width", 1));
    const css::Style& c = parts.get("cursor");
    const css::Style& ph = parts.get("playhead");
    cursor = panel_style::fg(c);
    playhead = panel_style::fg(ph);
    cursorW = std::max(1, panel_style::px(c, "width", 1));
    playheadW = std::max(1, panel_style::px(ph, "width", 1));
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

// a grid line's colour in a view's look (timing::BAR… strength)
const Color& grid(const midi_editor::NoteLook& look, int strength) { return look.grid[std::max(0, std::min(2, strength))]; }

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
    "Ctrl+U quantize, G slide, K knife, Ctrl+J join, Shift+click a key: its row. Alt: no snapping (to the grid or the scale).";
const char* const KNIFE_HINT =
    "The knife: click a note, or drag across notes, to cut them at the line (Enter: at the keyboard's cursor). "
    "K or Esc puts it away.";
const char* const STEPS_HINT =
    "Click a pad to add a note one step long, click a lit pad to remove it; drag along to paint (or clear) more. "
    "Ctrl+click: select; right-click: delete; + Row: another pitch; G slide. Ctrl+wheel: zoom (the grid follows).";

// A note's box (the piano roll's and the Steps view's): its velocity in the fill's lightness, its chance in
// the fill's opacity, its repeats as ticks across it; switched off: an outline.
void paintNoteBox(Painter& p, const RectF& r, const Note& n, Color color, bool chosen, bool isOff,
                  const midi_editor::NoteLook& look) {
    Color fill = color.lighter(100 + (int)(n.velocity / 127 * 60));
    if (!isOff) p.fillRect(r, n.chance < 100 ? fill.withAlpha(70 + (int)(std::max(0.0, n.chance) / 100 * 185)) : fill);
    if (n.repeats > 1 && r.w >= 3.0f * (float)n.repeats)
        for (int k = 1; k < n.repeats; ++k)
            p.fillRect(RectF(r.x + r.w * (float)k / (float)n.repeats - 0.5f, r.y + 2, 1, std::max(1.0f, r.h - 4)),
                       fill.darker(170));
    if (chosen) p.strokeRect(r, look.selected, look.selectedW);
    else p.strokeRect(r, isOff ? fill : fill.darker(160), look.edgeW);
}

// a sliding note's tail: a short line from the pitch it glides from (`from`) into its start (`to`), arrowed
void paintSlideTail(Painter& p, PointF from, PointF to, Color c, const midi_editor::NoteLook& look) {
    p.drawLine(from, to, c, look.slideW, LineCap::Round);
    float dx = to.x - from.x, dy = to.y - from.y, len = std::sqrt(dx * dx + dy * dy);
    if (len < 1) return;
    dx /= len, dy /= len;
    for (float side : {-1.0f, 1.0f})
        p.drawLine(to, PointF(to.x - 5 * dx + side * 3 * dy, to.y - 5 * dy - side * 3 * dx), c, look.headW,
                   LineCap::Round);
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
    setStyleType("MidiEditor");
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
    {
        const css::Style& bar = computedStyle("toolbar");
        ui::style::Insets m = ui::style::insets(bar, "padding");
        tools->setContentsMargins(m.left, m.top, m.right, m.bottom);
        tools->spacing = ui::style::px(bar, "column-gap", 4);
    }
    stepsButton = ui::toolButton(this, "Steps", nullptr,
                                 "Show the clip as steps: a row per pitch it uses, a column per grid step. Click a "
                                 "pad to add or remove a note one step long; again for the piano roll");
    stepsButton->setCheckable(true);
    stepsButton->toggled.connect([this](bool on) { setStepsView(on); });
    tools->addWidget(stepsButton);
    tools->addWidget(buildGhostsButton());
    tools->addWidget(buildStampButton());
    tools->addSpacing(ui::style::px(computedStyle("scale"), "margin-left", 8));
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
    tools->addWidget(buildToolsButton());
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
    {
        ui::style::Insets m = ui::style::insets(computedStyle("hint"), "padding");
        hintBox->setContentsMargins(m.left, m.top, m.right, m.bottom);
    }
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
    hrow->setContentsMargins(keysW(), 0, vbarW, 0);
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
    if (toolsPanel) {  // (it never outlives its editor)
        toolsPanel->editor = nullptr;
        toolsPanel->close();
    }
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

double MidiEditor::xOf(double t) const { return keysW() + (t * pps - hbar->value()); }
double MidiEditor::tAt(double x) const { return (x - keysW() + hbar->value()) / pps; }

std::optional<Steps> MidiEditor::scale() const {
    std::string name = song_scale::scaleOf(project, *clip);  // (the clip's own, or the song's)
    for (auto& s : scales())
        if (s.first == name) return scaleSteps(song_scale::keyOf(project, *clip), name);
    return std::nullopt;
}

int MidiEditor::onScale(int pitch, int mods, int dir) const {
    if (!snapScaleButton->isChecked() || (mods & plat::ModAlt)) return pitch;
    std::optional<Steps> steps = scale();
    if (!steps || steps->empty() || steps->count(((pitch % 12) + 12) % 12)) return pitch;
    for (int d = 1; d < 12; d++)  // (the nearer side first; a tie: towards dir, else down)
        for (int sign : {dir > 0 ? 1 : -1, dir > 0 ? -1 : 1}) {
            int p = pitch + sign * d;
            if (p >= 0 && p <= 127 && steps->count(p % 12)) return p;
        }
    return pitch;
}

const std::vector<int>& MidiEditor::rows() const {
    std::vector<int64_t> key = {fold == "scale" ? 1 : fold == "used" ? 2 : 0, song_scale::keyOf(project, *clip),
                                (int64_t)std::hash<std::string>()(song_scale::scaleOf(project, *clip))};
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
    double width = std::max(200, view()->width() - keysW() - 20);
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
    int viewW = view()->width() - keysW();
    hbar->setRange(0, std::max(0, (int)(lengthS() * pps) - viewW));
    hbar->setPageStep(std::max(1, viewW));
    if (stepsView) {  // (its rows, and "+ Row" under them)
        vbar->setRange(0, std::max(0, ((int)stepRows().size() + 1) * stepRowH() - steps->height()));
        vbar->setPageStep(std::max(1, steps->height()));
        vbar->setSingleStep(stepRowH());
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
    hbar->setValue((int)(t * pps - (x - keysW())));
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
    int sides = ui::style::insets(computedStyle("hint"), "padding").w();  // (the hint's row's margins)
    int h = wrappedHeight(hint->text(), hint->font(), (float)std::max(40, width() - sides - 2 * hint->margin));
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
    cancelPreview();
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
    std::pair<int, std::string> sc(song_scale::keyOf(project, *clip), song_scale::scaleOf(project, *clip));
    std::vector<int64_t> clips = {  // (and the grid: Ctrl+1/2/4/5, the tempo, from the transport)
        (int64_t)std::llround(t.grid * 1e6), t.snapOn, t.beats, t.thirds, (int64_t)std::llround(t.bpm * 1e6),
        t.beatsPerBar, (int64_t)t.changes.size(), (int64_t)(intptr_t)t.map().get()};  // (the map: ramps too)
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

void MidiEditor::paintCursorLines(Painter& p, int height, const NoteLook& look) {
    std::optional<double> c = cursorX();
    if (c && *c >= keysW()) p.fillRect(Rect((int)*c, 0, look.cursorW, height + 1), look.cursor);
    double ph = playheadX();
    if (ph >= keysW()) p.fillRect(Rect((int)ph, 0, look.playheadW, height + 1), look.playhead);
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
    if (toolsPanel) toolsPanel->updatePreview();  // (the open tool on the notes as they now are)
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

void MidiEditor::reload() {
    drag = Drag();
    release();
    notes = clip->notes ? *clip->notes : std::vector<Note>();
    selected.clear();
    takeMuted();
    takeUsed();
    syncScaleTools();  // (the scale may have been undone too)
    updateRanges();
    if (toolsPanel) toolsPanel->updatePreview();
    refresh();
}

double MidiEditor::wholeBars(double t) const {
    const Timing& timing = project.timing;
    double o = originS(), at = o;
    while (at < o + t - 1e-6) at = timing.secondsAt(timing.beatAt(at) + timing.beatsPerBarAt(at));  // (a bar of beats)
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
        int dir = key == plat::Key_Up ? 1 : -1;
        for (int i : chosen) {  // (snapping to the scale: on to its next note that way)
            int p = (int)pitches[i];
            if (pitches[i] == before[i].pitch) continue;  // (at the edge: stays)
            for (int k = 0; k < 12 && p >= 0 && p <= 127 && onScale(p, mods, dir) != p; k++) p += dir;
            notes[i].pitch = p >= 0 && p <= 127 ? p : before[i].pitch;
        }
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

std::string MidiEditor::keyLabel(int pitch) const {
    TrackPtr t = std::atomic_load(&clip->track);
    return t ? t->keyLabel(pitch) : std::string();
}

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

void MidiEditor::startKeyCursor() {
    if (keyT >= 0) return;
    double t = timeline.cursor ? (double)timeline.cursor() / sr() - originS() : 0;
    double from = 0, to = 0;
    windowS(from, to);
    keyT = snap(std::clamp(t, from, std::max(from, to - stepS(from))), 0, true);
    keyPitch = notes.empty() ? 60 : (int)notes.front().pitch;
}

void MidiEditor::moveKeyCursor(int key) {
    if (key == plat::Key_Left || key == plat::Key_Right) {
        double d = key == plat::Key_Left ? -stepBeats(keyT) : stepBeats(keyT);
        keyT = std::max(0.0, snap(shifted(keyT, d), 0));
    } else {
        std::vector<int> stepped = stepsView ? stepRows() : std::vector<int>();
        const std::vector<int>& rs = stepsView ? stepped : rows();  // (top to bottom; the Steps view: its rows)
        if (rs.empty()) return;
        auto at = std::find(rs.begin(), rs.end(), keyPitch);
        int i = at == rs.end() ? nearestRow(keyPitch) : (int)(at - rs.begin());
        i = std::clamp(i + (key == plat::Key_Up ? -1 : 1), 0, (int)rs.size() - 1);
        keyPitch = rs[(size_t)i];
        hold(keyPitch);  // (heard a moment)
        ui::App::get().singleShot(150, [this] {
            if (ui::Widget::alive(this)) release();
        });
    }
    refresh();
}

void MidiEditor::keyNote() {
    startKeyCursor();
    if (stepsView) {  // the Steps view: the pad at the cursor lit, or cleared (as a click)
        std::vector<int> rs = stepRows();
        if (rs.empty()) return;
        if (std::find(rs.begin(), rs.end(), keyPitch) == rs.end()) keyPitch = rs.front();
        bool light = notesInCell(keyPitch, keyT).empty();
        if (setCell(keyPitch, keyT, light)) {
            if (light) {
                hold(keyPitch);
                ui::App::get().singleShot(150, [this] {
                    if (ui::Widget::alive(this)) release();
                });
            }
            commit();
        }
        return;
    }
    for (int i = 0; i < (int)notes.size(); i++)
        if ((int)notes[i].pitch == keyPitch && notes[i].start <= keyT + 1e-9 && keyT < notes[i].end - 1e-9) {
            selected = {i};
            lastVelocity = (int)notes[i].velocity;
            refresh();
            return;
        }
    keyPitch = onScale(keyPitch, 0);  // (snapping to the scale: its nearest note)
    Note n;
    n.start = keyT;
    n.end = keyT + stepS(keyT);
    n.pitch = keyPitch;
    n.velocity = lastVelocity;
    notes.push_back(n);
    selected = {(int)notes.size() - 1};
    hold(keyPitch);
    ui::App::get().singleShot(150, [this] {
        if (ui::Widget::alive(this)) release();
    });
    commit();
}

void MidiEditor::velocityBy(int amount) {
    if (selected.empty()) {
        lastVelocity = std::clamp(lastVelocity + amount, 1, 127);
        updateTitle();
        return;
    }
    for (int i : selected)
        if (i < (int)notes.size()) notes[i].velocity = std::clamp(notes[i].velocity + amount, 1.0, 127.0);
    lastVelocity = (int)notes[*selected.begin()].velocity;
    commit();
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
        startKeyCursor();
        if (selected.empty()) {
            moveKeyCursor(e.key);
        } else {
            nudge(e.key, e.mods & plat::ModShift, e.mods);
            int first = *selected.begin();  // (the cursor goes with the notes)
            if (first < (int)notes.size()) keyT = notes[first].start, keyPitch = (int)notes[first].pitch;
        }
    } else if ((e.key == plat::Key_Return || e.key == plat::Key_Enter) && !ctrl) {
        if (knife) splitAtKeyCursor();
        else keyNote();
    } else if (e.key == 'K' && !(e.mods & (plat::ModCtrl | plat::ModAlt | plat::ModShift | plat::ModMeta))) {
        setKnife(!knife);
    } else if (e.key == 'J' && ctrl) {
        joinNotes();
    } else if (e.key == 'P' && !(e.mods & (plat::ModCtrl | plat::ModAlt | plat::ModShift | plat::ModMeta))) {
        startKeyCursor();
        int first = selected.empty() ? -1 : *selected.begin();
        selectRow(first >= 0 && first < (int)notes.size() ? (int)notes[first].pitch : keyPitch, false);
    } else if (e.key == plat::Key_F2 && !e.mods && stepsView && steps) {  // the row at the cursor: its swing
        std::vector<int> rs = stepRows();
        if (!rs.empty()) {
            int pitch = std::find(rs.begin(), rs.end(), keyPitch) != rs.end() ? keyPitch : rs.front();
            steps->rowSwingMenu(pitch, steps->mapToGlobal(Point(0, 0)));
        }
    } else if (e.key == plat::Key_F2 && !e.mods) {  // the tools' menu (tinydaw --tui's way to them)
        ui::Menu menu;
        toolsMenu(menu);
        ui::Widget* v = view();
        menu.exec(v->mapToGlobal(Point(v->width() / 2, v->height() / 3)), v);
    } else if (e.key == 'R' && !e.mods && stepsView && steps && steps->rowMenu) {  // "+ Row"
        steps->rowMenu->exec(steps->mapToGlobal(Point(0, 0)), steps);
    } else if ((e.key == ',' || e.key == '.') && !ctrl && !selected.empty()) {  // (Shift+Left / Right, which consoles can't send)
        nudge(e.key == ',' ? plat::Key_Left : plat::Key_Right, true, 0);
    } else if ((e.key == '+' || e.key == '=' || e.key == '-') && !ctrl) {
        velocityBy(e.key == '-' ? -8 : 8);
    } else if (e.key == plat::Key_Escape) {
        if (knife) setKnife(false);
        else selected.clear();
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
    ghostsButton = ui::toolButton(this, "Ghosts", nullptr,  // (its menu gives it the ▾)
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
    cancelPreview();  // (a tool's preview never follows to another clip)
    if (timeline.editNotes) timeline.editNotes(other);  // (the main window opens, or raises, its editor)
}

// --- the chord stamp ---

ui::Button* MidiEditor::buildStampButton() {
    stampButton = ui::toolButton(this, "Stamp", nullptr,  // (its menu gives it the ▾)
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
    stampButton->setText(stamp == "Off" ? "Stamp" : shown);
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
    foldButton = ui::toolButton(this, "Fold", nullptr,  // (its menu gives it the ▾)
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
    static bool snapScale = false;  // (remembered for the next editor)
    snapScaleButton = ui::toolButton(this, "Snap to scale", nullptr,
                                     "Notes added or moved land on the scale's notes (hold Alt to place one outside "
                                     "it); notes not moved stay as they are");
    snapScaleButton->setCheckable(true);
    snapScaleButton->setCheckedSilently(snapScale);
    snapScaleButton->toggled.connect([](bool on) {
        snapScale = on;
        util::debugf("midi editor: snap to scale %s\n", on ? "on" : "off");
    });
    tools->addWidget(snapScaleButton);
}

void MidiEditor::syncScaleTools() {
    keyBox->setCurrentIndexSilently(std::min(11, std::max(0, song_scale::keyOf(project, *clip))));
    scaleBox->setCurrentIndexSilently(std::max(0, scaleBox->findData(song_scale::scaleOf(project, *clip))));
    bool own = clip->key >= 0;
    std::string tip = own ? "This clip's own key and scale (the clip menu's Scale \xe2\x96\xb8 gives it the song's): "
                            "notes outside it are shaded"
                          : "The song's key and scale (saved with the project): notes outside it are shaded";
    keyBox->setToolTip(tip);
    scaleBox->setToolTip(tip);
    bool on = scale().has_value();
    if (!stampActions.empty()) syncStamp();
    keyBox->setEnabled(on);
    fitButton->setEnabled(on);
    foldActions["scale"]->enabled = on;
    if (fold == "scale" && !on) fold = "off";  // (no scale any more: nothing to fold to)
    checkOnly(foldActions, fold);
    foldButton->setText(fold == "off" ? "Fold" : fold == "scale" ? "Folded: scale" : "Folded: used");
    foldButton->updateGeometry();
}

void MidiEditor::scaleChosen() {
    int key = keyBox->currentIndex();
    std::string sc = scaleBox->currentData();
    refold([this, key, sc] {
        if (clip->key >= 0) {  // (the clip's own)
            clip->key = key;
            clip->scale = sc;
        } else {
            project.timing.key = key;
            project.timing.scale = sc;
        }
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
    setStyleType("PianoRoll");
    setMouseTracking(true);
    setFocusPolicy(ui::FocusPolicy::Strong);
    setMinimumHeight(theme::metric("piano-roll-min-height", 200));
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
}

bool PianoRoll::textCells(int cols, int rows, std::vector<ui::TextCell>& cells) const {
    const MidiEditor& ed = *ed_;
    cells.assign((size_t)cols * rows, ui::TextCell());
    const int KEYS = 5;  // the key names' column
    if (cols <= KEYS + 2 || rows < 2) return true;
    auto at = [&](int c, int r) -> ui::TextCell& { return cells[(size_t)(r * cols + c)]; };
    auto put = [&](int c, int r, const std::string& text, uint8_t fg, uint8_t attr) {
        for (size_t i = 0; i < text.size() && c + (int)i < cols; i++) at(c + (int)i, r) = ui::TextCell{(char32_t)text[i], fg, 9, attr};
    };
    // time across: the roll's pixels (its scroll and zoom), a share of them per column
    int noteCols = cols - KEYS;
    double x0 = midi_editor::keysW(), x1 = std::max(x0 + 1, (double)width());
    auto tOf = [&](int c) { return ed.tAt(x0 + (x1 - x0) * c / noteCols); };
    double t0 = tOf(0), dt = tOf(1) - t0;
    // (the keyboard's cursor kept in view: the roll scrolls to it)
    double keyT = ed.keyT >= 0 ? ed.keyT : 0;
    const Timing& tm = ed.project.timing;
    double origin = ed.originS();
    // row 0: the bars
    for (int c = 0; c < noteCols; c++) {
        double a = origin + t0 + c * dt;
        if (c == 0 || tm.barAt(a + dt - 1e-9) != tm.barAt(a - 1e-9)) put(KEYS + c, 0, std::to_string(tm.barAt(a + dt - 1e-9) + 1), 9, 1);
    }
    // the pitches around the cursor's
    const std::vector<int>& rs = ed.rows();
    if (rs.empty()) return true;
    auto found = std::find(rs.begin(), rs.end(), ed.keyPitch);
    int centre = found == rs.end() ? ed.nearestRow(ed.keyPitch) : (int)(found - rs.begin());
    int shown = rows - 1;
    int first = std::clamp(centre - shown / 2, 0, std::max(0, (int)rs.size() - shown));
    double from = 0, to = 0;
    ed.windowS(from, to);
    for (int r = 0; r < shown && first + r < (int)rs.size(); r++) {
        int pitch = rs[(size_t)(first + r)];
        bool black = pitch % 12 == 1 || pitch % 12 == 3 || pitch % 12 == 6 || pitch % 12 == 8 || pitch % 12 == 10;
        std::string name = ed.keyLabel(pitch);
        if (name.empty()) name = midi_editor::noteName(pitch);
        put(0, r + 1, name.substr(0, KEYS - 1), black ? 4 : 9, pitch == ed.keyPitch ? 2 : 0);
        for (int c = 0; c < noteCols; c++) {
            double a = t0 + c * dt;
            bool beat = std::floor(tm.beatAt(origin + a + dt - 1e-9)) != std::floor(tm.beatAt(origin + a - 1e-9));
            bool outside = a + dt <= from || a >= to;
            at(KEYS + c, r + 1) = ui::TextCell{beat ? U'·' : U' ', (uint8_t)(outside ? 0 : 4), 9, 0};
        }
        for (int i = 0; i < (int)ed.notes.size(); i++) {
            const Note& n = ed.notes[(size_t)i];
            if ((int)std::lround(n.pitch) != pitch) continue;
            int a = (int)std::floor((n.start - t0) / dt + 1e-9), b = (int)std::ceil((n.end - t0) / dt - 1e-9);
            b = std::max(b, a + 1);
            bool sel = ed.selected.count(i) > 0, off = ed.off.count(i) > 0;
            for (int c = std::max(0, a); c < std::min(noteCols, b); c++)
                at(KEYS + c, r + 1) = ui::TextCell{off ? U'░' : c == a ? U'▐' : U'█', 2, 9, (uint8_t)(sel ? 1 | 2 : 0)};
        }
        for (const Note& n : ed.preview) {  // the open tool's result: ▒ where no note is, ▌ where each starts
            if ((int)std::lround(n.pitch) != pitch) continue;
            int a = (int)std::floor((n.start - t0) / dt + 1e-9), b = std::max(a + 1, (int)std::ceil((n.end - t0) / dt - 1e-9));
            for (int c = std::max(0, a); c < std::min(noteCols, b); c++) {
                ui::TextCell& cell = at(KEYS + c, r + 1);
                if (c == a) cell = ui::TextCell{U'▌', 6, 9, 1};
                else if (cell.ch == U' ' || cell.ch == U'·') cell = ui::TextCell{U'▒', 6, 9, 0};
            }
        }
    }
    // the keyboard's cursor
    int kr = -1;
    for (int r = 0; r < shown && first + r < (int)rs.size(); r++)
        if (rs[(size_t)(first + r)] == ed.keyPitch) kr = r + 1;
    int kc = (int)std::floor((keyT - t0) / dt + 1e-9);
    if (kr > 0 && kc >= 0 && kc < noteCols) {
        ui::TextCell& k = at(KEYS + kc, kr);
        if (k.ch == U' ' || k.ch == U'·') k.ch = U'│';
        k.attr |= 2 | 1;
        k.fg = 3;
    }
    return true;
}

namespace {
void stepColumns(const MidiEditor& ed, int noteCols, double& t0, double& sb);
int stepIndex(const MidiEditor& ed, double t0, double sb, double t);
}  // namespace

bool VelocityLane::textCells(int cols, int rows, std::vector<ui::TextCell>& cells) const {
    const MidiEditor& ed = *ed_;
    cells.assign((size_t)cols * rows, ui::TextCell());
    const int KEYS = 5;
    if (cols <= KEYS + 2 || rows < 1) return true;
    const char* label = "vel.";
    for (int i = 0; label[i] && i < KEYS; i++) cells[(size_t)((rows - 1) * cols + i)] = ui::TextCell{(char32_t)label[i], 9, 9, 0};
    int noteCols = cols - KEYS;
    double x0 = midi_editor::keysW(), x1 = std::max(x0 + 1, (double)width());
    double t0 = ed.tAt(x0), dt = (ed.tAt(x1) - t0) / noteCols, sb = 1;
    if (ed.stepsView) stepColumns(ed, noteCols, t0, sb);  // (under the Steps view's columns)
    static const char32_t BLOCKS[] = {U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█'};
    for (int i = 0; i < (int)ed.notes.size(); i++) {
        const Note& n = ed.notes[(size_t)i];
        int c = ed.stepsView ? stepIndex(ed, t0, sb, n.start) : (int)std::floor((n.start - t0) / dt + 1e-9);
        if (c < 0 || c >= noteCols) continue;
        // (the loudest note starting in the column; several rows: a taller bar)
        double v = std::clamp(n.velocity / 127.0, 0.0, 1.0) * rows;
        bool sel = ed.selected.count(i) > 0;
        for (int r = 0; r < rows; r++) {
            double fill = std::clamp(v - (rows - 1 - r), 0.0, 1.0);
            if (fill <= 0) continue;
            ui::TextCell& cell = cells[(size_t)(r * cols + KEYS + c)];
            char32_t ch = BLOCKS[std::clamp((int)std::lround(fill * 8) - 1, 0, 7)];
            if (cell.ch == U' ' || ch > cell.ch) cell = ui::TextCell{ch, 2, 9, (uint8_t)(sel ? 1 : 0)};
        }
    }
    return true;
}

namespace {
// (as text) the Steps view's columns: the first one's note time and the step (beats), the keyboard's cursor in view
void stepColumns(const MidiEditor& ed, int noteCols, double& t0, double& sb) {
    t0 = ed.snap(std::max(0.0, ed.tAt(midi_editor::keysW())), 0, true);
    sb = ed.stepBeats(t0);
    const Timing& tm = ed.project.timing;
    double origin = ed.originS();
    double kt = ed.keyT >= 0 ? ed.keyT : t0;
    if (kt < t0) t0 = kt;
    else if ((tm.beatAt(origin + kt) - tm.beatAt(origin + t0)) / sb >= noteCols - 1e-6) t0 = ed.shifted(kt, -(noteCols - 1) * sb);
}
int stepIndex(const MidiEditor& ed, double t0, double sb, double t) {
    const Timing& tm = ed.project.timing;
    double origin = ed.originS();
    return (int)std::floor((tm.beatAt(origin + t) - tm.beatAt(origin + t0)) / sb + 1e-6);
}
}  // namespace

bool StepGrid::textCells(int cols, int rows, std::vector<ui::TextCell>& cells) const {
    const MidiEditor& ed = *ed_;
    cells.assign((size_t)cols * rows, ui::TextCell());
    const int KEYS = 5;
    if (cols <= KEYS + 2 || rows < 2) return true;
    auto at = [&](int c, int r) -> ui::TextCell& { return cells[(size_t)(r * cols + c)]; };
    auto put = [&](int c, int r, const std::string& text, uint8_t fg, uint8_t attr) {
        for (size_t i = 0; i < text.size() && c + (int)i < cols; i++) at(c + (int)i, r) = ui::TextCell{(char32_t)text[i], fg, 9, attr};
    };
    int noteCols = cols - KEYS;
    // a column per step, from the first in view; (the keyboard's cursor kept in view)
    double t0 = 0, sb = 1;
    stepColumns(ed, noteCols, t0, sb);
    const Timing& tm = ed.project.timing;
    double origin = ed.originS();
    auto stepsFrom = [&](double a, double b) { return stepIndex(ed, a, sb, b); };
    std::vector<int> rs = ed.stepRows();
    int keyRow = -1;
    for (int i = 0; i < (int)rs.size(); i++)
        if (rs[(size_t)i] == ed.keyPitch) keyRow = i;
    int shown = rows - 1;  // (the last: "+ Row")
    int first = std::clamp(std::max(0, keyRow) - shown / 2, 0, std::max(0, (int)rs.size() - shown));
    for (int r = 0; r < shown && first + r < (int)rs.size(); r++) {
        int pitch = rs[(size_t)(first + r)];
        std::string name = ed.keyLabel(pitch);
        if (name.empty()) name = midi_editor::noteName(pitch);
        put(0, r, name.substr(0, KEYS - 1), 9, first + r == keyRow ? 2 : 0);
        for (int c = 0; c < noteCols; c++) {
            double t = ed.shifted(t0, c * sb);
            bool lit = !ed.notesInCell(pitch, t).empty();
            bool beat = std::fabs(tm.beatAt(origin + t) - std::round(tm.beatAt(origin + t))) < 1e-6;
            at(KEYS + c, r) = ui::TextCell{lit ? U'■' : beat ? U'▫' : U'·', (uint8_t)(lit ? 2 : 4), 9, (uint8_t)(lit ? 1 : 0)};
            if (first + r == keyRow && ed.keyT >= 0 && stepsFrom(t, ed.keyT) == 0 && ed.keyT >= t - 1e-9) {
                at(KEYS + c, r).attr |= 2 | 1;
                at(KEYS + c, r).fg = 3;
            }
        }
    }
    if (rs.empty()) put(0, 0, "no rows yet: R adds one", 9, 0);
    put(0, rows - 1, "+ Row (R)", 6, 0);
    return true;
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
    case plat::Key_Return: case plat::Key_Enter: case ',': case '.': case '+': case '=': case '-':  // (the keyboard's cursor)
        return !ctrl;
    case 'A': case 'C': case 'D': case 'U': case 'V': case 'X': case 'J':
        return ctrl && !(e.mods & plat::ModShift);
    case 'K': case 'P': case plat::Key_F2:  // (the note tools: the knife, a row, their menu)
        return !ctrl && !(e.mods & plat::ModShift);
    default:
        return false;
    }
}

const PianoRoll::Look& PianoRoll::look() {
    if (!parts_.refresh()) return look_;
    Look l;
    l.notes.read(parts_);
    l.rowWhite = &parts_.get("row", "white");
    l.rowBlack = &parts_.get("row", "black");
    l.outside = &parts_.get("row", "outside");
    l.tonic = &parts_.get("row", "tonic");
    const css::Style& octave = parts_.get("octave");
    l.octave = panel_style::bg(octave);
    l.octaveH = std::max(0, panel_style::px(octave, "height", 1));
    l.below = &parts_.get("below");
    l.shade = &parts_.get("shade");
    const css::Style& loopEnd = parts_.get("loop-end");
    l.loopEnd = panel_style::fg(loopEnd);
    l.loopEndW = std::max(1, panel_style::px(loopEnd, "width", 1));
    l.selectionBox = &parts_.get("selection-box");
    l.boxEdge = panel_style::color(*l.selectionBox, "border-top-color");
    l.keys = &parts_.get("keys");
    const css::Style& line = parts_.get("key-line");
    l.keyLine = panel_style::bg(line);
    l.keyLineH = std::max(0, panel_style::px(line, "height", 1));
    const css::Style& label = parts_.get("key-label");
    l.label = panel_style::fg(label);
    l.labelPad = panel_style::px(label, "padding-right", 4);
    l.font = ui::style::font(label, theme::uiFont());
    double keys = keysW();
    auto share = [&](const char* states, double def) {
        return keys > 0 ? parts_.get("key", states).length("width", def * keys, keys) / keys : def;
    };
    l.keyBlack = &parts_.get("key", "black");
    l.blackShare = share("black", 0.62);
    l.foldedShare = share("black folded", 0.4);
    l.namedShare = share("black named", 0.18);
    l.sounding = &parts_.get("key", "sounding");
    const css::Style& edge = parts_.get("keys-edge");
    l.keysEdge = panel_style::bg(edge);
    l.keysEdgeW = std::max(0, panel_style::px(edge, "width", 1));
    look_ = l;
    return look_;
}

void PianoRoll::paint(Painter& p) {
    MidiEditor& ed = *ed_;
    const Look& look = this->look();
    const int KEYS_W = keysW();
    int w = width(), h = height();
    const std::vector<int>& rows = ed.rows();
    std::optional<Steps> steps = ed.scale();
    int root = ed.project.timing.key;
    int first, last;
    ed.visibleRows(h, first, last);
    for (int i = first; i < last; ++i) {  // rows: black keys darker, a line between octaves; outside the scale shaded
        int pitch = rows[i], y = i * ed.rowH - ed.vbar->value();
        Rect row(KEYS_W, y, w, ed.rowH);
        ui::style::box(p, row, isBlackKey(pitch) ? *look.rowBlack : *look.rowWhite);
        if (steps) {
            if (!steps->count(pitch % 12)) ui::style::box(p, row, *look.outside);
            else if (pitch % 12 == root) ui::style::box(p, row, *look.tonic);
        }
        if (i > 0 && rows[i - 1] / 12 != pitch / 12) p.fillRect(Rect(KEYS_W, y, w, look.octaveH), look.octave);
    }
    int below = (int)rows.size() * ed.rowH - ed.vbar->value();  // (folded: no rows under the last one)
    if (below < h) ui::style::box(p, Rect(KEYS_W, below, w, h - below), *look.below);
    paintGrid(p, std::min(h, below));
    double lo, hi;  // shade what the clip doesn't play
    ed.windowS(lo, hi);
    ui::style::box(p, aliased(KEYS_W, 0, std::max(0.0, ed.xOf(lo) - KEYS_W), h), *look.shade);
    ui::style::box(p, aliased(ed.xOf(hi), 0, w, h), *look.shade);
    paintGhosts(p, w);
    paintRepeats(p, w, h);
    Color color = ed.color();
    for (size_t i = 0; i < ed.notes.size(); ++i) {
        const Note& n = ed.notes[i];
        std::optional<double> y = ed.yOf(n.pitch);
        if (!y) continue;  // (folded away)
        RectF r((float)ed.xOf(n.start), (float)(*y + 1), (float)std::max(NOTE_MIN_PX, (n.end - n.start) * ed.pps),
                (float)(ed.rowH - 2));
        if (r.right() < KEYS_W || r.x > w || r.bottom() < 0 || r.y > h) continue;
        paintNoteBox(p, r, n, color, ed.selected.count((int)i) > 0, ed.off.count((int)i) > 0, look.notes);
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
        paintSlideTail(p, PointF(x - 14, from), PointF(x + 1, mid), color.lighter(170), look.notes);
    }
    if (!ed.preview.empty()) {  // the open tool's result (note_tools_panel.h), ghosted over the notes
        Color fill = color.lighter(150).withAlpha(GHOST_ALPHA), edge = look.notes.selected;
        for (const Note& n : ed.preview) {
            std::optional<double> y = ed.yOf(n.pitch);
            if (!y) continue;
            RectF r((float)ed.xOf(n.start), (float)(*y + 1), (float)std::max(NOTE_MIN_PX, (n.end - n.start) * ed.pps),
                    (float)(ed.rowH - 2));
            if (r.right() < KEYS_W || r.x > w || r.bottom() < 0 || r.y > h) continue;
            p.fillRect(r, fill);
            dashedBox(p, r, edge);
        }
    }
    ed.paintCursorLines(p, h, look.notes);
    if (ed.drag.kind == MidiEditor::Drag::Cut) {  // the knife's line, across the rows dragged over
        int x = (int)ed.xOf(ed.snap(ed.tAt(ed.drag.startPos.x), 0));
        int y0 = std::min(ed.drag.startPos.y, ed.drag.pos.y), y1 = std::max(ed.drag.startPos.y, ed.drag.pos.y);
        y0 = ed.rowAt(y0) * ed.rowH - ed.vbar->value();
        y1 = (ed.rowAt(y1) + 1) * ed.rowH - ed.vbar->value();
        p.fillRect(Rect(x, y0, std::max(1, look.notes.cursorW), y1 - y0), look.notes.cursor);
    }
    if (ed.drag.kind == MidiEditor::Drag::Box) {  // (Shift+drag: the selection box)
        RectF box = normalized(ed.drag.startPos, ed.drag.pos);
        // (whole pixels: the mouse's positions) its fill as a box, then its dashed edge in border-color
        ui::style::box(p, Rect((int)box.x, (int)box.y, (int)box.w, (int)box.h), *look.selectionBox);
        dashedBox(p, box, look.boxEdge);
    }
    paintKeys(p, h);
}

void PianoRoll::paintRepeats(Painter& p, int w, int h) {
    MidiEditor& ed = *ed_;
    const int KEYS_W = keysW();
    const Clip& clip = *ed.clip;
    if (!clip.loop || clip.lengthS <= 0) return;
    double lo, hi;
    ed.windowS(lo, hi);
    Color line = look_.loopEnd;
    float lineW = (float)look_.loopEndW;
    for (int k = 1; k * clip.lengthS < hi && k < 4096; ++k) {  // where each pass ends
        int x = (int)ed.xOf(k * clip.lengthS);
        if (KEYS_W <= x && x <= w)
            p.drawDashedLine(PointF(x + lineW / 2, 0), PointF(x + lineW / 2, (float)h), line, lineW, 4, 2);
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
    double t0 = ed.tAt(keysW()), t1 = ed.tAt(w);
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
    const NoteLook& look = look_.notes;
    double t0 = ed.tAt(keysW()), t1 = ed.tAt(width());
    const Timing& timing = ed.project.timing;
    if (h <= 0) return;
    if (timing.beats) {
        double origin = ed.originS();
        for (const GridLine& line : timing.lines(std::max(0.0, origin + t0), origin + t1, ed.pps)) {
            int x = (int)ed.xOf(line.seconds - origin);
            p.fillRect(Rect(x, 0, look.gridW, h + 1), grid(look, line.strength));
        }
    } else {  // the ruler's steps from 0 s (as the ruler counts them), and fainter quarters of them (where
              // Quantize and new notes go: stepS)
        double step = fmt::rulerStep(ed.pps) / 4, origin = ed.originS();
        for (int k = (int)(std::max(0.0, origin + t0) / step); k < (int)((origin + t1) / step) + 2; ++k) {
            int x = (int)ed.xOf(k * step - origin);
            p.fillRect(Rect(x, 0, look.gridW, h + 1), k % 4 == 0 ? look.grid[1] : grid(look, 0));
        }
    }
}

void PianoRoll::paintKeys(Painter& p, int h) {
    MidiEditor& ed = *ed_;
    const Look& look = look_;
    const int KEYS_W = keysW();
    const std::vector<int>& rows = ed.rows();
    std::optional<Steps> steps = ed.scale();
    int root = ed.project.timing.key;
    bool folded = rows.size() < 128;
    int below = (int)rows.size() * ed.rowH - ed.vbar->value();
    ui::style::box(p, Rect(0, 0, KEYS_W, std::min(h, below)), *look.keys);  // (the white keys)
    if (below < h) ui::style::box(p, Rect(0, below, KEYS_W, h - below), *look.below);  // (folded: no keys there)
    const Font& font = look.font;
    int first, last;
    ed.visibleRows(h, first, last);
    for (int i = first; i < last; ++i) {
        int pitch = rows[i], y = i * ed.rowH - ed.vbar->value();
        std::string label = ed.keyLabel(pitch);  // (a Pad Kit's pad: its name on every key that has one)
        if (isBlackKey(pitch))  // (folded or named after a pad, narrower: its name right of it)
            ui::style::box(p,
                           aliased(0, y, KEYS_W * (!label.empty() ? look.namedShare : folded ? look.foldedShare : look.blackShare),
                                   ed.rowH),
                           *look.keyBlack);
        if (!isBlackKey(pitch) || folded)  // (folded: a line under every key, none are next to theirs)
            p.fillRect(Rect(0, y + ed.rowH - look.keyLineH, KEYS_W, look.keyLineH), look.keyLine);
        if (ed.held && *ed.held == pitch) ui::style::box(p, Rect(0, y, KEYS_W, ed.rowH), *look.sounding);
        bool named = pitch % 12 == 0 || folded || (steps && pitch % 12 == root) || !label.empty();
        if (named && ed.rowH >= 9)
            p.drawText(RectF(0, (float)y, (float)(KEYS_W - look.labelPad), (float)ed.rowH), AlignRight | AlignVCenter,
                       label.empty() ? noteName(pitch) : label, font, look.label, !label.empty());
    }
    p.fillRect(Rect(KEYS_W - look.keysEdgeW, 0, look.keysEdgeW, h), look.keysEdge);
}

bool PianoRoll::mousePress(ui::MouseEvent& e) {
    MidiEditor& ed = *ed_;
    using K = MidiEditor::Drag::Kind;
    Point pos = e.pos;
    if (pos.y + ed.vbar->value() >= (int)ed.rows().size() * ed.rowH) return true;  // (below the last row, folded)
    if (pos.x < keysW()) {  // a key: it sounds while held; dragging over the keys plays each one passed
        if (e.button == plat::LeftButton && (e.mods & plat::ModShift)) {  // Shift: its row selected (Ctrl: added)
            ed.selectRow(ed.pitchAt(pos.y), e.mods & plat::ModCtrl);
        } else if (e.button == plat::LeftButton) {
            ed.hold(ed.pitchAt(pos.y));
            ed.drag = MidiEditor::Drag();
            ed.drag.kind = K::Keys;
            ed.refresh();
        }
        return true;
    }
    if (ed.knife && e.button == plat::LeftButton) {  // the knife: a line, cutting on release
        MidiEditor::Drag d;
        d.kind = MidiEditor::Drag::Cut;
        d.startPos = d.pos = pos;
        ed.drag = d;
        ed.refresh();
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
        std::vector<int> pitches = ed.stamped(ed.onScale(ed.pitchAt(pos.y), e.mods));
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
        int index = pos.x >= keysW() ? ed.noteAt(pos.x, pos.y, &atEnd) : -1;
        setCursor(ed.knife && pos.x >= keysW() ? ui::Cursor::Cross
                  : index < 0                  ? ui::Cursor::Arrow
                  : atEnd                      ? ui::Cursor::SizeHor
                                               : ui::Cursor::OpenHand);
        return true;
    }
    if (ed.drag.kind == K::Keys) {
        ed.glide(ed.pitchAt(pos.y));
        ed.refresh();
        return true;
    }
    if (ed.drag.kind == K::Cut) {
        ed.drag.pos = pos;
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
            if (n.pitch != kv.second.pitch)  // (moved up or down: onto the scale, when snapping to it)
                n.pitch = ed.onScale((int)n.pitch, e.mods, n.pitch > kv.second.pitch ? 1 : -1);
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
    } else if (d.kind == K::Cut) {  // the knife: the notes on the rows passed, crossing the line, cut there
        double at = ed.snap(ed.tAt(d.startPos.x), e.mods);
        int r0 = ed.rowAt(std::min(d.startPos.y, d.pos.y)), r1 = ed.rowAt(std::max(d.startPos.y, d.pos.y));
        std::vector<int> which;
        for (int i = 0; i < (int)ed.notes.size(); ++i) {
            std::optional<int> r = ed.rowOf((int)ed.notes[i].pitch);
            if (r && r0 <= *r && *r <= r1) which.push_back(i);
        }
        ed.splitNotes(at, which);
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
    setStyleType("VelocityLane");
    setFixedHeight(velocityH());
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
    const ui::style::Insets& m = look().chooserMargin;
    chooser->setGeometry(Rect(m.left, m.top, keysW() - m.w(), hint.y));
}

std::string VelocityLane::toolTipAt(Point p) const {
    if (p.x < keysW()) return "";
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

const VelocityLane::Look& VelocityLane::look() {
    if (!parts_.refresh()) return look_;
    Look l;
    l.notes.read(parts_);
    l.self = &parts_.get("");
    l.middle = panel_style::fg(parts_.get("grid", "middle"));
    const css::Style& bar = parts_.get("bar");
    l.barW = std::max(1, panel_style::px(bar, "width", 4));
    l.selected = panel_style::bg(parts_.get("bar", "selected"));
    const css::Style& cap = parts_.get("cap");
    l.capW = std::max(1, panel_style::px(cap, "width", 8));
    l.capH = std::max(1, panel_style::px(cap, "height", 3));
    const css::Style& label = parts_.get("label");
    l.label = &label;
    l.labelText = panel_style::fg(label);
    l.labelPad = ui::style::insets(label, "padding");
    l.labelH = std::max(1, panel_style::px(label, "height", 16));
    l.font = ui::style::font(label, theme::uiFont());
    l.chooserMargin = ui::style::insets(parts_.get("chooser"), "margin");
    look_ = l;
    return look_;
}

void VelocityLane::paint(Painter& p) {
    MidiEditor& ed = *ed_;
    const Look& look = this->look();
    const NoteLook& nl = look.notes;
    const int KEYS_W = keysW();
    const LaneInfo& info = laneInfo(ed.lane);
    int w = width(), h = height();
    ui::style::box(p, rect(), *look.self);  // (its top border: the line under the notes)
    // where a value's bar reaches (0…1 of the height); velocity counts from 0, as it always has
    auto along = [&](double v) {
        return info.lane == Lane::Velocity ? v / 127 : (v - info.low) / (info.high - info.low);
    };
    if (info.lane == Lane::Velocity) {
        for (int level : {32, 64, 96, 127}) {
            int y = (int)(h - 3 - level / 127.0 * (h - 6));
            p.fillRect(Rect(KEYS_W, y, w, nl.gridW), level != 127 ? nl.grid[1] : nl.grid[0]);
        }
    } else {
        for (double part : {0.25, 0.5, 0.75, 1.0}) {
            int y = (int)(h - 3 - part * (h - 6));
            p.fillRect(Rect(KEYS_W, y, w, nl.gridW),
                       part == 1.0 ? nl.grid[0] : info.centred && part == 0.5 ? look.middle : nl.grid[1]);
        }
    }
    Color color = ed.color();
    Color light = color.lighter(130), dark = color.darker(150);
    double base = info.centred ? 0.5 : 0.0;  // (pan and fine: from the middle line, up or down)
    double capOut = (look.capW - look.barW) / 2.0;  // (the cap sticks out on both sides)
    for (size_t i = 0; i < ed.notes.size(); ++i) {
        const Note& n = ed.notes[i];
        double x = ed.xOf(n.start);
        if (x < KEYS_W - 4 || x > w || !ed.shows(n)) continue;  // (folded away: not in the roll, nor here)
        double top = along(laneValue(n, info.lane)) * (h - 6), bottom = base * (h - 6);
        double hi = std::max(top, bottom), lo = std::min(top, bottom);
        Color fill = ed.selected.count((int)i) ? look.selected : !ed.off.count((int)i) ? light : dark;
        p.fillRect(aliased(x, h - 3 - hi, look.barW, hi - lo), fill);
        p.fillRect(aliased(x - capOut, h - 2 - look.capH - top, look.capW, look.capH), fill);
    }
    ed.paintCursorLines(p, h, nl);
    ui::style::box(p, Rect(0, 0, KEYS_W, h), *look.label);
    const ui::style::Insets& pad = look.labelPad;
    p.drawText(RectF((float)pad.left, (float)(chooser->y() + chooser->height() + pad.top), (float)(KEYS_W - pad.w()),
                     (float)look.labelH),
               AlignLeft | AlignTop, info.range, look.font, look.labelText);
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
    if (e.button != plat::LeftButton || e.pos.x < keysW()) return true;
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
    setStyleType("EditorRuler");
    setFixedHeight(rulerH());
    setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Fixed);
    setCursor(ui::Cursor::Hand);
    setToolTip("Click or drag to move the selection cursor (where Play starts)");
}

const EditorRuler::Look& EditorRuler::look() {
    if (!parts_.refresh()) return look_;
    Look l;
    const css::Style& self = parts_.get("");
    l.self = &self;
    l.text = panel_style::fg(self);
    l.font = ui::style::font(self, theme::uiFont());
    l.tick = std::max(0, panel_style::px(parts_.get("tick"), "height", 4));
    l.barTick = std::max(0, panel_style::px(parts_.get("tick", "bar"), "height", 8));
    l.secondTick = std::max(0, panel_style::px(parts_.get("tick", "second"), "height", 6));
    const css::Style& label = parts_.get("label");
    l.labelPad = panel_style::px(label, "padding-left", 4);
    l.baseline = panel_style::px(label, "top", 15);
    l.corner = &parts_.get("corner");
    l.cursor = panel_style::fg(parts_.get("cursor"));
    l.playhead = panel_style::fg(parts_.get("playhead"));
    look_ = l;
    return look_;
}

void EditorRuler::paint(Painter& p) {
    MidiEditor& ed = *ed_;
    const Look& look = this->look();
    const int KEYS_W = keysW();
    int w = width(), h = height();
    ui::style::box(p, rect(), *look.self);
    double t0 = ed.tAt(KEYS_W), t1 = ed.tAt(w);
    const Timing& timing = ed.project.timing;
    double origin = ed.originS();
    Color text = look.text;
    const Font& font = look.font;
    float pad = (float)look.labelPad, baseline = (float)look.baseline;
    if (timing.beats) {
        for (const GridLine& line : timing.lines(std::max(0.0, origin + t0 - 1), origin + t1, ed.pps)) {
            int x = (int)ed.xOf(line.seconds - origin);
            if (x < KEYS_W - 40) continue;
            bool label = !line.label.empty();
            p.drawVLine(x, label ? 0 : h - (line.strength == timing::BAR ? look.barTick : look.tick), h + 1, text);
            if (label) p.drawText(PointF((float)x + pad, baseline), line.label, font, text);
        }
    } else {
        double step = fmt::rulerStep(ed.pps);
        for (int k = (int)(std::max(0.0, origin + t0) / step); k < (int)((origin + t1) / step) + 2; ++k) {
            int x = (int)ed.xOf(k * step - origin);
            p.drawVLine(x, h - look.secondTick, h + 1, text);
            p.drawText(PointF((float)x + pad, baseline), fmt::time(k * step, fmt::stepDecimals(step)), font, text);
        }
    }
    std::optional<double> c = ed.cursorX();
    if (c && *c >= KEYS_W) paint::cursor(p, (int)*c, look.cursor, h);
    double ph = ed.playheadX();
    if (ph >= KEYS_W) paint::cursor(p, (int)ph, look.playhead, h);
    ui::style::box(p, Rect(0, 0, KEYS_W, h), *look.corner);  // (labels left of the notes area stay hidden)
}

bool EditorRuler::mousePress(ui::MouseEvent& e) {
    if (e.button == plat::LeftButton && e.pos.x >= keysW()) ed_->moveCursor(e.pos.x, e.mods);
    return true;
}

bool EditorRuler::mouseMove(ui::MouseEvent& e) {
    if ((e.buttons & plat::LeftButton) && e.pos.x >= keysW()) ed_->moveCursor(e.pos.x, e.mods);
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
        knife = false;  // (the piano roll's tool)
        release();
        double from = tAt(keysW());
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

void MidiEditor::setKnife(bool on) {
    knife = on && !stepsView;  // (the piano roll's)
    hint->setText(knife ? KNIFE_HINT : stepsView ? STEPS_HINT : ROLL_HINT);
    resized();  // (the hint's height)
    roll->setCursor(knife ? ui::Cursor::Cross : ui::Cursor::Arrow);
    util::debugf("MIDI editor: the knife %s\n", knife ? "out" : "away");
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
    int y = row * stepRowH();
    if (y < vbar->value()) vbar->setValue(y);  // (scrolled to it)
    else if (y + stepRowH() > vbar->value() + steps->height()) vbar->setValue(y + stepRowH() - steps->height());
    soundBriefly(pitch, lastVelocity, NOTE_SOUND_S);
    refresh();
}

// --- the Steps view's widget ---

StepGrid::StepGrid(MidiEditor* editor) : ed_(editor) {
    setStyleType("Steps");
    setMouseTracking(true);
    setFocusPolicy(ui::FocusPolicy::Strong);
    setMinimumHeight(theme::metric("piano-roll-min-height", 200));
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
    int r = (int)std::floor((double)(y + ed_->vbar->value()) / stepRowH());
    if (r < 0 || r > (int)ed_->stepRows().size()) return -1;
    return r;
}

std::optional<int> StepGrid::yOfRow(int row) const {
    int y = row * stepRowH() - ed_->vbar->value();
    if (y + stepRowH() < 0 || y > height()) return std::nullopt;
    return y;
}

void StepGrid::resized() { ed_->updateRanges(); }

void StepGrid::rowSwingMenu(int pitch, Point globalPos) {
    MidiEditor& ed = *ed_;
    TrackPtr track = std::atomic_load(&ed.clip->track);
    if (!track) return;
    ui::Menu menu;
    menu.addSection(ed.keyLabel(pitch).empty() ? noteName(pitch) : ed.keyLabel(pitch));
    MidiEditor* editor = &ed;
    song_scale::addRowSwingMenu(menu, ed.project, ed.engine.sr, track, pitch, [editor] { editor->refresh(); });
    menu.exec(globalPos, this);
}

bool StepGrid::overridesShortcut(const ui::KeyEvent& e) const {
    if (e.key == 'R' && !e.mods) return true;  // ("+ Row")
    if (e.key == plat::Key_F2 && !e.mods) return true;  // (the row's menu)
    return ed_->roll->overridesShortcut(e);
}

std::string StepGrid::toolTipAt(Point p) const {
    int r = rowAt(p.y);
    if (r < 0) return "";
    if (r == (int)ed_->stepRows().size()) return p.x < keysW() ? "Add a row for another pitch" : "";
    if (p.x < keysW()) return "Press to hear this pitch; right-click (F2): its swing";
    return "Click to add a note one step long here (or remove the one here); drag along to paint. Ctrl+click: "
           "select its notes; right-click: delete them";
}

const StepGrid::Look& StepGrid::look() {
    if (!parts_.refresh()) return look_;
    Look l;
    l.notes.read(parts_);
    l.self = &parts_.get("");
    l.row = &parts_.get("row");
    const css::Style& pad = parts_.get("pad");
    l.pad = panel_style::bg(pad);
    l.padRadius = (float)std::max(0.0, pad.length("border-top-left-radius", 2));
    l.padMargin = ui::style::insets(pad, "margin");
    std::optional<std::string> darken = parts_.get("pad", "odd").variable("--darken");
    l.padDarken = 112;
    if (darken) {
        css::Style probe;  // (the variable's text as a number)
        probe.props_["x"] = *darken;
        l.padDarken = std::max(1, (int)std::lround(probe.number("x", 112)));
    }
    l.shade = &parts_.get("shade");
    l.noteMargin = ui::style::insets(parts_.get("note"), "margin");
    l.keys = &parts_.get("keys");
    const css::Style& line = parts_.get("key-line");
    l.keyLine = panel_style::bg(line);
    l.keyLineH = std::max(0, panel_style::px(line, "height", 1));
    const css::Style& label = parts_.get("key-label");
    l.label = panel_style::fg(label);
    l.labelPad = panel_style::px(label, "padding-right", 4);
    l.font = ui::style::font(label, theme::uiFont());
    l.keyBlack = &parts_.get("key", "black");
    l.keyBlackW = std::max(0, panel_style::px(*l.keyBlack, "width", 6));
    l.sounding = &parts_.get("key", "sounding");
    const css::Style& add = parts_.get("add-row");
    l.addRow = panel_style::fg(add);
    l.addRowPad = ui::style::insets(add, "padding");
    l.addRowFont = ui::style::font(add, theme::uiFont());
    const css::Style& edge = parts_.get("keys-edge");
    l.keysEdge = panel_style::bg(edge);
    l.keysEdgeW = std::max(0, panel_style::px(edge, "width", 1));
    look_ = l;
    return look_;
}

void StepGrid::paint(Painter& p) {
    MidiEditor& ed = *ed_;
    const Look& look = this->look();
    const int KEYS_W = keysW(), STEP_ROW_H = stepRowH();
    int w = width(), h = height();
    std::vector<int> rows = ed.stepRows();
    const Timing& timing = ed.project.timing;
    double origin = ed.originS();
    ui::style::box(p, rect(), *look.self);
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
    // (the pads: rounded fills of fractional rectangles, Steps::pad's colour, radius and margin: style::box once it
    // takes a RectF)
    Color pad = look.pad;
    Color padOdd = pad.darker(look.padDarken);
    const ui::style::Insets& pm = look.padMargin;
    float padRadius = theme::rounded ? look.padRadius : 0.f;
    int rowsDown = (int)rows.size() * STEP_ROW_H - ed.vbar->value();
    for (size_t r = 0; r < rows.size(); ++r) {
        std::optional<int> y = yOfRow((int)r);
        if (!y) continue;
        ui::style::box(p, Rect(KEYS_W, *y, w, STEP_ROW_H), *look.row);
        for (size_t k = 0; k < cells.size(); ++k) {
            double x0 = ed.xOf(std::max(0.0, cells[k].first)), x1 = ed.xOf(cells[k].second);
            RectF cell((float)x0 + pm.left, (float)*y + pm.top, (float)std::max(1.0, x1 - x0 - pm.w()),
                       (float)(STEP_ROW_H - pm.h()));
            if (padRadius > 0.01f) p.fillRoundedRect(cell, padRadius, even[k] ? pad : padOdd);
            else p.fillRect(cell, even[k] ? pad : padOdd);
        }
    }
    double lo, hi;  // shade what the clip doesn't play
    ed.windowS(lo, hi);
    int bottom = std::min(h, rowsDown);
    if (bottom > 0) {
        ui::style::box(p, aliased(KEYS_W, 0, std::max(0.0, ed.xOf(lo) - KEYS_W), bottom), *look.shade);
        ui::style::box(p, aliased(ed.xOf(hi), 0, w, bottom), *look.shade);
    }
    std::map<int, int> rowOf;
    for (size_t r = 0; r < rows.size(); ++r) rowOf[rows[r]] = (int)r;
    Color color = ed.color();
    const ui::style::Insets& nm = look.noteMargin;
    for (size_t i = 0; i < ed.notes.size(); ++i) {
        const Note& n = ed.notes[i];
        std::optional<int> y = yOfRow(rowOf[(int)n.pitch]);
        if (!y) continue;
        double x0 = ed.xOf(n.start), x1 = ed.xOf(n.end);
        if (x1 < KEYS_W || x0 > w) continue;
        RectF r((float)x0 + nm.left, (float)*y + nm.top, (float)std::max(NOTE_MIN_PX, x1 - x0 - nm.w()),
                (float)(STEP_ROW_H - nm.h()));
        paintNoteBox(p, r, n, color, ed.selected.count((int)i) > 0, ed.off.count((int)i) > 0, look.notes);
        if (n.slide)  // (a tail into it, as in the piano roll)
            paintSlideTail(p, PointF((float)x0 - 9, (float)*y + STEP_ROW_H - 3), PointF((float)x0 + 2, (float)*y + STEP_ROW_H / 2.0f),
                           color.lighter(170), look.notes);
    }
    ed.paintCursorLines(p, h, look.notes);
    // the rows' names (the piano roll's keys), and "+ Row"
    ui::style::box(p, Rect(0, 0, KEYS_W, std::min(h, rowsDown)), *look.keys);
    const Font& font = look.font;
    for (size_t r = 0; r <= rows.size(); ++r) {
        std::optional<int> y = yOfRow((int)r);
        if (!y) continue;
        if (r == rows.size()) {
            const ui::style::Insets& ap = look.addRowPad;
            p.drawText(RectF((float)ap.left, (float)*y, (float)(KEYS_W - ap.w()), (float)STEP_ROW_H), AlignLeft | AlignVCenter,
                       "+ Row", look.addRowFont, look.addRow);
            continue;
        }
        int pitch = rows[r];
        if (ed.held && *ed.held == pitch) ui::style::box(p, Rect(0, *y, KEYS_W, STEP_ROW_H), *look.sounding);
        else if (isBlackKey(pitch)) ui::style::box(p, Rect(0, *y, look.keyBlackW, STEP_ROW_H), *look.keyBlack);
        p.fillRect(Rect(0, *y + STEP_ROW_H - look.keyLineH, KEYS_W, look.keyLineH), look.keyLine);
        std::string label = ed.keyLabel(pitch);
        p.drawText(RectF(0, (float)*y, (float)(KEYS_W - look.labelPad), (float)STEP_ROW_H), AlignRight | AlignVCenter,
                   label.empty() ? noteName(pitch) : label, font, look.label, !label.empty());
    }
    p.fillRect(Rect(KEYS_W - look.keysEdgeW, 0, look.keysEdgeW, h), look.keysEdge);
}

bool StepGrid::mousePress(ui::MouseEvent& e) {
    MidiEditor& ed = *ed_;
    std::vector<int> rows = ed.stepRows();
    int r = rowAt(e.pos.y);
    if (r < 0) return true;
    if (r == (int)rows.size()) {  // "+ Row"
        if (e.button == plat::LeftButton && e.pos.x < keysW())
            rowMenu->popup(mapToGlobal(Point(0, *yOfRow(r) + stepRowH())), this);
        return true;
    }
    int pitch = rows[(size_t)r];
    if (e.pos.x < keysW() && e.button == plat::RightButton) {  // a row's name: its menu (its swing)
        rowSwingMenu(pitch, e.globalPos);
        return true;
    }
    if (e.pos.x < keysW()) {  // a row's name: it sounds while held
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
        if (onRow && e.pos.x >= keysW() && ed.setCell(rows[(size_t)r], ed.tAt(e.pos.x), ed.drag.lighting)) {
            ed.drag.changed = true;
            if (ed.drag.lighting) ed.hold(rows[(size_t)r]);
            ed.refresh();
        }
    } else {
        setCursor(onRow && e.pos.x >= keysW() ? ui::Cursor::Hand : ui::Cursor::Arrow);
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
