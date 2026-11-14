// The computer keyboard as a piano (the ⌨ button next to Follow, or Ctrl+K; remembered) (computer_keyboard.py).
//
// While it is on, letter keys play the selected MIDI track as a MIDI keyboard would (midi_input.h: heard
// through its instrument and effects, recorded by ● Rec, kept for Capture):
//
//      W E   T Y U   O P          black keys
//     A S D F G H J K L ; '       white keys, from C
//     Z / X  octave down / up      C / V  velocity down / up
//
// Those keys then play notes instead of doing what they do otherwise (S splits, F follows, L loops…);
// every other key (Space, R, Home, Delete, Ctrl+…) works as always. Keys typed into a text or number
// field, or while a dialog or menu is open, are left alone, and a key held while the program lost the
// focus is let go, so no note sticks.
//
// The toolkit has no application-wide key filter, so the windows that play it pass their events through
// computer_keyboard::filter() first (before shortcuts): the MIDI editor does (MidiEditor::onEvent).
//
// THE MAIN WINDOW:
//   keyboard = std::make_unique<computer_keyboard::ComputerKeyboard>(*midiInput, statusFn);
//   keyboard->showState = [](bool visible, const std::string& text) { …a permanent status-bar label… };
//   (its tooltip: computer_keyboard::LABEL_TIP)
//   keyboard->setOn(computer_keyboard::savedOn(), false); transport keysAction checked to match;
//   keysAction toggled → keyboard->setOn(on);
//   override MainWindow::onEvent(plat::Window* w, const plat::Event& e):
//       if (computer_keyboard::filter(this, e)) return;  ui::Window::onEvent(w, e);
//   closing: keyboard.reset() (lets every key go).
#pragma once
#include <functional>
#include <map>
#include <string>

#include "platform/platform.h"
#include "ui/midi_input.h"

namespace ui { class Window; }

namespace computer_keyboard {

constexpr int MIN_OCTAVE = 0, MAX_OCTAVE = 8;  // A plays C0 (12) … C8 (108); the keys reach 17 semitones higher
constexpr int VELOCITY_STEP = 20;
constexpr int OCTAVE_DOWN = 'Z', OCTAVE_UP = 'X', VELOCITY_DOWN = 'C', VELOCITY_UP = 'V';
extern const char* const LABEL_TIP;

// semitones above the octave's C that `key` (plat key: letters uppercase, ';', '\'') plays, or -1
int stepOf(int key);
// the MIDI note `key` plays at `octave` (C4 = 60), or -1 for a key that plays none
int noteOf(int key, int octave);
bool isControlKey(int key);
std::string noteName(int note);
// is a text or number field taking the keys (in `window`), or a dialog or menu open?
bool typingSomewhere(ui::Window* window);

bool savedOn();  // "options/computer_keyboard"

class ComputerKeyboard {
public:
    ComputerKeyboard(midi_input::MidiInput& input, std::function<void(const std::string&, int)> status);
    ~ComputerKeyboard();  // lets every key go

    void setOn(bool on, bool announce = true);
    bool isOn() const { return on_; }
    int octave = 4;      // A plays C of this octave ("options/keyboard_octave")
    int velocity = 100;  // 1…127 ("options/keyboard_velocity")
    std::string stateText() const;  // "⌨ C4  ·  velocity 100"
    std::function<void(bool visible, const std::string& text)> showState;  // the status bar's label

    // a window's event, before the window handles it: true if it was a note key taken here
    bool filter(ui::Window* window, const plat::Event& e);
    void press(int key);
    void noteOff(int key);
    void releaseAll();
    const std::map<int, int>& held() const { return held_; }  // key -> the note it started

private:
    midi_input::MidiInput& input_;
    std::function<void(const std::string&, int)> status_;
    bool on_ = false;
    std::map<int, int> held_;
    void remember();
    void show();
};

// the keyboard in use (the last one made), or null
ComputerKeyboard* active();
// pass a window's event through the keyboard in use first; true: taken (don't handle it further)
bool filter(ui::Window* window, const plat::Event& e);

}  // namespace computer_keyboard
