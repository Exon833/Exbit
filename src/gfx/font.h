// Fonts: Tiny DAW's own TrueType reader and glyph rasterizer (no FreeType).
//
// A Font is a family + pixel size + bold. Families are looked up among the system's .ttf/.otf
// (TrueType outlines) files: on Linux /usr/share/fonts, /usr/local/share/fonts, ~/.local/share/fonts,
// ~/.fonts; on Windows C:\Windows\Fonts. Family names are matched case-insensitively on the font's
// own name table (and on the file name as a fallback). "" means the system's own interface font
// (Linux: DejaVu Sans, Noto Sans, Liberation Sans… whichever exists; Windows: Segoe UI). A skin's own
// font files (@font-face) are added by addFamily and found first.
// Glyphs a font lacks (✕ ▾ ♩ ⟲ 🎹 …) come from fallback fonts (DejaVu Sans, DejaVu Sans Mono,
// Noto Sans Symbols, Symbola… whichever exist); a glyph nobody has is drawn as a hollow box.
//
// Glyphs are rendered once per (face, size, glyph, subpixel x offset in quarters, aliased or not) into
// 8-bit coverage masks and cached. Everything here is for the GUI thread only.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "gfx/geom.h"

class FontFace;  // internal: one loaded font file

class Font {
public:
    Font();  // the interface font at its default size
    Font(const std::string& family, float pixelSize, bool bold = false);
    static Font fromPoints(const std::string& family, float points, bool bold = false);  // 96 dpi: px = pt * 4/3

    const std::string& family() const { return family_; }
    float pixelSize() const { return size_; }
    float pointSize() const { return size_ * 0.75f; }
    bool bold() const { return bold_; }
    Font withPixelSize(float px) const { return Font(family_, px, bold_).withAliased(aliased_); }
    Font withPointSize(float pt) const { return fromPoints(family_, pt, bold_).withAliased(aliased_); }
    Font withBold(bool b) const { return Font(family_, size_, b).withAliased(aliased_); }
    // drawn without anti-aliasing (CSS font-smooth: never): each glyph's pixels fully on or off (its coverage
    // thresholded at half), each glyph on a whole pixel. Advances and metrics are the same.
    bool aliased() const { return aliased_; }
    Font withAliased(bool a) const {
        Font f = *this;
        f.aliased_ = a;
        return f;
    }

    float ascent() const;    // px above the baseline
    float descent() const;   // px below the baseline (positive)
    float height() const { return ascent() + descent(); }
    float lineSpacing() const;
    float width(const std::string& utf8) const;               // advance width
    float charWidth(uint32_t codepoint) const;
    std::string elided(const std::string& utf8, float maxWidth) const;  // cut with "…" to fit
    // index in bytes of the character boundary closest to x px into the text (for text cursors)
    size_t hitTest(const std::string& utf8, float x) const;
    // can this font (or a fallback) draw the codepoint?
    bool hasGlyph(uint32_t codepoint) const;

    // the interface font set by the skin (theme), and the numbers font
    static Font ui();          // the skin's "ui" font at its "ui_size" (0: 9 pt)
    static Font mono();        // the skin's "mono" font at the ui size
    static void setDefaults(const std::string& uiFamily, float uiPoints, const std::string& monoFamily);
    static std::vector<std::string> families();  // installed family names, sorted (skin editor)
    // a family from a font file (a skin's @font-face): `family` finds it from now on, before the system's fonts
    // of that name, as a face of the weight and style given (whatever the file says). False if the file has no
    // face that can be read (TrueType outlines: .ttf, .ttc, most .otf). Adding the same again changes nothing.
    static bool addFamily(const std::string& family, const std::string& path, bool bold = false, bool italic = false);

    // internal: used by Painter::drawText
    struct Glyph {
        int w = 0, h = 0;        // mask size
        int left = 0, top = 0;   // mask's top-left relative to the pen position on the baseline
                                 // (y down: top < 0 for a glyph above the baseline)
        std::vector<uint8_t> mask;
    };
    struct Run { const Glyph* glyph; float x; };  // glyph and pen x, laid out from 0
    // lays out the text; returns the total advance. Glyph pointers stay valid until the cache is
    // trimmed (at the start of the next frame at the earliest), so draw them right away.
    // Run::x is the pen position startX + offset, snapped to 1/4 px; each glyph is rendered for
    // that quarter-pixel phase, so its mask goes at (floor(x) + left, baselineY + top) with the
    // baseline on a whole pixel. (The cache is trimmed, when over its limit, at the start of a
    // layout() call, never during one.)
    float layout(const std::string& utf8, float startX, std::vector<Run>& out) const;

private:
    std::string family_;
    float size_;
    bool bold_;
    bool aliased_ = false;
    FontFace* face_ = nullptr;  // resolved lazily
    FontFace* face() const;
};

// UTF-8 helpers
uint32_t utf8Next(const std::string& s, size_t& i);        // decodes at i, moves i on
std::u32string utf8ToU32(const std::string& s);
std::string u32ToUtf8(const std::u32string& s);
std::string codepointToUtf8(uint32_t cp);
size_t utf8PrevBoundary(const std::string& s, size_t i);
size_t utf8NextBoundary(const std::string& s, size_t i);
