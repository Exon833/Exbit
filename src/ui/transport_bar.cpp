// The top bar (transport_bar.py).
#include "ui/transport_bar.h"

#include "audio/midi_effects.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

#include "config.h"
#include "core/util.h"
#include "gfx/svg.h"
#include "platform/platform.h"
#include "ui/commands.h"
#include "ui/formatting.h"
#include "ui/theme.h"
#include "ui/toolkit/style.h"
#include "ui/track_dialogs.h"
#include "ui/widgets.h"

// ============================================================================================
// TransportAction (QAction)
// ============================================================================================

TransportAction::TransportAction(ui::Button* button_, const std::string& text_) : button(button_), text(text_) {
    button->toggled.connect([this](bool on) { toggled.emit(on); });
    button->clicked.connect([this] { triggered.emit(); });
}

TransportAction::~TransportAction() = default;

void TransportAction::setText(const std::string& t) {
    text = t;
    if (iconText.empty() && !dynamic_cast<IconButton*>(button)) button->setText(t);
}

void TransportAction::setIconText(const std::string& t) {
    iconText = t;
    if (!dynamic_cast<IconButton*>(button)) button->setText(t.empty() ? text : t);
}

void TransportAction::setShortcut(const std::string& keys, ui::ShortcutScope scope) {
    seq_ = ui::KeySequence::parse(keys);
    scope_ = scope;
    shortcut_ = std::make_unique<ui::Shortcut>(shortcutOwner ? shortcutOwner : button, seq_, [this] { trigger(); }, scope_);
    shortcut_->active = [this] { return button->isEnabled(); };
}

void TransportAction::setShortcutContext(ui::ShortcutScope scope) {
    scope_ = scope;
    if (shortcut_) shortcut_->scope = scope;
}

void TransportAction::trigger() {
    if (!button->isEnabled()) return;
    if (button->isCheckable()) button->setChecked(!button->isChecked());  // (toggled)
    triggered.emit();
}

KeyAction::KeyAction(ui::Widget* owner, const std::string& keys, ui::ShortcutScope scope) {
    seq_ = ui::KeySequence::parse(keys);
    shortcut_ = std::make_unique<ui::Shortcut>(owner, seq_, [this] { trigger(); }, scope);
}

// ============================================================================================
// icons
// ============================================================================================

namespace transport_bar {

std::string iconFolder;

std::string iconDir() { return iconFolder.empty() ? plat::dataDir() + "/icons" : iconFolder; }

int timeW() { return std::max(0, theme::metric("transport-time-width", TIME_W)); }

const Image& tinted(const std::string& name, Color color, int px) {
    // (icon, colour, px) -> image, or an empty one if its file is missing
    static std::map<std::tuple<std::string, uint32_t, int>, Image> cache;
    static std::map<std::string, std::string> texts;  // file contents ("" = missing)
    auto key = std::make_tuple(name, color.premul(), px);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    auto t = texts.find(name);
    if (t == texts.end()) {
        std::string svg;
        if (!util::readFile(iconDir() + "/" + name + ".svg", svg)) svg.clear();
        t = texts.emplace(name, svg).first;
    }
    Image img = t->second.empty() ? Image() : renderSvgTinted(t->second, px, px, color);
    return cache.emplace(key, img).first->second;
}

}  // namespace transport_bar

IconButton::IconButton(const std::string& icon, const std::string& glyph) : icon_(icon), glyph_(glyph) {
    setStyle(ui::Button::Style::Tool);
    setStyleType("IconButton");
    int px = theme::metric("transport-button-size", config::TRANSPORT_BUTTON_PX);
    setFixedSize(px, px);
}

void IconButton::setIcon(const std::string& icon, const std::string& glyph) {
    icon_ = icon;
    glyph_ = glyph;
    update();
}

void IconButton::paint(Painter& p) {
    ui::Button::paint(p);  // the skin's button only (it has no text): the icon is drawn below
    // (IconButton::icon in its states: :checked, :disabled…; it repaints only when they change)
    Color color = ui::style::foreground(computedStyle("icon"));
    int px = std::max(1, theme::metric("transport-icon-size", config::TRANSPORT_ICON_PX));
    const Image& img = transport_bar::tinted(icon_, color, px);
    if (!img.empty()) {
        p.drawImage(Point((width() - px) / 2, (height() - px) / 2), img);
    } else {
        Font f = font();
        f = f.withPixelSize(f.pixelSize() * 1.7f);
        p.drawText(RectF(0, 0, (float)width(), (float)height()), AlignCenter, glyph_, f, color);
    }
}

// ============================================================================================
// TransportBar
// ============================================================================================

