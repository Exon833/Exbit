// The effects window of a track or the master, and a MIDI track's instrument patch panel
// (plugin_dialog.py).
#include "ui/plugin_dialog.h"

#include "ui/automation_dialog.h"

#include "audio/modulation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#include "audio/modulation.h"
#include "audio/patch.h"
#include "audio/plugin_audio.h"
#include "core/util.h"
#include "effects/effects.h"
#include "model/track_ops.h"
#include "plugins/library.h"
#include "ui/automation_dialog.h"
#include "ui/midi_learn.h"
#include "ui/plugin_chooser.h"
#include "ui/presets_ui.h"
#include "ui/plugin_host.h"
#include "ui/plugin_pictures.h"
#include "ui/theme.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/menu.h"

using namespace patch_panel;

namespace plugin_dialog {

std::string slotLabel(const PluginSlot& slot) {
    if (!aload(slot.instance)) return slot.name + "  (not loaded)";
    if (slot.failed.load()) return slot.name + "  (stopped after an error: " + slot.error + ")";
    return slot.name;
}

bool touches(const Cable& cable, const PluginSlot* slot) {
    End a = End::of(const_cast<PluginSlot*>(slot)), s = End::sidechain(const_cast<PluginSlot*>(slot));
    return cable.first == a || cable.second == a || cable.first == s || cable.second == s;
}

PanelUnit unitOf(const PluginSlot& slot, bool heard, bool fed, bool sidechain, bool instrument) {
    bool idle = false;
    std::string status, tip;
    auto instance = aload(slot.instance);
    bool stopped = instance && slot.failed.load();
    if (!instance) {
        status = "Not loaded";
        tip = slot.name + " isn't loaded (not installed here any more?). Its settings are kept.";
    } else if (stopped) {
        status = "Stopped";
        tip = slot.name + " stopped after an error: " + slot.error;
    } else if (instrument) {
        if (slot.bypass) {
            status = "Muted";
            tip = slot.name + " is muted. Click the light to hear it again.";
        } else if (!heard) {
            status = "Not wired", idle = true;
            tip = slot.name + " isn't heard: wire its output (right jack) to OUT.";
        } else if (!fed) {
            status = "No notes", idle = true;
            tip = slot.name + " gets no notes: wire IN to its input (left jack).";
        } else {
            status = "On";
            tip = slot.name + ": double-click to open its window. Click the light to mute it; its fader sets its volume.";
        }
    } else if (slot.bypass) {
        status = "Bypassed";
        tip = slot.name + " is bypassed: its input passes through unchanged. Click the light to switch it on.";
    } else if (!heard) {
        status = "Not wired", idle = true;
        tip = slot.name + " isn't heard: wire its output (right jack) to OUT or to another effect that is.";
    } else if (!fed) {
        status = "No input", idle = true;
        tip = "Nothing is wired into " + slot.name + ": wire IN (or another effect's output) to its input (left jack).";
    } else {
        status = "On";
        tip = slot.name + ": double-click to open its window. Click the light to bypass it; its Mix fader blends "
                          "its sound with its input (dry / wet).";
    }
    bool hasSidechain = sidechain || (plugin_audio::usable(&slot) && instance->hasSidechain());
    if (hasSidechain) tip += "\nIt has a sidechain input (SC, next to its input jack).";
    PanelUnit unit;
    unit.name = slot.name;
    if (auto pic = plugin_pictures::picture(slot)) unit.picture = *pic;
    unit.bypass = slot.bypass;
    unit.status = status;
    unit.problem = !instance || stopped;
    unit.idle = idle;
    unit.sidechain = hasSidechain && !instrument;
    unit.tooltip = tip + "\n" + slot.path;
    if (slot.placed) unit.pos = Point((int)std::lround(slot.atX), (int)std::lround(slot.atY));
    if (instrument) unit.volumeDb = slot.volumeDb;
    else unit.mix = slot.mix;
    return unit;
}

std::vector<plug::PluginInfo> choicesOf(const plug::PluginLibrary& library, const std::string& kind) {
    // the built-in effects first (the library doesn't list them), then the plugins of that kind
    std::vector<plug::PluginInfo> out;
    if (kind == plug::EFFECT)
        for (auto& info : effects::list()) out.push_back(plug::PluginInfo{info.path(), info.name, plug::EFFECT, ""});
    for (auto& info : library.ofKind(kind)) {
        if (effects::isBuiltin(info.path)) {
            bool listed = false;
            for (auto& o : out) listed = listed || o.path == info.path;
            if (listed) continue;
        }
        out.push_back(info);
    }
    return out;
}

bool listensTo(const Track& track, const Track* owner) {
    if (!owner) return false;
    std::vector<const Track*> todo{&track};
    std::set<const Track*> seen;
    while (!todo.empty()) {
        const Track* t = todo.back();
        todo.pop_back();
        if (t == owner) return true;
        for (auto& cable : t->cables)
            if (cable.first.type == End::TrackSource && cable.first.track && !seen.count(cable.first.track)) {
                seen.insert(cable.first.track);
                todo.push_back(cable.first.track);
            }
    }
    return false;
}

}  // namespace plugin_dialog

