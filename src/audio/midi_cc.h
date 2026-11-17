// The latest value of every MIDI controller played in (the mod wheel, the other control changes, the
// bend wheel), for whoever wants to follow one: the "MIDI Control" modulator (effects/modulators.cpp)
// reads it on the audio thread, and a "link to a controller" can watch which one moved last.
//
// A small lock-free table: one atomic per controller, written by the MIDI input path (ui/midi_input,
// on the GUI thread, as each event arrives, before the sustain pedal is applied: the pedal's own
// control 64 is noted too) and read from any thread. Channels are not told apart (as midi_input).
//
// Entries 0..127 are the control numbers (value 0..127, read as 0…1; 0 until one is played), entry
// PITCH_BEND (128) is the bend wheel (-8192..8191, read as 0…1 with its centre at 0.5, or -1…1 by bend()).
#pragma once
#include <cstdint>

namespace midi_cc {

constexpr int MOD_WHEEL = 1;
constexpr int PITCH_BEND = 128;  // the table's entry for the bend wheel (after the 128 control numbers)
constexpr int COUNT = 129;

// --- written by the MIDI input (any thread) ---
void control(int number, int value);  // a control change: number 0..127, value 0..127
void pitchBend(int value);            // the bend wheel: -8192..8191 (0: centre)

// --- read (any thread, the audio thread too: no lock, no allocation) ---
double value(int controller);  // 0…1 (PITCH_BEND: 0.5 at the centre); 0 for a controller out of range
double bend();                 // the bend wheel: -1…1 (0 at the centre)
int raw(int controller);       // as it came: 0..127, or -8192..8191 for PITCH_BEND
// the controller that moved last (-1: none yet) and how many moves there have been: a "link to a
// controller" waits for moves() to change, then takes last()
int last();
uint32_t moves();

void reset();  // every controller back at rest (tests)

}  // namespace midi_cc
