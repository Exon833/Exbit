#include "gfx/wrap_text_to_width.h"

std::vector<std::string> wrapTextToWidth(const std::string& text, const Font& font, float width) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (;;) {
        size_t nl = text.find('\n', start);
        std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!para.empty() && para.back() == '\r') para.pop_back();
        if (font.width(para) <= width) {
            lines.push_back(para);
        } else {
            std::string line;
            size_t i = 0;
            for (;;) {
                size_t sp = para.find(' ', i);
                if (sp == std::string::npos) sp = para.size();
                std::string word = para.substr(i, sp - i);
                std::string cand = line.empty() ? word : line + " " + word;
                if (!line.empty() && font.width(cand) > width) {
                    lines.push_back(line);
                    cand = word;
                }
                while (font.width(cand) > width) {  // (a word wider than a line, alone on it)
                    size_t fit = font.hitTest(cand, width);
                    while (fit > 0 && font.width(cand.substr(0, fit)) > width) fit = utf8PrevBoundary(cand, fit);
                    if (fit == 0) fit = utf8NextBoundary(cand, 0);
                    size_t slash = cand.find_last_of("/\\", fit - 1);
                    if (slash != std::string::npos && slash > 0) fit = slash + 1;
                    lines.push_back(cand.substr(0, fit));
                    cand = cand.substr(fit);
                }
                line = cand;
                if (sp == para.size()) break;
                i = sp + 1;
            }
            lines.push_back(line);
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}
