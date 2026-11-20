#include "ui/note_tools_panel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

#include "core/util.h"
#include "ui/midi_editor.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/menu.h"
#include "ui/widgets.h"

using note_tools::Tool;

namespace {

// the note values a rate or grid can be, in beats
const char* const VALUES[] = {"1/4", "1/8", "1/16", "1/32"};
const double VALUE_BEATS[] = {1.0, 0.5, 0.25, 0.125};

struct Spin {
    const char* label = nullptr;  // null: not used by the tool
    double lo = 0, hi = 0, def = 0;
    const char* suffix = "";
    const char* tip = "";
};
struct Choice {
    const char* label = nullptr;
    std::vector<const char*> items;
    int def = 0;
};
struct ToolControls {
    Spin spins[3];
    Choice choices[2];
    bool ends = false;  // Quantize's "ends too"
};

const ToolControls& controlsOf(Tool t) {
    static const std::vector<ToolControls> all = [] {
        std::vector<ToolControls> v(note_tools::TOOL_COUNT);
        v[(int)Tool::Strum].spins[0] = {"Time per note", 1, 500, 30, " ms", "How much later each note of a chord starts"};
        v[(int)Tool::Strum].spins[1] = {"Curve", -100, 100, 0, " %", "0: even; above: slow then fast; below: fast then slow"};
        v[(int)Tool::Strum].choices[0] = {"Direction", {"Up (lowest first)", "Down (highest first)"}, 0};
        v[(int)Tool::Arpeggiate].spins[0] = {"Gate", 5, 100, 80, " %", "How long each note is, of the step"};
        v[(int)Tool::Arpeggiate].spins[1] = {"Seed", 0, 9999, 1, "", "Random: another number, another order"};
        v[(int)Tool::Arpeggiate].choices[0] = {"Rate", {VALUES[0], VALUES[1], VALUES[2], VALUES[3]}, 2};
        v[(int)Tool::Arpeggiate].choices[1] = {"Style", {"Up", "Down", "Up and down", "Random"}, 0};
        v[(int)Tool::Chop].spins[0] = {"Pieces", 2, 64, 2, "", "Into how many equal pieces"};
        v[(int)Tool::Chop].choices[0] = {"Chop into", {"Pieces", "Grid steps (the editor's grid)"}, 0};
        v[(int)Tool::Flam].spins[0] = {"Time", 1, 200, 20, " ms", "How long before the note its grace note starts"};
        v[(int)Tool::Flam].spins[1] = {"Velocity", 1, 100, 60, " %", "The grace note's velocity, of the note's"};
        v[(int)Tool::Quantize].spins[0] = {"Strength", 0, 100, 100, " %", "How far towards the grid (100 %: onto it)"};
        v[(int)Tool::Quantize].choices[0] = {"Grid", {"The editor's grid", VALUES[0], VALUES[1], VALUES[2], VALUES[3]}, 0};
        v[(int)Tool::Quantize].ends = true;
        v[(int)Tool::Humanize].spins[0] = {"Timing \xc2\xb1", 0, 100, 10, " ms", "Starts moved by up to this much"};
        v[(int)Tool::Humanize].spins[1] = {"Velocity \xc2\xb1", 0, 64, 10, "", "Velocities changed by up to this much"};
        v[(int)Tool::Humanize].spins[2] = {"Seed", 0, 9999, 1, "", "Another number, other changes"};
        v[(int)Tool::VelocityShape].spins[0] = {"From", 1, 127, 40, "", "The first note's velocity"};
        v[(int)Tool::VelocityShape].spins[1] = {"To", 1, 127, 120, "", "The last note's velocity"};
        v[(int)Tool::VelocityShape].spins[2] = {"Curve", -100, 100, 0, " %", "0: straight; above: rises early; below: late"};
        return v;
    }();
    return all[(size_t)std::clamp((int)t, 0, note_tools::TOOL_COUNT - 1)];
}

// each tool's controls as last set (3 spins, 2 choices, ends), while the program runs
using Values = std::array<double, 6>;
std::map<int, Values>& remembered() {
    static std::map<int, Values> m;
    return m;
}
int gLastTool = 0;

Values defaults(Tool t) {
    const ToolControls& c = controlsOf(t);
    return {c.spins[0].def, c.spins[1].def, c.spins[2].def, (double)c.choices[0].def, (double)c.choices[1].def, 0};
}

// the grid notes go to: `beats` long (0: the editor's grid at its zoom, as Quantize in its toolbar)
void gridOf(const MidiEditor& ed, double beats, note_tools::Params& p) {
    const Timing& timing = ed.project.timing;
    double origin = ed.originS();
    if (beats > 0) {
        p.onGrid = [&timing, origin, beats](double t) {
            return timing.secondsAt(std::nearbyint(timing.beatAt(origin + t) / beats) * beats) - origin;
        };
        p.stepAt = [&timing, origin, beats](double t) {
            return timing.secondsAt(timing.beatAt(origin + t) + beats) - origin - t;
        };
        return;
    }
    double step = ed.stepS(), pps = ed.pps;
    p.onGrid = [&timing, origin, step, pps](double t) {
        return timing.beats ? timing.snap(origin + t, pps) - origin : std::nearbyint((origin + t) / step) * step - origin;
    };
    p.stepAt = [&ed](double t) { return ed.stepS(t); };
}

}  // namespace

