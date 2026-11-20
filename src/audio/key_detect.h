// Finding the key and scale of some notes (the transport's Key ▾ → "Detect", a MIDI clip's Scale menu).
//
// The Krumhansl–Schmuckler method: the notes' pitch classes, each counted by how long it sounds, are
// compared with a profile of every key (12 roots) of every scale of the MIDI effects' SCALES; the
// correlation of the two (Pearson's r, -1…1) is the key's score. Major and Minor use Temperley's profiles
// (how often each pitch class sounds in a key, from the Kostka–Payne corpus: "Music and Probability",
// 2007; the minor one turned natural minor), which tell C major from E minor far better than
// Krumhansl–Kessler's listener ratings; the other scales a profile made the same way from their notes
// (the tonic and fifth strongest, then the third, then the other notes of the scale, the rest least). The
// share of the notes' time outside a scale is taken off its score, so a pentatonic scale can't win when the
// major scale's other notes are heard too. Natural major and minor are preferred: they get a small bonus
// (PREFERRED), so on a tie (C major's notes, which are also D Dorian's) they come first. No GUI; any thread.
#pragma once
#include <string>
#include <vector>

#include "audio/synth.h"

namespace key_detect {

struct Candidate {
    int key = 0;        // 0 = C … 11 = B
    std::string scale;  // a SCALES name
    double score = 0;   // the correlation (less the share outside, with the bonus): higher is better
};

constexpr double PREFERRED = 0.04;  // added to Major and Minor's scores

// every key of every scale, best first; empty when the notes have no length (nothing to tell)
std::vector<Candidate> rank(const std::vector<Note>& notes);
// the best `count` of them
std::vector<Candidate> detect(const std::vector<Note>& notes, int count = 5);
// "A minor", "D Dorian"
std::string name(const Candidate& c);

}  // namespace key_detect