using namespace plugin_dialog;

PluginsDialog::PluginsDialog(ui::Window* parent, PluginHost& host_, const std::string& title, Project& project_,
                             TrackPtr owner_, std::function<void()> onChange_, bool instruments)
    : ui::Dialog(parent, title, 2 * END_W + 3 * (CARD_W + GAP) + GAP + 40,
                 CARD_H + VOLUME_H) ,
      INSTRUMENTS(instruments), host(host_), project(project_), owner(std::move(owner_)), onChange(std::move(onChange_)) {
    if (INSTRUMENTS) {
        KIND = plug::INSTRUMENT, NOUN = "instrument";
        HEADER = "— the notes go from IN to every instrument wired from it; what is wired to OUT is heard, added up, "
                 "on to the track's effects";
        HINT = "Drag from a jack to another to plug in a cable: from IN to an instrument's input (left), and from its "
               "output (right) to OUT. Drag a plug off an input to move or unplug it; right-click a cable to remove it.\n"
               "Each instrument's fader sets its volume (drag it, or turn the mouse wheel over it; double-click: 0 dB); "
               "click its light to mute it. Drag an instrument to move it; double-click it to open its window. "
               "Double-click an empty spot to place an instrument there; right-click for more.";
        BYPASS = "Mute";
    } else {
        KIND = plug::EFFECT, NOUN = "effect";
        HEADER = "— the sound goes from IN, through the cables, to OUT";
        HINT = "Drag from a jack to another to plug in a cable: from IN or an effect's output (right) to an effect's "
               "input (left), its sidechain (SC) or OUT. Drag a plug off an input to move or unplug it; right-click a "
               "cable to remove it; the other tracks' jacks along the bottom go into sidechains.\n"
               "Drag an effect to move it; double-click it to open its window; click its light to bypass it. "
               "Double-click an empty spot to place an effect there; right-click for more.";
        BYPASS = "Bypass";
    }
    setModal(false);  // the main window stays usable: play, stop, edit…
    deleteOnClose = true;
    where = owner ? owner->name : "Master";

    setStyleType("PluginsDialog");
    auto* lay = ui::styleRoom(ui::vbox(this, 10, 6), computedStyle());
    {  // "<b>Effects</b> — …"
        auto row = std::make_unique<ui::HBox>();
        row->spacing = 4;
        ui::styleRoom(row.get(), computedStyle("header"));
        auto* bold = add<ui::Label>(INSTRUMENTS ? "Instruments" : "Effects");
        bold->addStyleClass("heading");  // (default.css: Label.heading, bold)
        auto* rest = add<ui::Label>(HEADER);
        rest->setElide(true);
        row->addWidget(bold);
        row->addWidget(rest, 1);
        lay->addLayout(std::move(row));
    }
    panel = nullptr;
    scroll = add<ui::ScrollArea>();
    panel = scroll->makeContent<PatchPanel>(INSTRUMENTS);  // (the space fills the window; it scrolls when bigger)
    scroll->setSizePolicy(ui::SizePolicy::Expanding, ui::SizePolicy::Expanding);
    lay->addWidget(scroll, 1);
    panel->opened.connect([this](int i) {
        if (i >= 0 && i < (int)effects().size()) openWindow(effects()[i]);
    });
    panel->bypassToggled.connect([this](int i) { toggleBypass(i); });
    panel->unitMoved.connect([this](int i, Point p) { unitMoved(i, p); });
    panel->patched.connect([this](std::optional<PanelCable> r, std::optional<PanelCable> a) { patch(r, a); });
    panel->addRequested.connect([this](std::optional<Point> at) { addEffect(at); });
    panel->removeRequested.connect([this](int) { removeEffect(); });
    panel->menuRequested.connect([this](int i, Point where_) { unitMenu(i, where_); });
    panel->spaceMenuRequested.connect([this](Point pos, Point where_) { spaceMenu(pos, where_); });
    panel->volumeChanged.connect([this](int i, double db) { volumeChanged(i, db); });
    panel->reloadRequested.connect([this](int i) {
        if (i >= 0 && i < (int)effects().size()) reloadUnit(effects()[i]);
    });
    panel->setFocus();  // (the keyboard on the units: Left/Right, Enter, B, C… patch_panel.h)

    {
        auto buttons = std::make_unique<ui::HBox>();
        buttons->spacing = 6;
        ui::styleRoom(buttons.get(), computedStyle("buttons"));
        auto* addB = add<ui::Button>("Add…");
        addB->clicked.connect([this] { addEffect(); });
        auto* removeB = add<ui::Button>("Remove");
        removeB->clicked.connect([this] { removeEffect(); });
        auto* openB = add<ui::Button>("Open window");
        openB->clicked.connect([this] { openWindow(selected()); });
        for (auto* b : {addB, removeB, openB}) buttons->addWidget(b);
        buttons->addStretch(1);
        lay->addLayout(std::move(buttons));
    }
    hint = add<ui::Label>(HINT);
    hint->setProperty("hint", "true");  // the skin's hint colour
    hint->setWordWrap(true);
    lay->addWidget(hint);
    auto* close_ = add<ui::ButtonBox>(ui::ButtonBox::Close);
    close_->rejected.connect([this] { close(); });
    lay->addWidget(close_);
    pictureConn_ = host.pictureTaken.connect([this](SlotPtr slot) {
        for (auto& s : effects())
            if (s->path == slot->path && s->pluginName == slot->pluginName) {
                refresh();
                break;
            }
    });
    changedConn_ = host.slotChanged.connect([this](SlotPtr slot) {  // (stopped, or reloaded)
        if (effects().contains(slot)) refresh(panel->current);
    });
    resizeWindow(2 * END_W + 3 * (CARD_W + GAP) + GAP + 40, panel->cardH * 2 + 260);
    refresh();
    auto alive = alive_;
    ui::App::get().singleShot(0, [this, alive] {
        if (*alive) photographMissing();
    });
}