// =================================================================================================
// the panel
// =================================================================================================

NoteToolsPanel::NoteToolsPanel(MidiEditor* editor_) : ui::Dialog(editor_, "Note tools", 320, 260), editor(editor_) {
    deleteOnClose = true;
    ui::FormLayout* form = ui::styleRoom(ui::form(this, 11, 6), computedStyle());
    tool = add<ui::ComboBox>();
    for (int i = 0; i < note_tools::TOOL_COUNT; ++i) tool->addItem(note_tools::toolName((Tool)i));
    tool->setToolTip("What to do to the notes: the result shows ghosted in the editor until Apply");
    form->addRow("Tool", tool);
    target = add<ui::Label>("");
    form->addRow(target);
    for (int i = 0; i < 3; ++i) {
        labels[i] = add<ui::Label>("");
        spins[i] = add<ui::SpinBox>();
        spins[i]->setKeyboardTracking(true);
        spins[i]->valueChanged.connect([this](double) { updatePreview(); });
        form->addRow(labels[i], spins[i]);
    }
    for (int i = 0; i < 2; ++i) {
        labels[3 + i] = add<ui::Label>("");
        choices[i] = add<ui::ComboBox>();
        choices[i]->currentIndexChanged.connect([this](int) { updatePreview(); });
        form->addRow(labels[3 + i], choices[i]);
    }
    ends = add<ui::CheckBox>("Quantize the ends too");
    ends->toggled.connect([this](bool) { updatePreview(); });
    form->addRow(ends);
    auto* buttons = add<ui::ButtonBox>(ui::ButtonBox::Cancel);
    applyButton = buttons->addButton("Apply", 1);
    buttons->accepted.connect([this] { apply(); });
    buttons->rejected.connect([this] { reject(); });
    form->addRow(buttons);
    setTool((Tool)gLastTool);
    tool->currentIndexChanged.connect([this](int i) {
        remember();
        setTool((Tool)i);
    });
}

NoteToolsPanel::~NoteToolsPanel() {
    if (editor && editor->toolsPanel == this) editor->toolsPanel = nullptr;
}

void NoteToolsPanel::remember() {
    int t = gLastTool;
    remembered()[t] = {spins[0]->value(), spins[1]->value(), spins[2]->value(), (double)choices[0]->currentIndex(),
                       (double)choices[1]->currentIndex(), ends->isChecked() ? 1.0 : 0.0};
}

void NoteToolsPanel::setTool(Tool t) {
    gLastTool = (int)t;
    tool->setCurrentIndexSilently((int)t);
    const ToolControls& c = controlsOf(t);
    auto it = remembered().find((int)t);
    Values v = it != remembered().end() ? it->second : defaults(t);
    for (int i = 0; i < 3; ++i) {
        const Spin& s = c.spins[i];
        bool used = s.label != nullptr;
        labels[i]->setVisible(used);
        spins[i]->setVisible(used);
        if (!used) continue;
        labels[i]->setText(s.label);
        spins[i]->setRange(s.lo, s.hi);
        spins[i]->setSuffix(s.suffix);
        spins[i]->setToolTip(s.tip);
        spins[i]->setValueSilently(std::clamp(v[(size_t)i], s.lo, s.hi));
    }
    for (int i = 0; i < 2; ++i) {
        const Choice& ch = c.choices[i];
        bool used = ch.label != nullptr;
        labels[3 + i]->setVisible(used);
        choices[i]->setVisible(used);
        if (!used) continue;
        labels[3 + i]->setText(ch.label);
        choices[i]->clear();
        for (const char* item : ch.items) choices[i]->addItem(item);
        choices[i]->setCurrentIndexSilently(std::clamp((int)v[3 + (size_t)i], 0, (int)ch.items.size() - 1));
    }
    ends->setVisible(c.ends);
    ends->setCheckedSilently(c.ends && v[5] != 0);
    adjustSize();
    updatePreview();
}