namespace {
// InstantPopup without the toolkit's menu indicator (the stylesheet hides it: the text has its ▾)
void openMenuOnClick(ui::Button* b, ui::Menu* menu) {
    b->clicked.connect([b, menu] {
        Point g = b->mapToGlobal(Point(0, 0));
        menu->execBelow(Rect(g.x, g.y, b->width(), b->height()), b);
    });
}

// a plain upright line between groups of controls (the stylesheet's QFrame#separator)
ui::Widget* separator(ui::Widget* parent) {
    auto* line = parent->add<ui::Separator>(ui::Orientation::Vertical);
    line->objectName = "separator";
    line->setFixedWidth(theme::metric("transport-separator-width", 7));  // (the line is 1 px: the margins take the rest)
    return line;
}

ui::HBox* partLayout(ui::Widget* holder, int spacing) {
    ui::HBox* row = ui::hbox(holder, 0, spacing);
    return row;
}

std::string g(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}
}  // namespace

TransportBar::TransportBar() {
    objectName = "Transport";
    setStyleType("Transport");
    left_ = add<ui::Widget>();
    middle_ = add<ui::Widget>();
    right_ = add<ui::Widget>();
    int spacing = std::max(0, ui::style::px(computedStyle(), "column-gap", 2));  // (between its controls)
    ui::HBox* left = partLayout(left_, spacing);
    partLayout(middle_, spacing);
    ui::HBox* right = partLayout(right_, spacing);

    // (each action's text is for menus, its icon text for its button)
    addFileAction = button(left_, "Add file…", "Add file");
    addFileAction->setToolTip("Add audio or MIDI files (Ctrl+I)");
    addFileAction->setShortcut("Ctrl+I");
    // (the track actions keep their shortcuts and commands, but their buttons are never shown: tracks are added from
    // the track panel's right-click menu, LanePanel::fillAddMenu)
    addAudioTrackAction = button(left_, "Audio track", "+ Audio");
    addAudioTrackAction->setShortcut("Ctrl+T");
    addAudioTrackAction->setToolTip("Add an audio track (Ctrl+T)");
    addMidiTrackAction = button(left_, "MIDI track", "+ MIDI");
    addMidiTrackAction->setShortcut("Ctrl+Shift+T");
    addMidiTrackAction->setToolTip("Add a MIDI track (Ctrl+Shift+T)");
    addReturnTrackAction = button(left_, "Return track", "+ Return");
    addReturnTrackAction->setShortcut("Ctrl+Alt+T");
    addReturnTrackAction->setToolTip("Add a return track (Ctrl+Alt+T): effects (a reverb, a delay) the other tracks "
                                     "share, each sending it as much as you like (→ on their rows)");
    for (auto* a : {addAudioTrackAction.get(), addMidiTrackAction.get(), addReturnTrackAction.get()}) a->button->hide();
    keyButton = menuButton(left_, "Key \xe2\x96\xbe", &keyMenu,
                           "The song's key and scale (a MIDI clip can have its own): the MIDI editor shades the notes "
                           "outside it and can snap to it. Detect suggests keys from the notes");
    keyMenu->aboutToShow = [this](ui::Menu* m) {
        m->clear();
        if (fillKeyMenu) fillKeyMenu(*m);
    };
    keyButton->show();
    swingButton = menuButton(left_, "Swing \xe2\x96\xbe", &swingMenu,
                             "Swing for the whole song: the off-beat 8ths or 16ths of every MIDI clip come later "
                             "(0 %: straight). A track, or a Steps row, can have its own");
    swingMenu->aboutToShow = [this](ui::Menu* m) {
        m->clear();
        if (fillSwingMenu) fillSwingMenu(*m);
    };
    swingButton->show();
    syncButton = menuButton(left_, "Sync â¾", &syncMenu,
                            "What the transport follows: Internal (its own), MIDI clock (an input with Clock in, Options "
                            "â MIDI portsâ¦) or JACK's transport (both ways)");
    syncMenu->aboutToShow = [this](ui::Menu* m) {
        m->clear();
        if (fillSyncMenu) fillSyncMenu(*m);
    };
    syncButton->show();
    left->addStretch(1);

    playAction = iconButton("Play", "play", "▶");
    playAction->setShortcut("Space");
    playAction->setToolTip("Play (Space), from the selection cursor");
    recordAction = iconButton("Rec", "record", "●");
    recordAction->setShortcut("R");
    recordAction->setToolTip("Record (R) onto the selected audio track, from the selection cursor; again to "
                             "stop. Right-click for the input and a count-in");
    recordButton = static_cast<IconButton*>(recordAction->button);
    recordButton->setProperty("toggle", "record");  // red while recording
    startAction = iconButton("Start", "return_to_start", "⏮");
    startAction->setShortcut("Home");
    startAction->setToolTip("Back to the start (Home): both cursors to 0 s");
    loopAction = iconButton("Loop", "loop", "⟲");
    loopAction->setCheckable(true);
    loopAction->setShortcut("L");
    loopAction->setToolTip("Loop playback (L): from the loop range's end back to its start. Turning it on "
                           "with a time range selected loops that range; drag the ends of the band on the "
                           "ruler to change it");
    for (auto* action : {playAction.get(), startAction.get()})  // also from the plugins (FX) windows
        action->setShortcutContext(ui::ShortcutScope::ApplicationWide);
    followAction = iconButton("Follow", "follow", "⇥");
    followAction->setCheckable(true);
    followAction->setCheckedSilently(true);
    followAction->setShortcut("F");
    followAction->setToolTip("Keep the playhead in view during playback (F)");
    keysAction = iconButton("Computer keyboard", "keyboard", "⌨");
    keysAction->setCheckable(true);
    keysAction->setShortcut("Ctrl+K", ui::ShortcutScope::ApplicationWide);  // (from the MIDI editor too)
    keysAction->setToolTip("Play the selected MIDI track (or every track whose keyboard focus toggle is lit) on the "
                           "computer keyboard (Ctrl+K): A S D F G H J K L are white keys from C, W E T Y U O P black keys; Z / X octave down / up, C / V velocity down / up. "
                           "While on, those letters play notes instead of their shortcuts");
    captureAction = iconButton("Capture", "capture", "⤓");
    captureAction->setShortcut("Ctrl+Alt+R", ui::ShortcutScope::ApplicationWide);
    captureAction->setToolTip("Capture (Ctrl+Alt+R): turn what you just played on a MIDI keyboard or the computer "
                              "keyboard into a clip on the selected MIDI track, even though you weren't recording");
    playButton = middleButtons[0];

    right->addStretch(1);
    timeLabel = right_->add<ui::Label>();
    timeLabel->objectName = "time";  // the skin's numbers font
    timeLabel->setMinimumWidth(transport_bar::timeW());
    timeLabel->setAlignment(AlignCenter);
    right->addWidget(timeLabel);
    timeSeparator = separator(right_);
    right->addWidget(timeSeparator);
    buildTimingControls();
    moreButton = menuButton(right_, "»", &moreMenu, "More: the controls there's no room for on the bar");
    moreMenu->aboutToShow = [this](ui::Menu* m) { fillMoreMenu(*m); };
    buildFitting();
}

