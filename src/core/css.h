// Tiny DAW's own CSS: the stylesheets that give the whole interface its look (skins: data/skins/*.css,
// and the user's own). A defined subset of CSS, parsed and matched here without any library.
//
// SELECTORS. A type (Button, Slider, Lane…: the widgets' and surfaces' names, docs/skins.md), `*`, a class
// (.accent, .hint: from the widgets' properties), an id (#transport), states (:hover :pressed :checked
// :disabled :focus, and any other a node says it has, e.g. :alternate), :root (the node without a parent),
// a part of the node (Slider::handle, ScrollBar::thumb: on the last compound only), combined with the
// descendant (A B) and child (A > B) combinators; selector lists (A, B). Specificity as in CSS: ids, then
// classes / states / :root, then types / parts; then source order. `!important` wins over that.
//
// VALUES. Colours: #rgb #rgba #rrggbb #rrggbbaa, rgb() / rgba() (commas or spaces, `/ alpha`, numbers or %),
// hsl() / hsla(), the CSS named colours, transparent, currentColor, color-mix(in srgb, A p%, B q%). Lengths:
// px (logical pixels: the interface size scales them), pt, em (of the node's font size), % (of what the
// property refers to), and calc() of those (+ - * /). var(--name, fallback), nested. Numbers, keywords,
// strings ('…' "…"), comma lists (font-family). Colours are rounded to 8 bits as the program's own colour code
// rounds (halves to even): color-mix() of two opaque colours is exactly the program's own mixColor()
// (gfx/geom.cpp), so color-mix(in srgb, A 90%, B) is mixColor(A, B, 0.10f) to the byte.
//
// CASCADE. An Engine stacks stylesheets in order (the defaults, then a skin, then more): later sheets win at
// equal specificity. Custom properties (--name) and the text properties (color, font-family, font-size,
// font-weight, font-style, letter-spacing) inherit from the parent node; the rest don't. var() is resolved on
// the node it is used on (after inheritance), with cycles reported and treated as invalid (the fallback,
// else the property's default). Shorthands are expanded when parsed: padding / margin / border-width /
// border-style / border-color / border-radius (1–4 values), border / border-top… (width style colour, any
// order; what is left out takes its initial value: medium, none, currentColor), background (the colour of its
// last layer and each layer's gradient, in any order: background-color and background-image; url() images,
// positions, sizes and repeats in it are not read).
//
// PAINTING VALUES (css_paint.cpp; drawn by ui/toolkit's style::box). background-image: none, or a comma list of
// linear-gradient() / radial-gradient() and their repeating- forms (the first on top): angles in deg, rad,
// turn, grad, or `to` a side or a corner; a radial gradient's shape (circle / ellipse), size (closest-side,
// closest-corner, farthest-side, farthest-corner, or a circle's length / an ellipse's two lengths or
// percentages) and `at` position (keywords, lengths, percentages); colour stops with no, one or two positions
// (% or lengths, before or after the colour). Style::gradients(). box-shadow: none, or a list of [inset] x y
// [blur [spread]] [colour] (the colour: currentColor when left out); text-shadow the same without inset and
// spread. Style::shadows(). border-style and border-top-style…: none hidden solid dashed dotted double groove
// ridge inset outset; a side whose style is none or hidden has a width of 0 (Style::length, edges). opacity:
// a number or a percentage (the whole node: Widget::paintTree). declaresOnNodes() tells whether a sheet sets a
// property at all. Transition hints, conic gradients and url() images are not supported (reported).
//
// PROBLEMS. Nothing in a stylesheet stops it loading: a rule, declaration or value that can't be read is
// left out and reported (line, column, message), as browsers skip what they don't understand. At-rules are
// skipped (reported) except @charset (ignored). Comments /* … */.
//
// MORE (being added: features/css-full; each part documents itself where it is implemented):
// - selectors: :not(…), :is(…), :first-child, :last-child, :only-child, :nth-child(an+b), and the sibling
//   combinators A + B and A ~ B (Node::childIndex / sibling);
// - at-rules: @import url(…) / "…" (ParseOptions), @media (width / height / resolution… : Engine::setMedia),
//   @font-face (fontFaces());
// - units: rem (of :root's font size), vw / vh (of the media's width / height), ch.
//
// THREADS. Parsing and computing are pure functions of their inputs: any thread. An Engine is changed on
// one thread (the GUI's) and read there.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace css {

struct Problem {
    std::string source;  // the stylesheet's name (its file)
    int line = 0, column = 0;  // 1-based
    std::string message;
    std::string text() const;  // "dark.css:12:5: unknown colour 'blu'"
};