PluginsDialog::~PluginsDialog() {
    *alive_ = false;
    host.pictureTaken.disconnect(pictureConn_);
    host.slotChanged.disconnect(changedConn_);
}

void PluginsDialog::resized() {
    ui::Dialog::resized();
    if (!hint) return;
    // the hint wrapped at the window's width (a label alone would wrap at 360 px)
    Font f = hint->font();
    const ui::Layout* lay = layout();
    float width = (float)std::max(100, this->width() - (lay ? lay->margins[0] + lay->margins[2] : 20));
    int lines = 0;
    for (const std::string& paragraph : util::split(HINT, '\n')) {
        std::string line;
        ++lines;
        for (const std::string& word : util::splitWords(paragraph)) {
            std::string next = line.empty() ? word : line + " " + word;
            if (!line.empty() && f.width(next) > width) {
                ++lines;
                line = word;
            } else {
                line = next;
            }
        }
    }
    hint->setFixedHeight((int)std::ceil((lines - 1) * f.lineSpacing() + f.ascent() + f.descent()));
}

// --- effects and cables ---

const List<SlotPtr>& PluginsDialog::effects() const {
    if (INSTRUMENTS) return owner->rack;
    return owner ? owner->effects : project.masterEffects;
}

const Cables& PluginsDialog::cables() const {
    if (INSTRUMENTS) return owner->rackCables;
    return owner ? owner->cables : project.masterCables;
}

