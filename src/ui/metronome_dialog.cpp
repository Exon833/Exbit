#include "ui/metronome_dialog.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "audio/engine.h"
#include "core/util.h"
#include "ui/theme.h"
#include "ui/toolkit/layout.h"

using namespace ui;
namespace m = metronome;

namespace metronome_dialog {

const char* const SETTINGS_KEY = "metronome/sounds";

void limit(m::Click& c) {
    c.attackMs = std::min(std::max(c.attackMs, 0.0), m::MAX_ATTACK_MS);
    c.sustainMs = std::min(std::max(c.sustainMs, 0.0), m::MAX_SUSTAIN_MS);
    c.releaseMs = std::min(std::max(c.releaseMs, 0.0), m::MAX_RELEASE_MS);
    c.levelDb = std::min(std::max(c.levelDb, m::MIN_DB), m::MAX_DB);
    c.pitch = std::min(std::max(c.pitch, m::MIN_PITCH), m::MAX_PITCH);
    bool known = false;
    for (const char* w : m::WAVEFORMS) known = known || c.waveform == w;
    if (!known) c.waveform = "sine";
}

namespace {
Json clickJson(const m::Click& c) {
    Json j = Json::object();
    j.set("pitch", c.pitch);
    j.set("level_db", c.levelDb);
    j.set("attack_ms", c.attackMs);
    j.set("sustain_ms", c.sustainMs);
    j.set("release_ms", c.releaseMs);
    j.set("waveform", c.waveform);
    return j;
}

// float(value) as Python does it: numbers, and strings of numbers
bool toNumber(const Json& v, double& out) {
    if (v.isNumber()) {
        out = v.toDouble();
        return true;
    }
    if (v.isBool()) {
        out = v.toBool() ? 1 : 0;
        return true;
    }
    if (v.isString()) {
        std::string s = util::trim(v.toString());
        char* end = nullptr;
        out = strtod(s.c_str(), &end);
        return !s.empty() && end && *end == 0 && std::isfinite(out);
    }
    return false;
}

bool readClick(const Json& j, m::Click& c) {
    if (j.isNull()) return true;
    if (!j.isObject()) return false;
    struct Field { const char* key; double* v; };
    Field fs[] = {{"pitch", &c.pitch}, {"level_db", &c.levelDb}, {"attack_ms", &c.attackMs},
                  {"sustain_ms", &c.sustainMs}, {"release_ms", &c.releaseMs}};
    for (auto& f : fs)
        if (j.has(f.key) && !toNumber(j[f.key], *f.v)) return false;
    if (j.has("waveform")) {
        const Json& w = j["waveform"];
        if (w.isString()) c.waveform = w.toString();
        else c.waveform = w.dump();  // (str() of anything; limit() then makes it "sine")
    }
    limit(c);
    return true;
}
}  // namespace

Json toJson(const m::MetronomeSettings& s) {
    Json j = Json::object();
    j.set("bar", clickJson(s.bar));
    j.set("beat", clickJson(s.beat));
    return j;
}

m::MetronomeSettings fromJson(const Json& stored) {
    m::MetronomeSettings s;
    Json data = stored;
    if (stored.isString()) data = Json::parse(stored.toString());
    if (data.isNull()) return s;
    if (!data.isObject()) return m::MetronomeSettings();
    if (!readClick(data["bar"], s.bar) || !readClick(data["beat"], s.beat)) return m::MetronomeSettings();
    return s;
}

m::MetronomeSettings loadSettings() { return fromJson(settings::get(SETTINGS_KEY)); }

void saveSettings(const m::MetronomeSettings& s) { settings::set(SETTINGS_KEY, toJson(s)); }

void testClick(audio::Engine& engine, const m::Click& click) {
    std::vector<float> mono = m::renderClick(click, engine.sr);
    auto buf = std::make_shared<AudioBuffer>();
    buf->samples.resize(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); i++) buf->samples[i * 2] = buf->samples[i * 2 + 1] = mono[i];
    engine.preview(buf);
}

}  // namespace metronome_dialog

