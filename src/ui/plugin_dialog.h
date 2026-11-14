// The effects window of a track (FX button on its row) or of the master: its patch space
// (patch_panel.h), with a picture of each plugin (plugin_pictures.h). Effects are placed in it and
// wired by hand (model.h, audio/patch.h): a newly chosen effect is only placed, so it isn't heard
// until it is wired in. Changes apply immediately, while playing too. (A MIDI track's instrument is
// chosen with the instrument button on its row: instrument_menu.h.) plugin_dialog.py.
//
// InstrumentPanelDialog is the same window for a MIDI track's instrument patch panel (several
// instruments on one track, each with its own volume): its notes go from IN to the instruments wired
// from it, and those wired to OUT are heard, added up, on to the track's effects.
//
// It is an ordinary window, not a modal one: the rest of the program keeps working while it is
// open (play, stop, edit, open other tracks' windows). The main window keeps one per track. It can
// be made bigger both ways; the patch space scrolls when it is bigger than the window.
// Effects without a picture yet are photographed when the window opens (invisibly, about a second
// each; then never again, unless their window is opened or "Take a new picture" is chosen).
//
// THE MAIN WINDOW: `auto* w = new PluginsDialog(this, host, "Effects: " + name, project, track,
// onChange)` (track null: the master's, "Effects: Master (outputs 1/2)"); deleteOnClose is set, so
// forget it on `finished`; show it with w->open() (modeless), raiseWindow(). After tracks changed:
// close those whose `owner` is gone / frozen (or, for `instruments`, not using its rack any more),
// refresh() the others.
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "model/model.h"
#include "plugins/library.h"
#include "ui/patch_panel.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

class PluginHost;

namespace plugin_dialog {
std::string slotLabel(const PluginSlot& slot);
// is the cable plugged into this effect (its input, sidechain or output)?
bool touches(const Cable& cable, const PluginSlot* slot);
// how an effect (or an instrument of an instrument patch panel) shows on the patch panel. heard:
// its output reaches OUT; fed: something is wired into it
PanelUnit unitOf(const PluginSlot& slot, bool heard, bool fed, bool sidechain = false, bool instrument = false);
// Does `track` listen (through sidechains, maybe via other tracks) to `owner`? Then wiring it into
// owner's patch would make a loop. (Nothing listens to the master: owner null.)
bool listensTo(const Track& track, const Track* owner);
// what Add… offers: the built-in effects first (effects::list(), for effects), then the library's
// plugins of that kind (and those whose kind is not known yet)
std::vector<plug::PluginInfo> choicesOf(const plug::PluginLibrary& library, const std::string& kind);
}  // namespace plugin_dialog

class PluginsDialog : public ui::Dialog {
public:
    // owner: the track (its effects and cables), or null: the master (project.masterEffects / masterCables).
    // onChange() is called after every change.
    PluginsDialog(ui::Window* parent, PluginHost& host, const std::string& title, Project& project, TrackPtr owner,
                  std::function<void()> onChange, bool instruments = false);
    ~PluginsDialog() override;

    const bool INSTRUMENTS;          // an instrument patch panel (InstrumentPanelDialog)
    PluginHost& host;
    Project& project;
    TrackPtr owner;                  // null: the master
    std::function<void()> onChange;
    std::string where;               // for the plugin windows' titles: the track's name, or "Master"
    std::vector<TrackPtr> others;    // the other tracks, as the panel lists them (sidechain sources)
    PatchPanel* panel;
    ui::ScrollArea* scroll;
    ui::Label* hint = nullptr;
    void resized() override;         // (the hint's height for the window's width)

    // what it holds: owner's effects / the master's / the rack's (instrument panels)
    const List<SlotPtr>& effects() const;
    const Cables& cables() const;
    // new effects and/or cables (new lists: the audio thread sees old or new, never half). Cables go
    // first when effects are removed, last when they are added: the audio thread never finds a cable
    // to an effect that is gone.
    void setPatch(std::optional<std::vector<SlotPtr>> effects, std::optional<std::vector<Cable>> cables,
                  std::optional<int> current = {});
    void refresh(std::optional<int> current = {});
    SlotPtr selected() const;
    void openWindow(const SlotPtr& slot);
    End endOf(const PanelNode& node) const;
    // a cable plugged in, moved or unplugged on the panel
    void patch(const std::optional<PanelCable>& remove, const std::optional<PanelCable>& add);
    void keepPlaces();               // store where every unit is, so they stay there
    void unitMoved(int row, Point pos);
    // choose an effect (an instrument, in an instrument patch panel) and place it (at `at`, or on a free spot); not wired
    void addEffect(std::optional<Point> at = {});
    // place an already loaded slot (what addEffect does after choosing and loading); false if of the wrong kind
    bool place(const SlotPtr& slot, std::optional<Point> at = {});
    std::string wrongKind(const PluginSlot& slot) const;
    // a unit's fader moved: an instrument's volume (dB) or an effect's Mix (0…1)
    void volumeChanged(int row, double value);
    void photographMissing();        // a picture of each effect that has none yet
    void removeEffect();
    void unplug(const SlotPtr& slot);
    void toggleBypass(int row);
    void unitMenu(int row, Point where);
    void spaceMenu(Point pos, Point where);
    void wireInARow();               // IN → the effects from left to right (top to bottom where level) → OUT
    void wireAll();                  // instrument panels: every instrument gets the notes and is heard
    // the menus' entries (for the menus and tests): filled into `menu`
    void fillUnitMenu(ui::Menu& menu, int row);
    void fillSpaceMenu(ui::Menu& menu, Point pos);

    std::string KIND, NOUN, HEADER, HINT, BYPASS;

private:
    int pictureConn_ = -1;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

// A MIDI track's instrument patch panel (owner: the track, instruments in `rack`, cables in
// `rackCables`): instruments placed side by side, each wired from IN (the notes) and to OUT, each
// with its own volume.
class InstrumentPanelDialog : public PluginsDialog {
public:
    InstrumentPanelDialog(ui::Window* parent, PluginHost& host, const std::string& title, Project& project,
                          TrackPtr track, std::function<void()> onChange)
        : PluginsDialog(parent, host, title, project, std::move(track), std::move(onChange), true) {}
};