void PluginsDialog::setPatch(std::optional<std::vector<SlotPtr>> effects_, std::optional<std::vector<Cable>> cables_,
                             std::optional<int> current) {
    auto& e = const_cast<List<SlotPtr>&>(effects());
    auto& c = const_cast<Cables&>(cables());
    if (cables_ && effects_ && effects_->size() < e.size()) c = *cables_;
    if (effects_) e = *effects_;
    if (cables_) c = *cables_;
    if (owner) {  // (modulation cables whose ends went with an effect or instrument: gone too, their knobs back)
        auto keep = modulation::kept(*owner);
        if (keep.size() != owner->modCables.size()) {
            for (auto& m : owner->modCables)
                if (std::find(keep.begin(), keep.end(), m) == keep.end()) modulation::release(m);
            owner->modCables = keep;
        }
    }
    patch::ensureStates(project);
    refresh(current);
    if (onChange) onChange();
}

void PluginsDialog::refresh(std::optional<int> current) {
    const auto& fx = *effects();
    auto cablesNow = cables().get();
    auto plan = patch::makePlan(cablesNow);
    std::set<const PluginSlot*> heard;
    std::map<const PluginSlot*, bool> fedHeard;
    for (auto& node : plan->order) {
        heard.insert(node.slot);
        fedHeard[node.slot] = !node.in.empty();
    }
    std::map<const PluginSlot*, int> index;
    for (int i = 0; i < (int)fx.size(); ++i) index[fx[i].get()] = i;

    others.clear();
    if (!INSTRUMENTS)
        for (auto& t : project.allTracks())
            if (t != owner && t->kind != Kind::Return) others.push_back(t);
    std::map<const Track*, int> otherIndex;
    for (int k = 0; k < (int)others.size(); ++k) otherIndex[others[k].get()] = k;

    auto node = [&](const End& end) -> std::optional<PanelNode> {
        switch (end.type) {
        case End::TrackSource: {
            auto it = otherIndex.find(end.track);
            return it != otherIndex.end() ? std::optional<PanelNode>(PanelNode::track(it->second)) : std::nullopt;
        }
        case End::Sidechain: {
            auto it = index.find(end.slot);
            return it != index.end() ? std::optional<PanelNode>(PanelNode::side(it->second)) : std::nullopt;
        }
        case End::In: return PanelNode::in();
        case End::Out: return PanelNode::out();
        default: {
            auto it = index.find(end.slot);
            return it != index.end() ? std::optional<PanelNode>(PanelNode::unit(it->second)) : std::nullopt;
        }
        }
    };

    // (a plugin not loaded here keeps the sidechain jack its cables are plugged into)
    std::vector<PanelUnit> units;
    for (auto& s : fx) {
        bool fed = false, side = false;
        for (auto& c : *cablesNow) {
            if (c.second == End::of(s.get())) fed = true;
            if (c.second == End::sidechain(s.get())) side = true;
        }
        if (heard.count(s.get())) fed = fedHeard[s.get()];
        units.push_back(unitOf(*s, heard.count(s.get()) > 0, fed, side, INSTRUMENTS));
        if (host.canReload(s)) {
            units.back().reload = true;
            units.back().tooltip += "\nClick Reload to start it again, with the settings it last had.";
        }
    }
    bool nonePlaced = std::all_of(units.begin(), units.end(), [](const PanelUnit& u) { return !u.pos; });
    if (nonePlaced)  // (none placed yet, e.g. from an older project: in a row, as they were wired)
        for (int i = 0; i < (int)units.size(); ++i) units[i].pos = Point(END_W + GAP + i * (CARD_W + GAP), panel->top());
    std::vector<PanelCable> wires;
    for (auto& c : *cablesNow) {
        auto a = node(c.first), b = node(c.second);
        if (a && b) wires.push_back({*a, *b});
    }
    // modulators: their own look and status, and a line to each unit whose knob they move
    std::vector<PatchPanel::ModLink> links;
    for (size_t i = 0; i < fx.size(); i++) {
        if (!modulation::isModulator(fx[i].get())) continue;
        PanelUnit& u = units[i];
        u.modulator = true;
        u.mix.reset();  // (no sound: no Mix fader)
        if (u.problem) continue;
        int moves = 0;
        if (modulates())
            for (auto& m : owner->modCables)
                if (m.from == fx[i].get()) {
                    moves++;
                    auto to = index.find(m.to);
                    char depth[32];
                    std::snprintf(depth, sizeof depth, " %+d %%", (int)std::lround(m.depth * 100));
                    std::string label = m.param + depth;
                    if (to != index.end()) links.push_back({(int)i, to->second, label});
                }
        bool listens = fx[i]->path == "builtin:mod_follower";
        u.idle = moves == 0;
        u.status = moves ? "Moves " + std::to_string(moves) + (moves == 1 ? " knob" : " knobs") : "Moves nothing";
        u.tooltip = fx[i]->name + " is a modulator: it moves other units' knobs (the dashed lines), not the sound. "
                    "Right-click it → Modulate a knob… to choose one; double-click it for its settings." +
                    std::string(listens ? " It follows the loudness of what is wired into its input (or its "
                                          "sidechain: another track's sound)." : "");
        if (!modulates()) u.tooltip += " (Modulation works in a track's effects window.)";
    }
    std::vector<std::pair<std::string, bool>> sources;
    for (auto& t : others) sources.push_back({t->name, listensTo(*t, owner.get())});
    panel->setUnits(std::move(units), std::move(wires), current, std::move(sources));
    panel->setModLinks(std::move(links));
}

