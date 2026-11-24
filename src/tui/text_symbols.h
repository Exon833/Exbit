// Symbols that have an emoji form (Unicode's Emoji=Yes, Emoji_Presentation=No: ⏮ ✔ ❄ ⚠ ▶ ©…) are one cell
// wide, but some terminals draw them as colour emoji about two cells wide, over the next cell: Windows Terminal
// whenever its font lacks the symbol (U+FE0E, "as text", doesn't change that there). The terminal UI draws them:
//  - as a look-alike that has no emoji form, where there is one (⏮ → ⇤, ✔ → ✓, ▶ → ►…): text in every terminal
//    (Screen::set puts it in the cell);
//  - else as the symbol followed by U+FE0E, for the terminals that honour it (Screen::output; the Windows console
//    leaves it out: a classic console draws it as a cell of its own).
#pragma once

namespace tui {

bool hasEmojiForm(char32_t cp);
// its look-alike without an emoji form, one cell as it is; `cp` itself if there is none
char32_t textForm(char32_t cp);

}  // namespace tui