using metronome_dialog::GRAPH_MS;
using metronome_dialog::HANDLE_R;

// ---------------------------------------------------------------------------------------------
// EnvelopeGraph

EnvelopeGraph::EnvelopeGraph(m::Click* c, const std::string& role) : click(c) {
    setStyleType("EnvelopeGraph");
    if (!role.empty()) addStyleClass(role);
    setMinimumSize(240, 120);
    setSizePolicy(SizePolicy::Expanding, SizePolicy::Expanding);
    setMouseTracking(true);
    setToolTip("Drag the points: attack, sustain and release times; up and down for the level");
}

RectF EnvelopeGraph::area() const {
    float m = HANDLE_R + 2;
    return RectF(m, m, width() - 2 * m, height() - 2 * m);
}

double EnvelopeGraph::xOf(double ms) const {
    RectF a = area();
    return a.x + ms / GRAPH_MS * a.w;
}

double EnvelopeGraph::yOf(double db) const {
    RectF a = area();
    return a.y + a.h - (db - m::MIN_DB) / (m::MAX_DB - m::MIN_DB) * a.h;
}

std::vector<PointF> EnvelopeGraph::points() const {
    const m::Click& c = *click;
    float y = (float)yOf(c.levelDb), bottom = area().y + area().h;
    return {PointF((float)xOf(0), bottom), PointF((float)xOf(c.attackMs), y),
            PointF((float)xOf(c.attackMs + c.sustainMs), y),
            PointF((float)xOf(c.attackMs + c.sustainMs + c.releaseMs), bottom)};
}

const EnvelopeGraph::Look& EnvelopeGraph::look() const {
    if (!parts_.refresh()) return look_;
    Look l;
    const css::Style& self = parts_.get("");
    l.self = &self;
    l.color = panel_style::fg(self);
    const css::Style& grid = parts_.get("grid");
    l.grid = panel_style::fg(grid);
    l.gridW = panel_style::lengthF(grid, "stroke-width", 1);
    const css::Style& envelope = parts_.get("envelope");
    l.lineW = panel_style::lengthF(envelope, "stroke-width", 2);
    l.fillAlpha = (int)std::lround(std::min(1.0, std::max(0.0, envelope.number("fill-opacity", 60 / 255.0))) * 255);
    const css::Style& label = parts_.get("label");
    l.label = panel_style::fg(label);
    l.font = ui::style::font(label, theme::uiFont());
    const css::Style& handle = parts_.get("handle");
    l.handle = panel_style::bg(handle);
    l.handleEdge = panel_style::color(handle, "border-top-color", l.color);
    l.handleW = panel_style::lengthF(handle, "border-top-width", 2);
    l.handleDragging = panel_style::color(parts_.get("handle", "dragging"), "border-top-color", l.handleEdge);
    look_ = l;
    return look_;
}

Color EnvelopeGraph::color() const { return look().color; }

void EnvelopeGraph::paint(Painter& p) {
    const Look& look = this->look();
    RectF a = area();
    ui::style::box(p, rect(), *look.self);
    Color grid = look.grid;
    for (int ms = 0; ms <= GRAPH_MS; ms += 50) {  // a line every 50 ms
        float x = (float)xOf(ms);
        p.drawLine(PointF(x, a.y), PointF(x, a.y + a.h), grid, look.gridW);
    }
    for (int db : {-30, -20, -10, 0}) {
        float y = (float)yOf(db);
        p.drawLine(PointF(a.x, y), PointF(a.x + a.w, y), grid, look.gridW);
    }
    std::vector<PointF> pts = points();
    Path path;
    path.moveTo(pts[0].x, pts[0].y);
    for (size_t i = 1; i < pts.size(); i++) path.lineTo(pts[i].x, pts[i].y);
    p.fillPath(path, look.color.withAlpha(look.fillAlpha));
    p.strokePath(path, look.color, look.lineW);
    p.drawText(RectF(a.x + 4, a.y + 2, a.w - 8, a.h - 4), AlignRight | AlignTop, std::to_string(GRAPH_MS) + " ms",
               look.font, look.label);
    for (int i = 1; i < (int)pts.size(); i++) {
        RectF dot(pts[i].x - HANDLE_R, pts[i].y - HANDLE_R, 2 * HANDLE_R, 2 * HANDLE_R);
        p.fillEllipse(dot, look.handle);
        p.strokeEllipse(dot, i == dragging ? look.handleDragging : look.handleEdge, look.handleW);
    }
}