TransportBar::~TransportBar() = default;

std::unique_ptr<TransportAction> TransportBar::button(ui::Widget* part, const std::string& text, const std::string& shortText) {
    auto* b = part->add<ui::Button>(shortText.empty() ? text : shortText);
    b->setStyle(ui::Button::Style::Tool);
    b->setProperty("autoRaise", "true");
    b->setFocusPolicy(ui::FocusPolicy::None);
    static_cast<ui::HBox*>(part->layout())->addWidget(b);
    auto action = std::make_unique<TransportAction>(b, text);
    action->iconText = shortText;
    action->shortcutOwner = this;  // (its shortcut works while the button is hidden too)
    return action;
}

ui::Button* TransportBar::menuButton(ui::Widget* part, const std::string& text, ui::Menu** menu, const std::string& tip) {
    // a toolbar-style button opening a menu; hidden until fit shows it
    auto* b = part->add<ui::Button>(text);
    b->setStyle(ui::Button::Style::Tool);
    b->setProperty("autoRaise", "true");
    b->setFocusPolicy(ui::FocusPolicy::None);
    b->setToolTip(tip);
    *menu = widgets::ownedMenu(b);
    openMenuOnClick(b, *menu);
    b->hide();
    static_cast<ui::HBox*>(part->layout())->addWidget(b);
    return b;
}

std::unique_ptr<TransportAction> TransportBar::iconButton(const std::string& text, const std::string& icon,
                                                          const std::string& glyph) {
    auto* b = middle_->add<IconButton>(icon, glyph);
    b->setProperty("autoRaise", "true");
    b->setFocusPolicy(ui::FocusPolicy::None);
    static_cast<ui::HBox*>(middle_->layout())->addWidget(b);
    middleButtons.push_back(b);
    auto action = std::make_unique<TransportAction>(b, text);
    action->shortcutOwner = this;  // (its shortcut works while the button is hidden too)
    return action;
}