SlotPtr PluginsDialog::selected() const {
    int row = panel->current;
    return row >= 0 && row < (int)effects().size() ? effects()[row] : nullptr;
}

void PluginsDialog::openWindow(const SlotPtr& slot) {
    if (slot) host.edit(slot, where);
}

End PluginsDialog::endOf(const PanelNode& node) const {
    switch (node.type) {
    case PanelNode::Track: return End::source(others[node.index].get());
    case PanelNode::Side: return End::sidechain(effects()[node.index].get());
    case PanelNode::In: return End::in();
    case PanelNode::Out: return End::out();
    default: return End::of(effects()[node.index].get());
    }
}

void PluginsDialog::patch(const std::optional<PanelCable>& remove, const std::optional<PanelCable>& add_) {
    std::vector<Cable> list = cables().copy();
    if (remove) {
        Cable old(endOf(remove->first), endOf(remove->second));
        list.erase(std::remove(list.begin(), list.end(), old), list.end());
    }
    if (add_) {
        Cable fresh(endOf(add_->first), endOf(add_->second));
        if (std::find(list.begin(), list.end(), fresh) == list.end()) list.push_back(fresh);
    }
    setPatch(std::nullopt, list, panel->current);
}

void PluginsDialog::keepPlaces() {
    const auto& fx = *effects();
    for (size_t i = 0; i < fx.size() && i < panel->units().size(); ++i)
        if (panel->units()[i].pos) {
            fx[i]->placed = true;
            fx[i]->atX = panel->units()[i].pos->x;
            fx[i]->atY = panel->units()[i].pos->y;
        }
}

void PluginsDialog::unitMoved(int row, Point pos) {
    keepPlaces();
    if (row >= 0 && row < (int)effects().size()) {
        effects()[row]->placed = true;
        effects()[row]->atX = pos.x;
        effects()[row]->atY = pos.y;
    }
    refresh(row);
    if (onChange) onChange();
}