struct Rgba {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool operator==(const Rgba& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
    bool operator!=(const Rgba& o) const { return !(*this == o); }
};

// a colour on its own (the skin's variables, tests): #hex, rgb(), hsl(), a name, color-mix() of colours;
// false if it isn't one (var() isn't resolved here)
bool parseColor(const std::string& text, Rgba& out);
// "#rrggbb" or "#rrggbbaa" (with alpha below 255)
std::string hex(const Rgba& c);
// a value that is one string ("a \"b\"" or 'x'): its text, escapes undone; any other value: as it is, trimmed
std::string unquote(const std::string& value);

// --- what selectors match ---
// A node of the interface as the stylesheets see it: implemented by the widgets (ui/toolkit) and by the
// program's own surfaces (lanes, clips, the ruler…), which are often not widgets but drawn parts of one.
class Node {
public:
    virtual ~Node() = default;
    // its type, and the types it counts as too (a ToolButton is a Button: Button rules apply to it as well,
    // ToolButton rules win at equal specificity only by source order, as for any two type selectors)
    virtual const std::string& type() const = 0;
    virtual bool isType(const std::string& name) const { return name == type(); }
    virtual const std::string& id() const = 0;  // "" none
    virtual bool hasClass(const std::string& name) const = 0;
    virtual bool hasState(const std::string& name) const = 0;  // hover, pressed, checked, disabled, focus…
    virtual const Node* parent() const = 0;     // null: the root
    // its place among its parent's children, for :first-child / :nth-child(…) and the + / ~ combinators: its
    // index (-1: not known: those selectors don't match it), how many children the parent has, and the
    // parent's child at an index (null: none)
    virtual int childIndex() const { return -1; }
    virtual int siblingCount() const { return 0; }
    virtual const Node* sibling(int index) const { return nullptr; }
};

// A simple node for tests and for surfaces that aren't widgets.
class SimpleNode : public Node {
public:
    std::string typeName, idName;
    std::vector<std::string> types;    // more types it counts as
    std::vector<std::string> classes, states;
    const Node* up = nullptr;
    explicit SimpleNode(std::string t = "", const Node* parent = nullptr) : typeName(std::move(t)), up(parent) {}
    const std::string& type() const override { return typeName; }
    bool isType(const std::string& name) const override;
    const std::string& id() const override { return idName; }
    bool hasClass(const std::string& name) const override;
    bool hasState(const std::string& name) const override;
    const Node* parent() const override { return up; }
    // (siblings: the parent's children in order, this one at `index`; null: not known)
    const std::vector<const Node*>* family = nullptr;
    int index = -1;
    int childIndex() const override { return family ? index : -1; }
    int siblingCount() const override { return family ? (int)family->size() : 0; }
    const Node* sibling(int i) const override {
        return family && i >= 0 && i < (int)family->size() ? (*family)[(size_t)i] : nullptr;
    }
};

// --- stylesheets ---
class Stylesheet;
using SheetPtr = std::shared_ptr<const Stylesheet>;

struct ParseOptions {
    // where relative url()s are taken from (@import, @font-face's src): the stylesheet's folder ("": as written)
    std::string baseDir;
    // reads an imported stylesheet's file (its path, resolved): false if it can't be read. Null: @import is
    // reported and skipped. An import is parsed with its own folder as baseDir; loops and more than
    // maxDepth levels are reported and skipped.
    std::function<bool(const std::string& path, std::string& text)> readFile;
    int maxDepth = 8;
};
// Parse a stylesheet; what can't be read is left out and added to `problems`. Never throws. An @import's rules
// come where the @import is (before the sheet's own rules, as @import must come first).
SheetPtr parse(const std::string& text, const std::string& sourceName, std::vector<Problem>& problems,
               const ParseOptions& options = {});
// the files a sheet imported, at any depth (resolved paths: what to watch for changes)
std::vector<std::string> importedFiles(const SheetPtr& sheet);
// its @font-face rules (its imports' too): a family name and the font file it is in (resolved path)
struct FontFace {
    std::string family, file;
    bool bold = false, italic = false;
};
std::vector<FontFace> fontFaces(const SheetPtr& sheet);

// --- computed styles ---
struct Edges {  // padding, margin, border widths (px), or radii (top-left, top-right, bottom-right, bottom-left)
    double top = 0, right = 0, bottom = 0, left = 0;
};

// What applies to one node (and maybe one of its parts) after the cascade, inheritance and var().
class Style {
public:
    // was the property declared for it (or inherited)?
    bool has(const std::string& property) const;
    // its value as written, var() resolved ("" if not there)
    std::string raw(const std::string& property) const;
    // typed: `def` when not there or not of that type
    Rgba color(const std::string& property, Rgba def) const;              // currentColor: its `color`
    double length(const std::string& property, double def, double percentOf = 0) const;  // in px
    double number(const std::string& property, double def) const;
    std::string keyword(const std::string& property, const std::string& def) const;  // lower case
    std::vector<std::string> list(const std::string& property) const;    // comma list, quotes taken off
    Edges edges(const std::string& prefix, const Edges& def) const;      // "padding", "margin", "border-width"
    Edges radii(const Edges& def) const;                                  // border-*-radius
    double fontSizePx(double def) const;                                  // font-size (pt/em/% handled)

