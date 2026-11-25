// Any of the toolkit's windows (a dialog: a message, a question, the file chooser, Audio properties…)
// drawn as text in the terminal, and given the keys typed (exbit --tui). The program's dialogs are
// built of the toolkit's controls; each is drawn as what it is (a label's text, [ OK ], [x] a check
// box, [text_] a line edit, a list's rows…), the one with the keyboard focus reversed. Controls are
// placed by their geometry: those side by side in the window share a row of the text (rows: the
// controls' heights overlapping), across as far as their x says. Their keys go to the window as from
// the keyboard (Tab to the next control, Return its default button, Escape to cancel): the dialog
// works as it does in the window system, by its own code.
#pragma once
#include "tui/screen.h"
#include "tui/term.h"

namespace ui {
class Window;
}

namespace tui {

constexpr int COL_PX = 7;  // a window's pixels across a column of text (its controls' x and width)
constexpr int ROW_PX = 22;  // …and down a row (how many rows a list or a long label takes)

// draws it in a box centred in `area` (x, y, w, h cells), as big as its controls need up to the area
void drawWindow(Screen& s, ui::Window& w, int ax, int ay, int aw, int ah);
// the key, pressed and let go, to the window (its focused control, its shortcuts)
void sendKey(ui::Window& w, const Key& k);

}  // namespace tui
