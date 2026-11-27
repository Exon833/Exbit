// Wrapping text to a width: one wrapping for the painter's text blocks and for the toolkit's labels and tooltips,
// so what is drawn takes the lines that were measured.
#pragma once
#include <string>
#include <vector>

#include "gfx/font.h"

// the lines of `text` at most `width` px wide in `font`: split at '\n' (a '\r' before it dropped), wrapped at
// spaces, and a word wider than a line (a long path, a log line) broken onto lines of its own after its last '/'
// or '\' that fits, else after its last character that fits (at least one a line)
std::vector<std::string> wrapTextToWidth(const std::string& text, const Font& font, float width);
