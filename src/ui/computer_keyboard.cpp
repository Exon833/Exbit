// The computer keyboard as a piano (computer_keyboard.py).
#include "ui/computer_keyboard.h"

#include <algorithm>

#include "core/json.h"
#include "core/util.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/widget.h"

namespace computer_keyboard {

const char* const LABEL_TIP = "The computer keyboard plays the selected MIDI track (Ctrl+K: off). "
                              "Z / X: octave down / up, C / V: velocity down / up";

namespace {
ComputerKeyboard* gActive = nullptr;
}

int stepOf(int key) {
    static const std::map<int, int> steps = {
        {'A', 0}, {'S', 2}, {'D', 4}, {'F', 5}, {'G', 7}, {'H', 9}, {'J', 11}, {'K', 12}, {'L', 14}, {';', 16},
        {'\'', 17}, {'W', 1}, {'E', 3}, {'T', 6}, {'Y', 8}, {'U', 10}, {'O', 13}, {'P', 15}};
    auto it = steps.find(key);
    return it == steps.end() ? -1 : it->second;
}

int noteOf(int key, int octave) {
    int step = stepOf(key);
    return step < 0 ? -1 : 12 * (octave + 1) + step;
}

bool isControlKey(int key) {
    return key == OCTAVE_DOWN || key == OCTAVE_UP || key == VELOCITY_DOWN || key == VELOCITY_UP;
}

std::vector<int> keysInUse() {
    std::vector<int> keys;
    if (!gActive || !gActive->isOn()) return keys;
    for (int key = ' '; key <= '~'; key++)
        if (stepOf(key) >= 0 || isControlKey(key)) keys.push_back(key);
    return keys;
}

std::string noteName(int note) {
    static const char* const NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(NAMES[((note % 12) + 12) % 12]) + std::to_string(note / 12 - 1);
}

bool typingSomewhere(ui::Window* window) {
    ui::App& app = ui::App::get();
    if (app.modalWindow() || !app.popups().empty()) return true;
    ui::Widget* focus = window ? window->focusWidget() : nullptr;
    for (ui::Widget* w = focus; w; w = w->parent()) {  // (a spin box's or combo box's editor is inside it)
        if (dynamic_cast<ui::LineEdit*>(w) || dynamic_cast<ui::SpinBox*>(w)) return true;
        if (auto* box = dynamic_cast<ui::ComboBox*>(w))
            if (box->lineEdit()) return true;  // (an editable one)
    }
    return false;
}

bool savedOn() { return settings::get("options/computer_keyboard").toBool(false); }

ComputerKeyboard::ComputerKeyboard(midi_input::MidiInput& input, std::function<void(const std::string&, int)> status)
    : input_(input), status_(std::move(status)) {
    octave = std::min(MAX_OCTAVE, std::max(MIN_OCTAVE, settings::get("options/keyboard_octave").toInt(4)));
    velocity = std::min(127, std::max(1, settings::get("options/keyboard_velocity").toInt(100)));
    gActive = this;
}

ComputerKeyboard::~ComputerKeyboard() {
    releaseAll();
    if (gActive == this) gActive = nullptr;
}

void ComputerKeyboard::setOn(bool on, bool announce) {
    if (on == on_) {
        show();
        return;
    }
    on_ = on;
    if (!on) releaseAll();
    settings::set("options/computer_keyboard", Json(on));
    show();
    if (announce && status_)
        status_(on ? "Computer keyboard: plays the selected MIDI track (A S D F\xe2\x80\xa6 white keys, W E T Y U\xe2\x80\xa6 "
                     "black keys, Z / X octave, C / V velocity)"
                   : "Computer keyboard: off",
                5000);
}

std::string ComputerKeyboard::stateText() const {
    return "\xe2\x8c\xa8 " + noteName(12 * (octave + 1)) + "  \xc2\xb7  velocity " + std::to_string(velocity);
}

void ComputerKeyboard::show() {
    if (showState) showState(on_, stateText());
}

void ComputerKeyboard::remember() {
    settings::set("options/keyboard_octave", Json(octave));
    settings::set("options/keyboard_velocity", Json(velocity));
    show();
}

bool ComputerKeyboard::filter(ui::Window* window, const plat::Event& e) {
    if (e.type == plat::Event::FocusOut) {  // (its key releases would go to another program: no note may stick)
        ui::App::get().singleShot(0, [] {
            if (gActive && !ui::App::get().activeWindow()) gActive->releaseAll();
        });
        return false;
    }
    if (e.type != plat::Event::KeyDown && e.type != plat::Event::KeyUp) return false;
    int key = e.key >= 'a' && e.key <= 'z' ? e.key - 'a' + 'A' : e.key;
    if (e.type == plat::Event::KeyUp) {  // (a held key is let go wherever the keys went meanwhile)
        if (held_.count(key)) {
            if (!e.autoRepeat) noteOff(key);
            return true;
        }
        return false;
    }
    if (!on_ || (stepOf(key) < 0 && !isControlKey(key)) || typingSomewhere(window)) return false;
    if (e.mods & (plat::ModCtrl | plat::ModAlt | plat::ModMeta | plat::ModShift)) return false;  // (Ctrl+S… stay)
    if (!e.autoRepeat) press(key);
    return true;
}

void ComputerKeyboard::press(int key) {
    if (isControlKey(key)) {
        if (key == OCTAVE_DOWN || key == OCTAVE_UP) {
            octave = std::min(MAX_OCTAVE, std::max(MIN_OCTAVE, octave + (key == OCTAVE_UP ? 1 : -1)));
        } else {
            int step = key == VELOCITY_UP ? VELOCITY_STEP : -VELOCITY_STEP;
            velocity = std::min(127, std::max(1, velocity > 1 || step < 0 ? velocity + step : step));
        }
        remember();
        return;
    }
    if (held_.count(key)) noteOff(key);
    int note = noteOf(key, octave);
    if (note < 0 || note > 127) return;
    held_[key] = note;
    input_.handle(midi_in::Event::noteOn(note, velocity, 0, input_.now()));
}

void ComputerKeyboard::noteOff(int key) {
    auto it = held_.find(key);
    if (it == held_.end()) return;
    int note = it->second;
    held_.erase(it);
    for (auto& kv : held_)
        if (kv.second == note) return;  // (another key still holds the same note)
    input_.handle(midi_in::Event::noteOff(note, 0, input_.now()));
}

void ComputerKeyboard::releaseAll() {
    std::vector<int> keys;
    for (auto& kv : held_) keys.push_back(kv.first);
    for (int k : keys) noteOff(k);
}

ComputerKeyboard* active() { return gActive; }

bool filter(ui::Window* window, const plat::Event& e) { return gActive && gActive->filter(window, e); }

}  // namespace computer_keyboard