void PluginsDialog::addEffect(std::optional<Point> at) {
    auto choices = plugin_dialog::choicesOf(host.library(), KIND);
    if (choices.empty()) {
        ui::msg::information(this, "No " + NOUN + "s", "No VST " + NOUN + "s were found on this computer.");
        return;
    }
    // (search, favourites: plugin_chooser.h)
    std::optional<plug::PluginInfo> chosen = plugin_chooser::choosePlugin(this, choices, NOUN);
    if (!chosen) return;
    auto alive = alive_;
    SlotPtr slot = host.load(*chosen);
    if (!*alive) {
        if (slot) project.retire(slot->instance);
        return;
    }
    // (the keys work on it next: wiring it, its window; once the chooser has gone, which gives the focus back)
    if (slot && place(slot, at))
        ui::App::get().singleShot(0, [this, alive] {
            if (*alive) panel->setFocus();
        });
}

bool PluginsDialog::place(const SlotPtr& slot, std::optional<Point> at) {
    if (slot->kind != KIND) {
        ui::msg::information(this, "Not an " + NOUN, wrongKind(*slot));
        project.retire(aload(slot->instance));
        return false;
    }
    keepPlaces();
    Point spot = at ? *at : panel->freeSpot(panel->units());
    slot->placed = true;
    slot->atX = spot.x;
    slot->atY = spot.y;
    std::vector<SlotPtr> fx = effects().copy();
    int current = (int)fx.size();
    fx.push_back(slot);
    setPatch(fx, std::nullopt, current);
    if (alsoOn && !INSTRUMENTS && alsoOn(slot) > 0)  // (several tracks selected: heard on each at once)
        setPatch(std::nullopt, track_ops::wiredAtEnd(*cables(), slot.get()), current);
    panel->ensureVisible(panel->unitRect(panel->current));
    photographMissing();
    return true;
}

std::string PluginsDialog::wrongKind(const PluginSlot& slot) const {
    if (INSTRUMENTS) return slot.name + " is an effect. Add it with the track's FX button instead.";
    return slot.name + " is an instrument. Choose it with a MIDI track's instrument button (🎹) instead.";
}

void PluginsDialog::volumeChanged(int row, double value) {
    // a unit's fader moved: an instrument's volume (instrument patch panels) or an effect's Mix, heard at once
    if (row < 0 || row >= (int)effects().size()) return;
    if (INSTRUMENTS) effects()[row]->volumeDb = value;
    else effects()[row]->mix = value;
}

void PluginsDialog::photographMissing() {
    // a picture of each effect that has none yet (its window opened invisibly for a moment)
    std::vector<SlotPtr> missing;
    for (auto& s : effects())
        if (plugin_audio::usable(s) && !plugin_pictures::hasPicture(*s)) missing.push_back(s);
    if (missing.empty()) return;
    auto alive = alive_;
    setCursor(ui::Cursor::Wait);
    for (auto& slot : missing) {
        if (!*alive) return;
        if (isWindowShown() && effects().contains(slot) && !plugin_pictures::hasPicture(*slot)) host.photograph(slot);
    }
    if (!*alive) return;
    setCursor(ui::Cursor::Arrow);
    if (isWindowShown()) raiseWindow();  // (back from the invisible plugin window)
}

void PluginsDialog::removeEffect() {
    SlotPtr slot = selected();
    if (!slot) return;
    host.closeEditor({slot});
    std::vector<SlotPtr> fx;
    for (auto& s : effects())
        if (s != slot) fx.push_back(s);
    std::vector<Cable> list;
    for (auto& c : cables())
        if (!touches(c, slot.get())) list.push_back(c);
    setPatch(fx, list);
    project.retire(aload(slot->instance));  // (destroyed on this thread, once the audio thread let go of it)
}

void PluginsDialog::unplug(const SlotPtr& slot) {
    std::vector<Cable> list;
    for (auto& c : cables())
        if (!touches(c, slot.get())) list.push_back(c);
    setPatch(std::nullopt, list, panel->current);
}