void TransportBar::buildTimingControls() {
    auto* right = static_cast<ui::HBox*>(right_->layout());
    bpmBox = right_->add<ui::SpinBox>();
    bpmBox->setRange(config::MIN_BPM, config::MAX_BPM);
    bpmBox->setDecimals(2);
    bpmBox->setSuffix(" BPM");
    bpmBox->setKeyboardTracking(false);  // apply typed tempos on Enter, not at every digit
    bpmBox->setToolTip("Tempo in beats (quarter notes) per minute");
    right->addWidget(bpmBox);
    // Tap counts on the button's press (the moment of the tap), not its click; T presses it too
    tapButton = right_->add<ui::Button>("Tap");
    tapButton->setStyle(ui::Button::Style::Tool);
    tapButton->setProperty("autoRaise", "true");
    tapButton->setFocusPolicy(ui::FocusPolicy::None);
    tapButton->setToolTip("Tap tempo (T): click in time with the music, at least twice; the BPM follows "
                          "the average of your last " + std::to_string(config::TAP_COUNT) + " taps. A pause of " +
                          g(config::TAP_RESET_S) + " s starts over");
    tapShortcut_ = std::make_unique<ui::Shortcut>(this, ui::KeySequence::parse("T"), [this] {
        tapButton->pressed.emit();  // (a tap counts when pressed)
        tapButton->released.emit();
        tapButton->clicked.emit();
    });
    right->addWidget(tapButton);
    metronomeAction = button(right_, "Metronome", "Metronome");
    metronomeButton = metronomeAction->button;
    metronomeAction->setCheckable(true);
    metronomeAction->setShortcut("Ctrl+M");
    metronomeAction->setToolTip("Metronome on/off while playing (Ctrl+M). Right-click for its sounds");
    metronomeButton->setProperty("accent", "true");  // lit in the skin's accent colour when on
    signatureBox = right_->add<ui::SpinBox>();
    signatureBox->setRange(1, config::MAX_BEATS_PER_BAR);
    signatureBox->setSuffix(" / 4");
    signatureBox->setKeyboardTracking(false);
    signatureBox->setToolTip("Time signature: beats per bar");
    right->addWidget(signatureBox);
    for (ui::SpinBox* box : {bpmBox, signatureBox}) {  // only take the keyboard when clicked
        box->setFocusPolicy(ui::FocusPolicy::Click);
        if (box->editor()) box->editor()->setFocusPolicy(ui::FocusPolicy::Click);
    }
    beatsAction = button(right_, "♩ Beats", "♩ Beats");
    beatsAction->setCheckable(true);
    beatsButton = beatsAction->button;
    beatsAction->setShortcut("B");
    beatsAction->setToolTip("Edit in bars and beats (B): bar ruler and grid, clicks snap to the grid "
                            "(hold Alt to place freely). Off: minutes and seconds");
    thirdsAction = button(right_, "⅓ Thirds", "⅓ Thirds");
    thirdsAction->setCheckable(true);
    thirdsButton = thirdsAction->button;
    thirdsAction->setShortcut("3");
    thirdsAction->setToolTip("Divide the grid in thirds: triplets (3)");
    // the grid: fixed or following the zoom, and snapping (its menu is filled from the timing shown last)
    gridButton = right_->add<ui::Button>("Grid auto ▾");
    gridButton->setStyle(ui::Button::Style::Tool);
    gridButton->setProperty("autoRaise", "true");
    gridButton->setFocusPolicy(ui::FocusPolicy::None);
    gridButton->setToolTip("The grid clicks and drags snap to: fixed (1/16, 1/8…) whatever the zoom, or following the "
                           "zoom; and snapping on or off.\nCtrl+1 / Ctrl+2: finer / coarser, Ctrl+5: fixed or following "
                           "the zoom, Ctrl+4: snapping on / off (Alt does the opposite for one drag)");
    gridMenu = widgets::ownedMenu(gridButton);
    openMenuOnClick(gridButton, gridMenu);
    gridMenu->aboutToShow = [this](ui::Menu* m) {
        m->clear();
        if (timing_) transport_bar::fillGridMenu(*m, *timing_, [this](const GridChange& c) { if (onGridChange) onGridChange(c); });
    };
    right->addWidget(gridButton);
    for (auto [name, keys] : {std::pair<const char*, const char*>{"finer", "Ctrl+1"}, {"coarser", "Ctrl+2"},
                              {"snap", "Ctrl+4"}, {"fixed", "Ctrl+5"}})  // (from the MIDI editor too)
        gridActions[name] = std::make_unique<KeyAction>(this, keys, ui::ShortcutScope::ApplicationWide);
}

