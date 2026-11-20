// PADS ON THEIR OWN TRACKS: any pad of a Pad Kit (model.h PAD KIT) can play on a track of its own, with its own
// effects, fader, pan, sends and meter (as Ableton Live's Drum Rack chains do; FL Studio's FPC can't). No GUI.
//
// The kit's track holds where each pad goes (Track::padOut: a track per pad, null: the kit's own sound). A PAD
// TRACK is an ordinary audio track without clips: its input is that pad's sound, before its effects, as clips
// would be. "Pads to tracks" (padsToTracks) gives every used pad without one a track of its own, named after its
// sound ("Kick (pad 1)"), placed below the kit, and puts them with the kit in a group named after the kit (its bus:
// the kit's whole sound stays one group). Setting a pad back to the kit, or removing its track, sends it back.
//
// THE SOUND. A clip on the kit is rendered with its routed pads kept apart (padkit::render's `apart`: in its
// data's AudioBuffer::pads, each as long as the kit's; only the routed ones are kept, so a kit with two pads routed
// holds two extra sounds, not 32); live notes play on the kit as ever, the mixer handing a routed pad's voices to
// its track. The mixer (audio/mixer.h PADS ON THEIR OWN TRACKS) works the kit out before its pad tracks (they wait
// for it, as a sidechain's listener does), so choke groups still cut across them, everything stays bit-identical
// with or without worker threads, and delay compensation lines the pad tracks up like any track. A pad whose
// track is gone (or that a stale rendering kept apart) plays in the kit's own sound again.
//
// Freezing a kit freezes its own sound (its pad tracks keep playing their pads); a pad track freezes and bounces to
// a new track like any track (its pad's sound through its effects). "Bounce in place" is refused for both (a pad
// track's pad would still play into it; a kit's routed pads would be lost).
//
// Saved by track index ("pad_tracks": [[pad, track], …] on the kit), only when set; references to tracks that
// are gone, to returns, to the kit itself or to a track another kit already plays into are cleaned on loading
// and when a track is removed (forgetGone).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "model/model.h"

namespace pad_tracks {

// pad's track (null: the kit's own sound)
TrackPtr trackOf(const Track& kit, int pad);
// the pads with a track of their own, as bits (bit i: pad i); 0 when the track doesn't play its Pad Kit
uint32_t routedMask(const Track& kit);
// the kit whose pads t plays (null: none), and those pads (in order)
TrackPtr kitOf(const Project& p, const Track& t, std::vector<int>* pads = nullptr);
// "Kick (pad 1)": the name of a pad's track (its sound's name; "Pad 1" when it has none)
std::string trackName(const Pad& pad, int index);
// "Kit pad 3": what plays into a pad track (its menu's "Input: …", its tooltip); "" for any other track
std::string inputName(const Project& p, const Track& t);
// pad's sound on `to` (null: back in the kit's own sound). The kit's clips must then be rendered again.
void setTrack(const TrackPtr& kit, int pad, const TrackPtr& to);
// every pad routed to t back in its kit (t a pad track); the kits changed
std::vector<TrackPtr> backToKit(const Project& p, const Track& t);
// the group the kit's pad tracks go in: the kit's own (named as the kit) if it is in one, else one made for it
GroupPtr kitGroup(Project& p, const TrackPtr& kit);
// pad on a new audio track of its own (below the kit and its other pad tracks, in kitGroup); the pad's track if
// it has one already. The kit's clips must then be rendered again.
TrackPtr giveTrack(Project& p, const TrackPtr& kit, int pad);
// "Pads to tracks": giveTrack for every used pad without a track; the tracks made
std::vector<TrackPtr> padsToTracks(Project& p, const TrackPtr& kit);
// references to tracks gone, returns, buses, the kit itself, or a track another kit plays into: cleared; how many
int forgetGone(Project& p);

}  // namespace pad_tracks
