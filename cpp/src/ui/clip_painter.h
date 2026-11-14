// Drawing one clip: its box, its contents (waveform or MIDI notes) and its name (clip_painter.py).
//
// The timeline often repaints only a strip (playhead moves, scrolling), so everything here draws
// only what falls inside `visible` and gives identical pixels whether the clip is painted whole or
// strip by strip. The contents come as ready-made images from waveform_cache.h.
#pragma once
#include <functional>
#include <map>
#include <string>

#include "gfx/painter.h"
#include "model/model.h"
#include "ui/selection.h"

namespace clip_painter {

constexpr int FADE_HANDLE_PX = 7;  // the square at each top corner of an audio clip's body: drag it to fade in or out
inline Color fadeShade() { return Color(0, 0, 0, 70); }  // over the part of the clip a fade makes quieter

// the pixels a QRectF covers when filled without antialiasing (pixel centres inside it)
Rect pixels(const RectF& r);

// Box, darker handle strip on top (drag to move), contents below (select / cut), name. Colours come
// from the clip's colour (or its kind's default) with automatic contrast. A frozen track's clips get
// an icy tint (they are locked). A disabled clip is only its outline and name, in its colour. An audio
// clip shows its fades and their handles (handles = false: not on a take lane, where its fades aren't
// edited), and a looping one a notch where each repeat begins.
void paintClip(Painter& p, const Clip& clip, const RectF& rect, const Rect& visible, int viewLeft, double pps, int sr,
               bool selected, bool audible, bool frozen = false, bool handles = true);
// a looping audio clip: a notch at the top of the clip where each repeat of its loop begins
void paintLoopStarts(Painter& p, const Clip& clip, const RectF& rect, const Rect& visible, double pps, int sr, Color color);
// An audio clip's fade handles (in, out): squares where its fades end (at least clear of the clip's
// trimming edges); false for a clip too narrow to have them.
bool fadeHandles(const Clip& clip, const RectF& body, double pps, int sr, RectF& in, RectF& out);
// each fade as a line from silence at the clip's edge to full level where it ends, the part it
// quietens shaded; and the two handles
void paintFades(Painter& p, const Clip& clip, const RectF& body, double pps, int sr, Color color);
// its name, and its gain when it has one ("Kick   -3.0 dB")
std::string clipLabel(const Clip& clip);
// name sticks to the view's left edge, but stays inside the clip
void paintName(Painter& p, const Clip& clip, const RectF& rect, int viewLeft, Color color);
// the waveform or MIDI notes: copies of ready-made tiles, only those that overlap `visible` (in the
// painter's device pixels when it draws at another scale: sharp at any interface size)
void paintContents(Painter& p, const Clip& clip, const RectF& body, const Rect& visible, double pps, int sr,
                   Color color);
// translucent box over the selected time range on each selected lane; spans: (lane top, height) per track
void paintRange(Painter& p, const TimeRange& range, const std::vector<std::pair<int, int>>& spans,
                const std::function<double(int64_t)>& xOf, const Rect& visible);

}  // namespace clip_painter