note_tools::Params NoteToolsPanel::params() const {
    note_tools::Params p;
    p.tool = (Tool)std::clamp(tool->currentIndex(), 0, note_tools::TOOL_COUNT - 1);
    double a = spins[0]->value(), b = spins[1]->value(), c = spins[2]->value();
    int x = std::max(0, choices[0]->currentIndex()), y = std::max(0, choices[1]->currentIndex());
    if (!editor) return p;
    const Timing& timing = editor->project.timing;
    switch (p.tool) {
    case Tool::Strum:
        p.strumS = a / 1000;
        p.curve = b / 100;
        p.down = x == 1;
        break;
    case Tool::Arpeggiate:
        p.gate = a / 100;
        p.seed = (uint32_t)std::max(0.0, b);
        // shortcut: the rate in seconds at the tempo where the clip starts (a tempo change inside it isn't followed)
        p.rateS = VALUE_BEATS[std::min(x, 3)] * timing.beatSAt(editor->originS());
        p.style = (note_tools::ArpStyle)std::min(y, 3);
        break;
    case Tool::Chop: {
        p.pieces = (int)std::lround(a);
        if (x == 1) {  // (the grid step where the first of the notes starts)
            double first = 1e300;
            for (int i : editor->toolTargets()) first = std::min(first, editor->notes[(size_t)i].start);
            p.chopStepS = editor->stepS(first < 1e300 ? first : 0.0);
        }
        break;
    }
    case Tool::Flam:
        p.flamS = a / 1000;
        p.flamVelocity = b / 100;
        break;
    case Tool::Legato: break;
    case Tool::Quantize:
        p.strength = a / 100;
        p.ends = ends->isChecked();
        gridOf(*editor, x == 0 ? 0.0 : VALUE_BEATS[std::min(x - 1, 3)], p);
        break;
    case Tool::Humanize:
        p.timingS = a / 1000;
        p.velocityRange = b;
        p.seed = (uint32_t)std::max(0.0, c);
        break;
    case Tool::VelocityShape:
        p.from = a;
        p.to = b;
        p.curve = c / 100;
        break;
    }
    return p;
}

void NoteToolsPanel::updatePreview() {
    if (!editor) return;
    std::vector<int> targets = editor->toolTargets();
    size_t all = editor->notes.size();
    target->setText(editor->selected.empty() ? "On all " + std::to_string(all) + " notes"
                                             : "On the " + std::to_string(targets.size()) + " selected notes");
    note_tools::Result r = note_tools::apply(editor->notes, targets, params());
    std::vector<char> isTarget(all, 0);
    for (int i : targets) isTarget[(size_t)i] = 1;
    editor->preview.clear();
    for (size_t k = 0; k < r.notes.size(); ++k)
        if (isTarget[(size_t)r.from[k]]) editor->preview.push_back(r.notes[k]);
    editor->refresh();
}

void NoteToolsPanel::apply() {
    if (editor) {
        note_tools::Params p = params();
        remember();
        MidiEditor* ed = editor;
        ed->preview.clear();
        ed->applyResult(note_tools::apply(ed->notes, ed->toolTargets(), p), note_tools::toolName(p.tool));
    }
    accept();
}

void NoteToolsPanel::done(int result) {
    if (editor) {
        remember();
        if (editor->toolsPanel == this) editor->toolsPanel = nullptr;
        editor->preview.clear();
        editor->refresh();
        util::debugf("note tools: %s\n", result == Accepted ? "applied" : "cancelled (the preview dropped)");
    }
    editor = nullptr;
    ui::Dialog::done(result);
}

// =================================================================================================
// the MIDI editor's tool commands (midi_editor.h)
// =================================================================================================

std::vector<int> MidiEditor::toolTargets() const {
    std::vector<int> out;
    for (int i : selected)
        if (i >= 0 && i < (int)notes.size()) out.push_back(i);
    if (out.empty())
        for (int i = 0; i < (int)notes.size(); ++i) out.push_back(i);
    return out;
}