    // background-image's gradients (also from the background shorthand), the first drawn on top as in CSS;
    // empty: none
    struct ColorStop {
        Rgba color;
        double position = -1;  // where on the gradient line: a fraction (0…1), or px if `px`; -1: not given
        bool px = false;
    };
    struct Gradient {
        enum Kind { Linear, Radial } kind = Linear;
        bool repeating = false;
        double angle = 180;  // linear: degrees (0: to top, 90: to right, 180: to bottom); `to right bottom`: corner
        bool toCorner = false;  // (linear) towards a corner: angle is worked out from the box's shape
        int cornerX = 0, cornerY = 0;  // (toCorner) -1 left / top, +1 right / bottom
        bool circle = false;  // radial: a circle (else an ellipse)
        enum Size { FarthestCorner, ClosestSide, ClosestCorner, FarthestSide, Explicit } size = FarthestCorner;
        double radiusX = 0, radiusY = 0;  // (Explicit) px, or fractions of the box's width / height if …Percent
        bool radiusXPercent = false, radiusYPercent = false;
        double centerX = 0.5, centerY = 0.5;  // radial: fractions of the box, or px from its left / top if …Px
        bool centerXPx = false, centerYPx = false;
        std::vector<ColorStop> stops;
    };
    std::vector<Gradient> gradients(const std::string& property = "background-image") const;
    // box-shadow or text-shadow: each shadow, the first on top ("none" or not set: none)
    struct Shadow {
        bool inset = false;
        double x = 0, y = 0, blur = 0, spread = 0;  // px
        Rgba color;
    };
    std::vector<Shadow> shadows(const std::string& property) const;
    // every custom property's value (--name -> text, var() resolved)
    const std::map<std::string, std::string>& variables() const { return vars_; }
    std::optional<std::string> variable(const std::string& name) const;  // "--lane" or "lane"

    // What a user of the style works out from it once (ui/toolkit/style.h: a box's colours, borders and radius),
    // kept with it: a computed style doesn't change. Not copied with the style (a copy may be changed); whoever
    // changes props_ of a style already in use resets it.
    struct Memo {
        std::shared_ptr<const void> value;
        Memo() = default;
        Memo(const Memo&) {}
        Memo& operator=(const Memo&) {
            value.reset();
            return *this;
        }
    };
    mutable Memo memo_;

    // (Engine fills these)
    std::map<std::string, std::string> props_;  // property -> resolved text
    std::map<std::string, std::string> vars_;
    double emPx_ = 12;                           // the node's font size in px, for em
};

// What @media rules are evaluated against, and what vw / vh are of.
struct Media {
    double width = 1280, height = 800;  // the main window's size (logical px)
    double scale = 1;                   // the interface size (1: 100 %): resolution in dppx
};

class Engine {
public:
    // the media @media rules are evaluated against; bumps the generation only if that changes which @media
    // rules apply, or the size when a sheet uses vw / vh
    void setMedia(const Media& media);
    const Media& media() const { return media_; }
    // the sheets, in cascade order (earlier ones are overridden by later ones at equal specificity)
    void setSheets(std::vector<SheetPtr> sheets);
    const std::vector<SheetPtr>& sheets() const { return sheets_; }
    // what applies to `node` (its `part` if not ""), inheriting from `parentStyle` (its parent's computed
    // style; null: the root's, so the skin's variables are always there)
    Style compute(const Node& node, const Style* parentStyle = nullptr, const std::string& part = "") const;
    // the root's style (:root / the node without parent): the skin's variables
    const Style& root() const;
    // bumped by every setSheets: computed styles kept by callers are stale when it changed
    int generation() const { return generation_; }
    // problems found while computing (var() cycles, values of the wrong type), each reported once
    std::vector<Problem> takeProblems() const;

private:
    struct Index;  // the sheets' selectors by their last compound's id / class / type (css.cpp)
    Style computeWith(const Node& node, const Style* parent, const std::string& part) const;

    std::vector<SheetPtr> sheets_;
    Media media_;
    int generation_ = 0;
    std::shared_ptr<const Index> index_;
    mutable std::unique_ptr<Style> root_;
    mutable std::vector<Problem> problems_;
    mutable std::set<std::string> reported_;  // (each problem once per set of sheets)
};

// does any rule of the engine's sheets declare `property` for nodes themselves (not only for parts, ::name)?
// (Widget::paintTree asks it for opacity once per set of sheets: no rule, nothing to look up per widget)
bool declaresOnNodes(const Engine& engine, const std::string& property);

}  // namespace css