void PluginsDialog::toggleBypass(int row) {
    if (row < 0 || row >= (int)effects().size()) return;
    SlotPtr slot = effects()[row];
    slot->bypass = !slot->bypass;
    refresh(row);
    if (onChange) onChange();
}

void PluginsDialog::reloadUnit(const SlotPtr& slot) {
    // (after the click or the menu that asked has returned: reloading shows a "please wait" window)
    auto alive = alive_;
    ui::App::get().singleShot(0, [this, alive, slot] {
        if (!*alive || !effects().contains(slot)) return;
        host.reloadStopped(slot);
        if (*alive) refresh(panel->current);
    });
}

void PluginsDialog::modulate(const SlotPtr& from) {
    if (!modulates() || !from) return;
    SlotPtr target;
    std::string param;
    {
        ParameterDialog d(this, owner, from.get());
        if (!d.exec() || !d.chosenSlot) return;
        target = d.chosenSlot;
        param = d.chosenParam;
    }
    std::vector<ModCable> list = owner->modCables.copy();
    for (auto& m : list)
        if (m.from == from.get() && m.to == target.get() && m.param == param) return;  // (already)
    ModCable c;
    c.from = from.get();
    c.to = target.get();
    c.param = param;
    c.depth = 0.5;
    list.push_back(c);
    setModCables(list);
    modulation::resolve(*owner);
}

void PluginsDialog::setModCables(std::vector<ModCable> cables) {
    if (!modulates()) return;
    for (auto& m : owner->modCables)
        if (std::find(cables.begin(), cables.end(), m) == cables.end()) {
            bool still = false;  // (the same cable with another depth: its knob stays modulated)
            for (auto& n : cables)
                if (n.state == m.state) still = true;
            if (!still) modulation::release(m);
        }
    owner->modCables = cables;
    refresh(panel->current);
    if (onChange) onChange();
}

void PluginsDialog::editDepth(const ModCable& cable) {
    std::string text = std::to_string((int)std::lround(cable.depth * 100));
    if (!ui::getText(this, "How far it moves " + cable.param,
                          "Depth in % (-100 … 100; negative: the other way). At 100 % the modulator's full swing\n"
                          "moves the knob over its whole range.",
                          text))
        return;
    char* end = nullptr;
    double pct = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) return;
    std::vector<ModCable> list = owner->modCables.copy();
    for (auto& m : list)
        if (m.state == cable.state) m.depth = std::max(-1.0, std::min(1.0, pct / 100));
    setModCables(list);
}

void PluginsDialog::fillModulatorMenu(ui::Menu& menu, const SlotPtr& slot) {
    menu.addAction("Modulate a knob…", [this, slot] { modulate(slot); })->tooltip =
        "Choose a knob of this track's effects or instrument for it to move";
    for (auto& m : owner->modCables) {
        if (m.from != slot.get()) continue;
        std::string target = m.to ? m.to->name : "?";
        char depth[32];
        std::snprintf(depth, sizeof depth, "  (%+d %%)", (int)std::lround(m.depth * 100));
        ui::Menu* sub = menu.addMenu("→ " + target + ": " + m.param + depth);
        ModCable cable = m;
        sub->addAction("Depth…", [this, cable] { editDepth(cable); });
        sub->addAction("The other way", [this, cable] {
            std::vector<ModCable> list = owner->modCables.copy();
            for (auto& x : list)
                if (x.state == cable.state) x.depth = -x.depth;
            setModCables(list);
        });
        sub->addAction("Remove", [this, cable] {
            std::vector<ModCable> list;
            for (auto& x : owner->modCables)
                if (x.state != cable.state) list.push_back(x);
            setModCables(list);
        });
    }
    menu.addSeparator();
}

