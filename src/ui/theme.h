// The colours and fonts of the whole interface, set by the current skin's stylesheets (skin.h).
//
// Every colour the program draws with is one of COLORS, by name: theme::c("lane") is the variable --lane of
// the stylesheets' :root (default.css, then the skin: underscores in the name are hyphens there). A skin may
// add variables of its own; c() finds those too. The table below keeps every colour's default, what it
// colours, and its group: what docs/skins.md lists, and what is used where no stylesheet could be read. The look is
// flat: every control is a plain fill in the skin's colours, no bevels, gradients or shadows.
// Hovering lightens a control a little (towards its text colour), pressing a little more;
// switches that are on, and a dialog's main button, are filled with the accent colour. Corners are
// slightly rounded (radius()), unless `rounded` is off (Options → Rounded corners).
// The shades a stylesheet derived from the skin (hover, pressed, field edges…) are in Shades.
#pragma once
#include <string>
#include <vector>

#include "core/css.h"
#include "gfx/font.h"
#include "gfx/geom.h"

namespace theme {

struct ColorInfo {
    const char* name;      // theme::c(name); the variable --name in stylesheets (with hyphens)
    const char* def;       // the Dark skin's (default.css)
    const char* label;     // what it colours
    const char* group;     // its group in default.css and docs/skins.md
};
const std::vector<ColorInfo>& colors();  // COLORS, in order

// fonts (from :root's font-family, font-size and --mono-font): "" / 0 = the system's own
struct Fonts {
    std::string ui;        // interface font family
    double uiSize = 0;     // its size in points
    std::string mono = "Consolas";  // times and LUFS numbers ("Consolas" falls back to a monospaced font)
};
// "lane_alt" -> "--lane-alt": a colour's name as a stylesheet variable
std::string variableName(const std::string& name);

// the current skin's colour by name (an unknown name: magenta, so it shows)
const Color& c(const std::string& name);
// The stylesheets the whole interface takes its look from, in cascade order (default.css, then the skin):
// colours, shades and fonts are read from them again. Problems found while computing are added to
// `problems` (if given). No sheets: the program's own stylesheet as built in (builtinSheet()).
void setStyles(std::vector<css::SheetPtr> sheets, std::vector<css::Problem>* problems = nullptr);
// back to the built-in default.css (tests; also what is used before any skin is put on)
void setSkin();
// data/skins/default.css as it was when the program was built: what is used where it can't be read
css::SheetPtr builtinSheet();
// what the stylesheets give the interface's nodes (the toolkit's controls, the program's surfaces)
const css::Engine& styles();
const Fonts& fonts();

// Options → Rounded corners
extern bool rounded;
constexpr int RADIUS = 3;       // px, the corners of buttons and fields
constexpr int SCROLLBAR_W = 10; // px, the thickness of scroll bars
inline int radius(int px = RADIUS) { return rounded ? px : 0; }

// the shades worked out from the skin: default.css's --hover, --pressed… (color-mix of the colours, so a
// skin may also set them outright); without stylesheets, the same mixes of the defaults
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