// --- fitting the bar to its width ----------------------------------------------------------

namespace {
// the narrowest a layout makes `w` (as Qt's qSmartMinSize does)
int minWidth(const ui::Widget* w) {
    if (w->minimumSize().x > 0) return std::min(w->minimumSize().x, w->maximumSize().x);
    int hint = w->minimumSizeHint().x;
    if (w->hPolicy == ui::SizePolicy::Fixed) hint = std::max(hint, w->sizeHint().x);
    return std::max(0, std::min(hint, w->maximumSize().x));
}
}  // namespace

void TransportBar::buildFitting() {
    // The steps fit takes, in order, while the bar is short of room. The first LOSSLESS keep every
    // control on the bar; each later one hides a control (the least needed first) into the » menu.
    auto hide = [](std::vector<ui::Widget*> widgets) {
        return [widgets](bool on) {
            for (auto* w : widgets) w->setVisible(!on);
        };
    };
    auto compact = [this](bool on) {
        compactTime = on;
        sizeTimeLabel();
    };
    auto shortLabel = [this](bool on) {
        beatsAction->setIconText(on ? "♩" : "♩ Beats");
        thirdsAction->setIconText(on ? "⅓" : "⅓ Thirds");
        metronomeAction->setIconText(on ? "Metro" : "Metronome");
        shortLabels = on;
        showGridName();
    };
    // middle buttons: play, record, start, loop, follow, keys, capture
    auto icon = [this](int i) { return static_cast<ui::Widget*>(middleButtons[i]); };
    steps_ = {compact, shortLabel,
              hide({icon(6)}), hide({syncButton}), hide({swingButton}), hide({keyButton}), hide({icon(5)}), hide({icon(4)}),
              hide({thirdsButton}), hide({gridButton}), hide({beatsButton}),
              hide({signatureBox}), hide({tapButton}), hide({metronomeButton}),
              hide({timeLabel, timeSeparator}), hide({icon(3)}),
              hide({icon(2)}), hide({addFileAction->button}), hide({bpmBox}),
              hide({icon(1)}), hide({icon(0)})};
}

void TransportBar::apply(int lvl) {
    for (int i = 0; i < (int)steps_.size(); ++i) steps_[i](i < lvl);
    moreButton->setVisible(lvl > LOSSLESS);
}

int TransportBar::needed() const {
    int total = 0;
    for (ui::Widget* part : {left_, middle_, right_}) {
        int count = 0, sum = 0;
        for (auto& child : part->children()) {
            ui::Widget* w = child.get();
            if (!w->isVisible()) continue;
            sum += minWidth(w);
            ++count;
        }
        int spacing = part->layout() ? part->layout()->spacing : 0;
        total += sum + spacing * std::max(0, count - 1);
    }
    return total;
}

std::vector<int> TransportBar::measure(int width) {
    apply(0);
    std::vector<int> needs{needed()};
    for (int lvl = 1; lvl <= (int)steps_.size(); ++lvl) {
        if (lvl > LOSSLESS && needs.back() <= width) break;
        steps_[lvl - 1](true);
        moreButton->setVisible(lvl > LOSSLESS);
        needs.push_back(needed());
    }
    return needs;
}

void TransportBar::fit() {
    if (fitting_ || steps_.empty()) return;
    fitting_ = true;
    int width = std::max(0, this->width() - frame().w());  // (inside the toolbar's border and padding)
    std::vector<int> needs = measure(width);
    int found = (int)needs.size() - 1;
    for (int i = 0; i < (int)needs.size(); ++i)
        if (needs[i] <= width) {
            found = i;
            break;
        }
    level = found;
    apply(level);
    fitting_ = false;
    if ((int)needs.size() > LOSSLESS && needs[LOSSLESS] != narrowest_) {
        narrowest_ = needs[LOSSLESS];
        updateGeometry();  // the window's narrowest changes with it
    }
}

int TransportBar::narrowestWidth() {
    if (narrowest_ < 0 && !fitting_ && !steps_.empty()) {
        fitting_ = true;
        narrowest_ = measure()[LOSSLESS];
        apply(level);
        fitting_ = false;
    }
    return std::max(0, narrowest_);
}

Point TransportBar::minimumSizeHint() const {
    int w = const_cast<TransportBar*>(this)->narrowestWidth();
    return Point(w + frame().w(), sizeHint().y);
}

void TransportBar::sizeTimeLabel() {
    // TIME_W, or (short of room) enough for a few minutes or bars
    int width = transport_bar::timeW();
    if (compactTime) {
        std::string sample = beatsShown ? "88.8.8   8:88.888" : "88:88.888";
        width = (int)std::ceil(timeLabel->font().width(sample)) + 12;
    }
    timeLabel->setMinimumWidth(std::min(width, transport_bar::timeW()));
}