void PluginsDialog::fillUnitMenu(ui::Menu& menu, int row) {
    SlotPtr slot = effects()[row];
    if (modulates() && modulation::isModulator(slot.get())) fillModulatorMenu(menu, slot);
    if (host.canReload(slot)) {
        menu.addAction("Reload (it stopped)", [this, slot] { reloadUnit(slot); });
        menu.addSeparator();
    }
    menu.addAction("Open its window", [this, slot] { openWindow(slot); })->setEnabled(plugin_audio::usable(slot));
    menu.addCheckable(BYPASS, slot->bypass, [this, row](bool) { toggleBypass(row); });
    auto alive = alive_;
    menu.addAction("Take a new picture", [this, slot, alive] {
            if (host.photograph(slot) && *alive) raiseWindow();
        })->setEnabled(plugin_audio::usable(slot));
    // its presets and compare, and MIDI learn (presets_ui.h, midi_learn.h): for plugins too, whose own
    // window is all the plugin's
    presets_ui::fillEffectMenu(*menu.addMenu("Presets"), this, slot);
    if (midi_learn::active()) {
        TrackPtr track = owner;
        ui::Action* link = menu.addAction("Link a knob to a MIDI controller\xe2\x80\xa6", [this, track, slot] {
            midi_learn::chooseAndLearn(this, track, slot);
        });
        link->tooltip = "Choose one of its parameters, then turn a knob on your MIDI controller to move it";
        link->setEnabled(plugin_audio::usable(slot));
    }
    menu.addSeparator();
    bool any = false;
    for (auto& c : cables())
        if (touches(c, slot.get())) any = true;
    menu.addAction("Unplug its cables", [this, slot] { unplug(slot); })->setEnabled(any);
    menu.addAction("Remove", [this] { removeEffect(); });
}

void PluginsDialog::unitMenu(int row, Point where_) {
    if (row < 0 || row >= (int)effects().size()) return;
    ui::Menu menu;
    fillUnitMenu(menu, row);
    menu.exec(where_, panel);
}

void PluginsDialog::fillSpaceMenu(ui::Menu& menu, Point pos) {
    if (INSTRUMENTS) {
        menu.addAction("Place an instrument here…", [this, pos] { addEffect(panel->spotAt(pos)); });
        menu.addSeparator();
        menu.addAction("Wire every instrument in (IN → each → OUT)", [this] { wireAll(); })
            ->setEnabled(!effects().empty());
        menu.addAction("Unplug every cable", [this] { setPatch(std::nullopt, std::vector<Cable>{}, panel->current); })
            ->setEnabled(!cables().empty());
        return;
    }
    menu.addAction("Place an effect here…", [this, pos] { addEffect(panel->spotAt(pos)); });
    menu.addSeparator();
    menu.addAction("Wire every effect in a row (left to right)", [this] { wireInARow(); })->setEnabled(!effects().empty());
    menu.addAction("Unplug every cable (IN straight to OUT)", [this] { setPatch(std::nullopt, straight(), panel->current); })
        ->setEnabled(*cables() != straight());
}

void PluginsDialog::spaceMenu(Point pos, Point where_) {
    ui::Menu menu;
    fillSpaceMenu(menu, pos);
    menu.exec(where_, panel);
}

void PluginsDialog::wireInARow() {
    // replace every cable: IN → the effects from left to right (top to bottom where level) → OUT
    keepPlaces();
    std::vector<SlotPtr> ordered = effects().copy();
    std::stable_sort(ordered.begin(), ordered.end(), [](const SlotPtr& a, const SlotPtr& b) {
        return a->atX != b->atX ? a->atX < b->atX : a->atY < b->atY;
    });
    setPatch(std::nullopt, chain(ordered), panel->current);
}

void PluginsDialog::wireAll() {
    // every instrument gets the notes and is heard (the cables already there stay)
    std::vector<Cable> list = cables().copy();
    for (auto& slot : effects())
        for (const Cable& c : {Cable(End::in(), End::of(slot.get())), Cable(End::of(slot.get()), End::out())})
            if (std::find(list.begin(), list.end(), c) == list.end()) list.push_back(c);
    setPatch(std::nullopt, list, panel->current);
}