bool EnvelopeGraph::mousePress(MouseEvent& e) {
    std::vector<PointF> pts = points();
    double best = 1e9;
    int index = 0;
    for (int i = 1; i < (int)pts.size(); i++) {
        double d = std::fabs(pts[i].x - e.pos.x) + std::fabs(pts[i].y - e.pos.y);
        if (d < best || (d == best && i < index)) best = d, index = i;
    }
    dragging = best < 20 ? index : 0;
    update();
    return true;
}

bool EnvelopeGraph::mouseMove(MouseEvent& e) {
    if (!dragging) return false;
    RectF a = area();
    m::Click& c = *click;
    double ms = (e.pos.x - a.x) / a.w * GRAPH_MS;
    if (dragging == 1) c.attackMs = ms;
    else if (dragging == 2) c.sustainMs = ms - c.attackMs;
    else c.releaseMs = ms - c.attackMs - c.sustainMs;
    if (dragging == 1 || dragging == 2) c.levelDb = m::MIN_DB + (a.y + a.h - e.pos.y) / a.h * (m::MAX_DB - m::MIN_DB);
    metronome_dialog::limit(c);
    auto round1 = [](double v) { return std::round(v * 10.0) / 10.0; };
    c.attackMs = round1(c.attackMs), c.sustainMs = round1(c.sustainMs), c.releaseMs = round1(c.releaseMs);
    c.levelDb = round1(c.levelDb);
    update();
    changed.emit();
    return true;
}

bool EnvelopeGraph::mouseRelease(MouseEvent& e) {
    dragging = 0;
    update();
    return true;
}

// ---------------------------------------------------------------------------------------------
// ClickEditor

namespace {
std::string capitalize(const std::string& s) {
    if (s.empty()) return s;
    std::string out = util::lower(s);
    out[0] = (char)toupper((unsigned char)out[0]);
    return out;
}
double* fieldOf(m::Click& c, const std::string& attr) {
    if (attr == "attack_ms") return &c.attackMs;
    if (attr == "sustain_ms") return &c.sustainMs;
    if (attr == "release_ms") return &c.releaseMs;
    if (attr == "level_db") return &c.levelDb;
    return &c.pitch;
}
}  // namespace

