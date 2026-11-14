// Automatic contrast for clip colours (colors.py).
//
// Whatever colour a clip gets, clipColors() derives everything drawn on it so it stays readable:
// the fill is lifted until it stands out from the dark lanes, the waveform/notes are pushed lighter
// or darker until they stand out from the fill, and the name is drawn in white or near-black,
// whichever reads better, on a handle strip shaded until the name is easy to read. A coloured
// track's row in the panel follows the same rules (trackColors()). Contrast is the WCAG ratio.
#pragma once
#include <string>
#include <vector>

#include "gfx/geom.h"
#include "model/model.h"

namespace colors {

extern const Color WHITE, INK;
constexpr double MIN_FILL_VS_LANE = 2.0;
constexpr double MIN_CONTENT_VS_FILL = 3.0;
constexpr double MIN_TEXT_VS_HANDLE = 4.5;
constexpr double SILENT_SATURATION = 0.15;

double luminance(const Color& c);
double contrast(const Color& a, const Color& b);
Color mix(const Color& a, const Color& b, double t);
Color ensureContrast(const Color& color, const Color& against, double ratio);
Color readableText(const Color& background);

struct ClipColors {
    Color fill, handle, content, text, border, selectedBorder;
};

extern bool followTrack;  // clips without a colour of their own take their track's (Options → Colours)

std::string defaultColor(Kind kind);  // the skin's clip colour for the kind, "#rrggbb" (a return track: its stripe's)
Color trackAccent(Kind kind);          // the stripe down a track's row: accent_audio / accent_midi / accent_return
// (colour "#rrggbb", tone) a clip is drawn in: its own, else its track's (followTrack), else its kind's
void clipColor(const Clip& clip, std::string& color, std::string& tone);
// a pleasant random colour for a new track, its hue far from those in `taken`
std::string randomColor(const std::vector<std::string>& taken);
Color toneColor(const std::string& tone);  // "light" → white, "dark" → #2a2c30
Color clipFill(const std::string& hexColor, bool audible = true);
bool toneAvailable(const std::string& hexColor, const std::string& tone, bool audible = true);
void trackColors(const std::string& hexColor, const std::string& tone, Color& background, Color& text);
ClipColors clipColors(const std::string& hexColor, bool audible = true, const std::string& tone = "");
void clearCache();  // after a skin change (the lanes' colour is part of the result)

}  // namespace colors