void TransportBar::showGridName() {
    // "Grid 1/16 ▾" (just "1/16 ▾" with the short labels), or "No snap ▾"
    if (gridName.empty()) gridButton->setText("No snap ▾");
    else gridButton->setText(shortLabels ? gridName + " ▾" : "Grid " + gridName + " ▾");
}

void TransportBar::fillMoreMenu(ui::Menu& menu) {
    menu.clear();
    auto addAction = [&menu](TransportAction* a) {
        ui::Action* item = menu.addAction(a->text, [a] { a->trigger(); }, a->shortcut().text());
        if (a->isCheckable()) {
            item->checkable = true;
            item->checked = a->isChecked();
        }
        item->setEnabled(a->isEnabled());
    };
    auto rightClick = [](ui::Button* b) {
        return [b] {
            ui::MouseEvent e;
            e.button = plat::RightButton;
            b->rightClicked.emit(e);  // (its right-click: the main window opens the settings)
        };
    };
    if (!addFileAction->button->isVisible()) {
        addAction(addFileAction.get());
        menu.addSeparator();
    }
    TransportAction* middle[] = {playAction.get(), recordAction.get(), startAction.get(), loopAction.get(),
                                 followAction.get(), keysAction.get(), captureAction.get()};
    for (auto* a : middle) {
        if (a->button->isVisible()) continue;
        addAction(a);
        if (a == recordAction.get()) menu.addAction("Recording settings…", rightClick(recordButton));
    }
    if (!bpmBox->isVisible()) {
        menu.addSeparator();
        char buf[48];
        std::snprintf(buf, sizeof buf, "%.2f BPM", bpmBox->value());
        menu.addAction(std::string("Tempo: ") + buf + "…", [this] { askTempo(); });
    }
    if (!tapButton->isVisible()) menu.addAction("Tap tempo", [this] { tapButton->pressed.emit(); }, "T");
    if (!metronomeButton->isVisible()) {
        addAction(metronomeAction.get());
        menu.addAction("Metronome sounds…", rightClick(metronomeButton));
    }
    if (!signatureBox->isVisible())
        menu.addAction("Time signature: " + std::to_string(signatureBox->intValue()) + " / 4…", [this] { askSignature(); });
    if (!beatsButton->isVisible()) {
        menu.addSeparator();
        addAction(beatsAction.get());
    }
    if (!thirdsButton->isVisible()) addAction(thirdsAction.get());
    for (auto [b, title, fill] : {std::make_tuple(keyButton, "Key", &fillKeyMenu),
                                  std::make_tuple(swingButton, "Swing", &fillSwingMenu),
                                  std::make_tuple(syncButton, "Sync", &fillSyncMenu)})
        if (!b->isVisible() && *fill) {
            if (title == std::string("Key")) menu.addSeparator();
            std::string text = b->text();  // ("Key: C major ▾": its name, without the ▾)
            if (text.size() > 4 && text.compare(text.size() - 4, 4, " \xe2\x96\xbe") == 0) text.resize(text.size() - 4);
            (*fill)(*menu.addMenu(text));
        }
    if (!gridButton->isVisible() && timing_) {
        ui::Menu* grid = menu.addMenu(gridName.empty() ? "Grid: no snap" : "Grid: " + gridName);
        transport_bar::fillGridMenu(*grid, *timing_, [this](const GridChange& c) { if (onGridChange) onGridChange(c); });
        if (!menu.actions().empty()) menu.actions().back()->setEnabled(gridButton->isEnabled());
    }
}

void TransportBar::askTempo() {
    double bpm = bpmBox->value();
    if (track_dialogs::getDouble(window(), "Tempo", "Beats (quarter notes) per minute:", bpm, bpmBox->minimum(),
                                 bpmBox->maximum(), 2))
        bpmBox->setValue(bpm);
}

void TransportBar::askSignature() {
    int beats = signatureBox->intValue();
    if (track_dialogs::getInt(window(), "Time signature", "Beats (quarter notes) per bar:", beats,
                              (int)signatureBox->minimum(), (int)signatureBox->maximum()))
        signatureBox->setValue(beats);
}