void MidiEditor::openTools() {
    if (!toolsPanel) {
        toolsPanel = new NoteToolsPanel(this);
        toolsPanel->open();
        Rect g = screenGeometry();  // (beside the editor, at its right)
        toolsPanel->moveWindow(g.x + g.w + 8, g.y);
        util::debugf("note tools: opened on \"%s\"\n", clip ? clip->name.c_str() : "");
    }
    toolsPanel->updatePreview();
    toolsPanel->raiseWindow();
}

void MidiEditor::cancelPreview() {
    if (toolsPanel) toolsPanel->reject();  // (done(): the preview goes)
    toolsPanel = nullptr;
    preview.clear();
}

bool MidiEditor::applyResult(const note_tools::Result& r, const char* what) {
    bool changed = r.notes.size() != notes.size();
    for (size_t k = 0; !changed && k < r.notes.size(); ++k) {
        const Note &a = r.notes[k], &b = notes[(size_t)r.from[k]];
        changed = a.start != b.start || a.end != b.end || a.pitch != b.pitch || a.velocity != b.velocity;
    }
    util::debugf("MIDI editor: %s: %zu notes -> %zu%s\n", what, notes.size(), r.notes.size(),
                 changed ? "" : " (nothing changed)");
    if (!changed) return false;
    std::set<int> chosen, offs;
    for (size_t k = 0; k < r.notes.size(); ++k) {
        if (selected.count(r.from[k])) chosen.insert((int)k);
        if (off.count(r.from[k])) offs.insert((int)k);
    }
    notes = r.notes;
    selected = std::move(chosen);
    off = std::move(offs);
    commit();
    return true;
}

void MidiEditor::quantize(double strength) {
    note_tools::Params p;
    p.tool = Tool::Quantize;
    p.strength = strength;
    p.ends = quantizeEnds && quantizeEnds->checked;
    gridOf(*this, 0.0, p);
    applyResult(note_tools::apply(notes, toolTargets(), p), "Quantize");
}

void MidiEditor::splitNotes(double at, const std::vector<int>& which) {
    applyResult(note_tools::split(notes, which, at), "Split");
}

void MidiEditor::splitAtKeyCursor() {
    startKeyCursor();
    std::vector<int> which;
    for (int i : selected)
        if (i < (int)notes.size()) which.push_back(i);
    if (which.empty()) which = note_tools::row(notes, keyPitch);  // (none selected: the cursor's row)
    splitNotes(keyT, which);
}

void MidiEditor::joinNotes() { applyResult(note_tools::join(notes, toolTargets()), "Join"); }

void MidiEditor::selectRow(int pitch, bool add) {
    if (!add) selected.clear();
    for (int i : note_tools::row(notes, pitch)) selected.insert(i);
    util::debugf("MIDI editor: the row of %s selected (%zu notes)\n", midi_editor::noteName(pitch).c_str(),
                 selected.size());
    refresh();
}

void MidiEditor::removeOverlaps() { applyResult(note_tools::removeOverlaps(notes, toolTargets()), "Remove overlaps"); }

void MidiEditor::legatoAll() { applyResult(note_tools::legato(notes, toolTargets()), "Legato"); }

void MidiEditor::toolsMenu(ui::Menu& menu) {
    menu.addAction("Tools\xe2\x80\xa6", [this] { openTools(); });
    menu.addSeparator();
    menu.addAction("Remove overlaps (same pitch)", [this] { removeOverlaps(); });
    menu.addAction("Legato (all pitches)", [this] { legatoAll(); });
    menu.addAction("Join notes\tCtrl+J", [this] { joinNotes(); });
    menu.addAction(std::string(knife ? "Put the knife away" : "Knife: cut notes") + "\tK", [this] { setKnife(!knife); });
    menu.addAction("Split at the cursor", [this] { splitAtKeyCursor(); });
    menu.addAction("Select the cursor's row\tP", [this] {
        startKeyCursor();
        selectRow(keyPitch, false);
    });
}

ui::Button* MidiEditor::buildToolsButton() {
    toolsButton = ui::toolButton(this, "Tools", [this] { openTools(); },
                                 "Strum, arpeggiate, chop, flam, legato, quantize, humanize or shape the velocities of "
                                 "the selected notes (none selected: all), previewed before Apply (F2); \xe2\x96\xbe "
                                 "for removing overlaps, legato, join and the knife");
    ui::Menu* menu = widgets::ownedMenu(toolsButton);
    menu->aboutToShow = [this](ui::Menu* m) {
        m->clear();
        toolsMenu(*m);
    };
    toolsButton->setMenu(menu, true);  // (MenuButtonPopup)
    return toolsButton;
}
