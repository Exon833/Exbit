// The colours and fonts of the whole interface, set by the current skin (theme.py).
//
// Every colour the program draws with is one of COLORS, by name: theme::c("lane"). The look is
// flat: every control is a plain fill in the skin's colours, no bevels, gradients or shadows.
// Hovering lightens a control a little (towards its text colour), pressing a little more;
// switches that are on, and a dialog's main button, are filled with the accent colour. Corners are
// slightly rounded (radius()), unless `rounded` is off (Options → Rounded corners).
// The shades a stylesheet derived from the skin (hover, pressed, field edges…) are in Shades.
#pragma once
#include <string>
#include <vector>

#include "gfx/font.h"
#include "gfx/geom.h"

namespace theme {

struct ColorInfo {
    const char* name;      // in skin files
    const char* def;       // the Dark skin's
    const char* label;     // what it colours (skin editor)
    const char* group;     // group in the skin editor
};
const std::vector<ColorInfo>& colors();  // COLORS, in order

// fonts: "" / 0 = the system's own
struct Fonts {
    std::string ui;        // interface font family
    double uiSize = 0;     // its size in points
    std::string mono = "Consolas";  // times and LUFS numbers ("Consolas" falls back to a monospaced font)
};
extern const char* const FONT_LABEL_UI;
extern const char* const FONT_LABEL_UI_SIZE;
extern const char* const FONT_LABEL_MONO;

// the current skin's colour by name (an unknown name: magenta, so it shows)
const Color& c(const std::string& name);
// put a skin on: colours {name: "#rrggbb[aa]"} (missing/invalid: the default) and fonts
void setSkin(const std::vector<std::pair<std::string, std::string>>& colors = {}, const Fonts& fonts = Fonts());
const Fonts& fonts();
// every colour of the current skin, as "#rrggbb[aa]" by name (skin editor, export)
std::vector<std::pair<std::string, std::string>> currentColors();

// Options → Rounded corners
extern bool rounded;
constexpr int RADIUS = 3;       // px, the corners of buttons and fields
constexpr int SCROLLBAR_W = 10; // px, the thickness of scroll bars
inline int radius(int px = RADIUS) { return rounded ? px : 0; }

// the shades worked out from the skin (theme.stylesheet())
struct Shades {
    Color hover;        // a control under the mouse          mix(button, button_text, .10)
    Color pressed;      // pressed, or switched on            mix(button, button_text, .18)
    Color disabled;     // a disabled button's fill           mix(button, window, .5)
    Color line;         // dividing lines, edges of menus     mix(window, window_text, .10)
    Color fieldEdge;    // edge of fields                     mix(base, text, .14)
    Color fieldHover;   // … under the mouse                  mix(base, text, .26)
    Color itemHover;    // list rows under the mouse          mix(base, text, .07)
    Color panel;        // boxes grouping controls            mix(window, base, .45)
    Color handle;       // scroll bar handles                 mix(window, window_text, .22)
    Color handleHover;  //                                    mix(window, window_text, .38)
    Color accentHover;  //                                    mix(accent, accent_text, .15)
};
const Shades& shades();
// a lit toggle's colours (mute / solo / record roles): its fill, and under the mouse
Color toggleColor(const std::string& role);
Color toggleHover(const std::string& role);

// fonts as the stylesheet set them
Font uiFont();                   // the interface font (9 pt default)
Font monoFont(double points);    // the numbers font at that size ("time": 12 pt, "lufs": 8 pt)
Font dimmedFont();               // QLabel[dimmed]: 8 pt

}  // namespace theme