namespace transport_bar {

void fillGridMenu(ui::Menu& menu, const Timing& timing, const std::function<void(const GridChange&)>& choose) {
    static int groupIds = 1000;
    int group = ++groupIds;
    auto fixed = timing.fixedStep();  // (the step in use, thirds counted)
    ui::Action* zoom = menu.addAction("Follows the zoom", [choose] { choose({GridChange::Grid, 0.0, true}); }, "Ctrl+5");
    zoom->checkable = true;
    zoom->checked = !fixed;
    zoom->group = group;
    Timing probe = timing;
    for (const Frac& step : timing::fixedSteps(timing.thirds)) {
        probe.grid = step.value();
        double value = step.value();
        ui::Action* a = menu.addAction(probe.gridName(), [choose, value] { choose({GridChange::Grid, value, true}); });
        a->checkable = true;
        a->checked = fixed && probe.fixedStep() == fixed;
        a->group = group;
    }
    menu.addSeparator();
    bool on = timing.snapOn;
    ui::Action* snap = menu.addAction("Snap to the grid", [choose, on] { choose({GridChange::Snap, 0.0, !on}); }, "Ctrl+4");
    snap->checkable = true;
    snap->checked = on;
    menu.addAction("Finer", [choose] { choose({GridChange::Finer, 0.0, true}); }, "Ctrl+1");
    menu.addAction("Coarser", [choose] { choose({GridChange::Coarser, 0.0, true}); }, "Ctrl+2");
    menu.addSeparator();
    bool follows = timing.automationFollows;
    ui::Action* follow = menu.addAction("Automation follows clips", [choose, follows] {
        choose({GridChange::Follow, 0.0, !follows});
    });
    follow->checkable = true;
    follow->checked = follows;
}

}  // namespace transport_bar

void TransportBar::registerCommands(commands::Registry& r) {
    // (ids: English, never translated — keymaps keep them)
    std::pair<TransportAction*, const char*> actions[] = {
        {addFileAction.get(), "Add file…"},        {addAudioTrackAction.get(), "Add an audio track"},
        {addMidiTrackAction.get(), "Add a MIDI track"}, {addReturnTrackAction.get(), "Add a return track"},
        {playAction.get(), "Play"},                {recordAction.get(), "Record"},
        {startAction.get(), "Back to the start"},  {loopAction.get(), "Loop"},
        {followAction.get(), "Follow the playhead"}, {keysAction.get(), "Computer keyboard"},
        {captureAction.get(), "Capture"},          {metronomeAction.get(), "Metronome"},
        {beatsAction.get(), "Bars and beats"},     {thirdsAction.get(), "Triplets (thirds)"},
    };
    for (auto& [a, name] : actions) {
        if (!a) continue;
        commands::KeyCommand c;
        c.id = std::string("Transport/") + name;
        c.name = name;
        if (!a->shortcut().empty()) c.defaults.push_back(a->shortcut());
        c.run = [a] { a->trigger(); };
        c.active = [a] { return a->isEnabled(); };
        c.owner = this;  // (as its own shortcut: it works while the button is hidden too)
        c.scope = a->shortcutScope();
        c.shown = [a](const ui::KeySequence& k) { a->showShortcut(k); };
        r.add(std::move(c));
    }
    std::pair<const char*, const char*> grid[] = {{"finer", "Grid/Finer"}, {"coarser", "Grid/Coarser"},
                                                  {"snap", "Grid/Snap on or off"}, {"fixed", "Grid/Fixed or following the zoom"}};
    for (auto& [key, id] : grid) {
        auto it = gridActions.find(key);
        if (it == gridActions.end()) continue;
        KeyAction* a = it->second.get();
        commands::KeyCommand c;
        c.id = id;
        c.name = std::string(id).substr(5);
        c.defaults = {a->shortcut()};
        c.run = [a] { a->trigger(); };
        c.owner = this;
        c.scope = ui::ShortcutScope::ApplicationWide;  // (from the MIDI editor too)
        c.shown = [a](const ui::KeySequence& k) { a->showShortcut(k); };
        r.add(std::move(c));
    }
    commands::KeyCommand tap;
    tap.id = "Transport/Tap tempo";
    tap.name = "Tap tempo";
    tap.defaults = {ui::KeySequence::parse("T")};
    tap.run = [this] {
        tapButton->pressed.emit();  // (a tap counts when pressed)
        tapButton->released.emit();
        tapButton->clicked.emit();
    };
    tap.owner = this;
    tap.shown = [this](const ui::KeySequence&) { tapShortcut_.reset(); };
    r.add(std::move(tap));
}