ClickEditor::ClickEditor(const std::string& title, m::Click* c, const std::string& role,
                         std::function<void(const m::Click&)> test)
    : GroupBox(title), click(c) {
    graph = add<EnvelopeGraph>(c, role);
    graph->changed.connect([this] { fromGraph(); });
    auto* col = vbox(this, 0, 6);
    col->addWidget(graph, 1);
    auto formOwner = std::make_unique<FormLayout>();
    FormLayout* form = formOwner.get();
    form->spacing = 6;
    struct Row { const char* attr; const char* label; double lo, hi; const char* suffix; double step; };
    const Row rows[] = {{"attack_ms", "Attack", 0, m::MAX_ATTACK_MS, " ms", 0.5},
                        {"sustain_ms", "Sustain", 0, m::MAX_SUSTAIN_MS, " ms", 1},
                        {"release_ms", "Release", 0, m::MAX_RELEASE_MS, " ms", 1},
                        {"level_db", "Level", m::MIN_DB, m::MAX_DB, " dB", 0.5},
                        {"pitch", "Pitch", m::MIN_PITCH, m::MAX_PITCH, " Hz", 10}};
    col->addLayout(std::move(formOwner));
    for (auto& r : rows) {
        SpinBox* box = add<SpinBox>();
        box->setRange(r.lo, r.hi);
        box->setDecimals(1);
        box->setSingleStep(r.step);
        box->setSuffix(r.suffix);
        box->setKeyboardTracking(false);
        std::string attr = r.attr;
        box->valueChanged.connect([this, attr](double v) { setValue(attr, v); });
        boxes[attr] = box;
        form->addRow(r.label, box);
    }
    waveform = add<ComboBox>();
    for (const char* w : m::WAVEFORMS) waveform->addItem(capitalize(w), w);
    waveform->currentIndexChanged.connect([this](int i) {
        if (i >= 0 && i < 4) setWaveform(m::WAVEFORMS[i]);
    });
    form->addRow("Waveform", waveform);
    testButton = add<Button>("▶ Test");
    testButton->setToolTip("Play this click once");
    testButton->clicked.connect([this, test] {
        if (test) test(*click);
    });
    col->addWidget(testButton);
    showValues();
}

void ClickEditor::showValues() {
    for (auto& kv : boxes) kv.second->setValueSilently(*fieldOf(*click, kv.first));
    int index = 0;
    for (int i = 0; i < 4; i++)
        if (click->waveform == m::WAVEFORMS[i]) index = i;
    waveform->setCurrentIndexSilently(index);
    boxes["pitch"]->setEnabled(click->waveform != "noise");
    graph->update();
}

void ClickEditor::setValue(const std::string& attr, double value) {
    *fieldOf(*click, attr) = value;
    metronome_dialog::limit(*click);
    showValues();
    changed.emit();
}

void ClickEditor::setWaveform(const std::string& w) {
    click->waveform = w;
    metronome_dialog::limit(*click);
    showValues();
    changed.emit();
}

void ClickEditor::fromGraph() {
    showValues();
    changed.emit();
}

// ---------------------------------------------------------------------------------------------
// MetronomeDialog

MetronomeDialog::MetronomeDialog(Window* parent, m::MetronomeSettings& s, Apply apply,
                                 std::function<void(const m::Click&)> test)
    : Dialog(parent, "Metronome", 560, 420), settings(s), apply_(std::move(apply)) {
    auto* col = vbox(this, 11, 6);
    auto rowOwner = std::make_unique<HBox>();  // the two envelopes right next to each other
    HBox* row = rowOwner.get();
    row->setSpacing(6);
    editors.push_back(add<ClickEditor>("Start of the bar", &settings.bar, "bar", test));
    editors.push_back(add<ClickEditor>("Other beats", &settings.beat, "beat", test));
    for (ClickEditor* e : editors) {
        e->changed.connect([this] { changed(); });
        row->addWidget(e, 1);
    }
    col->addLayout(std::move(rowOwner), 1);
    Button* defaults = add<Button>("Defaults");
    Button* close = add<Button>("Close");
    defaults->setToolTip("Back to the standard metronome sounds");
    defaults->clicked.connect([this] { useDefaults(); });
    close->setDefault(true);
    close->setMinimumWidth(80);
    close->clicked.connect([this] { accept(); });
    auto buttonsOwner = std::make_unique<HBox>();
    HBox* buttons = buttonsOwner.get();
    buttons->addWidget(defaults);
    buttons->addStretch(1);
    buttons->addWidget(close);
    col->addLayout(std::move(buttonsOwner));
    adjustSize();
}

void MetronomeDialog::changed() {
    metronome_dialog::saveSettings(settings);
    if (apply_) apply_(settings);
}

void MetronomeDialog::useDefaults() {
    m::MetronomeSettings fresh;
    *editors[0]->click = fresh.bar;
    *editors[1]->click = fresh.beat;
    for (ClickEditor* e : editors) e->showValues();
    changed();
}
