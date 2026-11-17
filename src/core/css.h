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
// border-color / border-radius (1–4 values), border / border-top… (width style colour, any order),
// background (its colour).
//
// PROBLEMS. Nothing in a stylesheet stops it loading: a rule, declaration or value that can't be read is
// left out and reported (line, column, message), as browsers skip what they don't understand. At-rules are
// skipped (reported) except @charset (ignored). Comments /* … */.
//
// THREADS. Parsing and computing are pure functions of their inputs: any thread. An Engine is changed on
// one thread (the GUI's) and read there.
#pragma once
#include <cstdint>
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
};

// --- stylesheets ---
class Stylesheet;
using SheetPtr = std::shared_ptr<const Stylesheet>;

// Parse a stylesheet; what can't be read is left out and added to `problems`. Never throws.
SheetPtr parse(const std::string& text, const std::string& sourceName, std::vector<Problem>& problems);

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

class Engine {
public:
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
    int generation_ = 0;
    std::shared_ptr<const Index> index_;
    mutable std::unique_ptr<Style> root_;
    mutable std::vector<Problem> problems_;
    mutable std::set<std::string> reported_;  // (each problem once per set of sheets)
};

}  // namespace css