void TransportBar::showTiming(const Timing& timing) {
    timing_ = &timing;
    bpmBox->setValueSilently(timing.bpm);
    signatureBox->setValueSilently(timing.beatsPerBar);
    beatsAction->setCheckedSilently(timing.beats);
    thirdsAction->setCheckedSilently(timing.thirds);
    loopAction->setCheckedSilently(timing.looping);
    thirdsAction->setEnabled(timing.beats);
    gridButton->setEnabled(timing.beats);
    std::string name = "auto";
    if (timing.fixedStep()) {
        name = timing.gridName();
        size_t at = name.find(" triplets");
        if (at != std::string::npos) name.replace(at, 9, "T");
    }
    gridName = timing.snapOn ? name : "";
    showGridName();
    std::string scale = timing.scale.empty() ? std::string()
                        : std::string(midi_effects::KEYS[((timing.key % 12) + 12) % 12]) + " " +
                              (timing.scale == "Major" ? "major" : timing.scale == "Minor" ? "minor" : timing.scale);
    keyButton->setText(scale.empty() ? "Key \xe2\x96\xbe" : "Key: " + scale + " \xe2\x96\xbe");
    char swing[48];
    std::snprintf(swing, sizeof swing, "Swing %g %%%s \xe2\x96\xbe", std::round(timing.swing * 10) / 10,
                  timing.swingGrid ? "" : " 8ths");
    swingButton->setText(timing.swing > 0 ? swing : "Swing \xe2\x96\xbe");
    if (timing.beats != beatsShown) {
        beatsShown = timing.beats;
        sizeTimeLabel();
    }
    fit();  // (the grid's name and the time display may need more or less room now)
}

void TransportBar::setRecording(bool on) {
    recordAction->setCheckable(true);
    recordAction->setCheckedSilently(on);
    recordAction->setCheckable(on);  // (not checkable otherwise: a click only asks to record)
}

void TransportBar::setPlaying(bool playing) {
    playAction->setText(playing ? "Pause" : "Play");
    playAction->setToolTip(playing ? "Pause (Space)" : "Play (Space), from the selection cursor");
    if (playing) playButton->setIcon("pause", "⏸");
    else playButton->setIcon("play", "▶");
}

void TransportBar::setTime(double seconds, const Timing& timing) {
    std::string text = fmt::time(seconds);
    timeLabel->setText(timing.beats ? fmt::bars(seconds, timing) + "   " + text : text);
}

// --- the toolbar ---------------------------------------------------------------------------

// its border and padding together (default.css: padding 2px, the line under it a 1 px border)
ui::style::Insets TransportBar::frame() const {
    const css::Style& st = computedStyle();
    ui::style::Insets b = ui::style::insets(st, "border-width"), p = ui::style::insets(st, "padding");
    return {b.left + p.left, b.top + p.top, b.right + p.right, b.bottom + p.bottom};
}

Point TransportBar::sizeHint() const {
    int h = 0, w = 0;
    for (ui::Widget* part : {left_, middle_, right_}) {
        Point s = part->layout() ? part->layout()->sizeHint() : part->sizeHint();
        h = std::max(h, s.y);
        w += s.x;
    }
    ui::style::Insets f = frame();
    return Point(w + f.w(), h + f.h());
}

void TransportBar::resized() {
    fit();
    arrange();
}

void TransportBar::arrange() {
    // the left and right parts are equally wide, so the middle is centred; when they don't fit
    // beside it that way, each keeps its own width and the middle goes between them
    ui::style::Insets f = frame();
    const int PAD = f.left, TOP = f.top;
    int W = width() - f.w(), H = height() - f.h();
    auto hint = [](ui::Widget* part) { return part->layout() ? part->layout()->sizeHint().x : 0; };
    int lw = hint(left_), mw = hint(middle_), rw = hint(right_);
    int side = (W - mw) / 2;
    int lx, mx, rx, lwid, rwid;
    if (lw <= side && rw <= side) {
        lwid = side;
        rwid = W - mw - side;
        lx = PAD;
        mx = PAD + side;
        rx = mx + mw;
    } else {
        int spare = std::max(0, W - lw - mw - rw);
        lwid = lw + spare / 2;
        rwid = rw + spare - spare / 2;
        // keep the middle as close to the centre as the sides allow
        int want = PAD + side;
        mx = std::max(PAD + lw, std::min(want, PAD + W - rw - mw));
        lwid = mx - PAD;
        rwid = std::max(0, PAD + W - (mx + mw));
        lx = PAD;
        rx = mx + mw;
    }
    left_->setGeometry(lx, TOP, std::max(0, lwid), H);
    middle_->setGeometry(mx, TOP, mw, H);
    right_->setGeometry(rx, TOP, std::max(0, rwid), H);
}

void TransportBar::paint(Painter& p) {
    ui::style::box(p, rect(), computedStyle());  // (its background, and the line under it: its border)
}
