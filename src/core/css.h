// Tiny DAW's own CSS: the stylesheets that give the whole interface its look (skins: data/skins/*.css,
// and the user's own). A defined subset of CSS, parsed and matched here without any library.
//
// SELECTORS. A type (Button, Slider, Lane…: the widgets' and surfaces' names, docs/skins.md), `*`, a class
// (.accent, .hint: from the widgets' properties), an id (#transport), states (:hover :pressed :checked
// :disabled :focus, and any other a node says it has, e.g. :alternate), :root (the node without a parent),
// a part of the node (Slider::handle, ScrollBar::thumb: on the last compound only), combined with the
// descendant (A B), child (A > B), next-sibling (A + B) and later-sibling (A ~ B) combinators; selector lists
// (A, B). Pseudo-classes of selector lists: :not(…), :is(…), :where(…) (complex selectors inside, nested up
// to 16 deep; no parts there). Its place among its siblings (Node::childIndex): :first-child, :last-child,
// :only-child, :nth-child(an+b | odd | even), :nth-last-child(…) (not `of S`); a node whose place isn't known
// matches none of them, nor A + B / A ~ B. Specificity as in CSS: ids, then classes / states / :root / the
// place pseudo-classes, then types / parts; :not() and :is() count as their most specific argument, :where()
// as nothing; then source order. `!important` wins over that. Engine::uses() says whether the sheets look at
// siblings at all (a cache of computed styles must then tell nodes apart by their place: ui/toolkit/widget.h).
//
// VALUES. Colours: #rgb #rgba #rrggbb #rrggbbaa, rgb() / rgba() (commas or spaces, `/ alpha`, numbers or %),
// hsl() / hsla(), the CSS named colours, transparent, currentColor, color-mix(in srgb, A p%, B q%). Lengths:
// px (logical pixels: the interface size scales them), pt, pc, in, cm, mm, Q (96 px to the inch), em (of the
// node's font size), rem (of :root's font size; in :root's own font-size, of the initial 12px), vw / vh / vmin
// / vmax (1% of the Media's width / height: Engine::setMedia), ch (the width of "0" in the node's font, from
// Engine::setCharWidth; half an em without it; in font-size, of the parent's font), % (of what the property
// refers to), and calc() of those (+ - * /). rem, vw, vh and ch become px when the style is computed (so
// inherited values and raw() are px; in custom properties rem / vw / vh do, ch is left for the node that uses
// it); a style computed with vw / vh is stale when the media's size changes (the generation says so).
// var(--name, fallback), nested. Numbers, keywords, strings ('…' "…"), comma lists (font-family). Colours are
// rounded to 8 bits as the program's own colour code rounds (halves to even): color-mix() of two opaque
// colours is exactly the program's own mixColor() (gfx/geom.cpp), so color-mix(in srgb, A 90%, B) is
// mixColor(A, B, 0.10f) to the byte.
//
// AT-RULES. @import url("x.css") / url(x.css) / "x.css", optionally with a media query list: before any rule
// (else reported and skipped), read through ParseOptions::readFile from ParseOptions::baseDir (each imported
// sheet from its own folder); its rules come where the @import is. Loops, more than maxDepth levels, files
// that can't be read, remote URLs, layer() / supports() are reported and skipped; importedFiles() lists every
// file an @import named (resolved, at any depth, read or not). @media <query list> { rules } (nested too, up
// to 32 deep): queries joined by commas (any), each `[not | only] [all | screen | another type] [and (feature)
// …]` or `[not] (feature) [and (feature) …]`; features width / min-width / max-width / height / min-height /
// max-height (px, or em / rem = 16px as in CSS; also the range forms (width >= 600px), (400px < width <
// 800px)), orientation (portrait / landscape), resolution / min-resolution / max-resolution (dppx or x: the
// Media's scale; dpi; dpcm), and each of those alone, (width), as "not zero". Other media types are false (so
// `not print` is true); a query with an unknown feature, `or`, or anything it can't read is false and reported,
// the list's other queries still count. Engine::setMedia evaluates them. @font-face { font-family: "X"; src:
// url(file.ttf) [format(…)], …; font-weight; font-style } goes into fontFaces() (the first url() in src of a
// local file in a format that can be read: TrueType / OpenType, not woff / woff2 / eot / svg; local() names
// are skipped; the path resolved against the sheet's folder; font-weight bold / bolder / 600+ is bold,
// font-style italic / oblique is italic); inside @media it is reported and skipped. @charset is ignored.
// Other at-rules (@supports, @keyframes, @layer…) are reported and skipped, as at-rules inside a rule's block.
//
// CASCADE. An Engine stacks stylesheets in order (the defaults, then a skin, then more): later sheets win at
// equal specificity. Custom properties (--name) and the text properties (color, font-family, font-size,
// font-weight, font-style, letter-spacing, font-smooth, -webkit-font-smoothing) and image-rendering inherit from
// the parent node; the rest don't. var() is resolved on the node it is used on (after inheritance), with cycles
// reported and treated as invalid (the fallback, else the property's default). Shorthands are expanded when
// parsed: padding / margin / border-width / border-style / border-color / border-radius (1–4 values), border /
// border-top… (width style colour, any order; what is left out takes its initial value: medium, none,
// currentColor), background (per layer, in any order: an image (a gradient or a url()), a repeat, a position
// with an optional `/ size`, one or two boxes (origin, then clip), and on the last layer the colour: into
// background-image, -repeat, -position, -size, -origin, -clip and -color; scroll / fixed / local are not read, any
// other word makes it a bad value).
//
// PAINTING VALUES (css_paint.cpp; drawn by ui/toolkit's style::box). background-image: none, or a comma list of
// url() images and linear-gradient() / radial-gradient() and their repeating- forms (the first on top): angles
// in deg, rad, turn, grad, or `to` a side or a corner; a radial gradient's shape (circle / ellipse), size
// (closest-side, closest-corner, farthest-side, farthest-corner, or a circle's length / an ellipse's two lengths
// or percentages) and `at` position (keywords, lengths, percentages); colour stops with no, one or two positions
// (% or lengths, before or after the colour). Style::gradients(), Style::backgrounds() (the layers with their
// background-repeat (repeat-x | repeat-y | [repeat | no-repeat | space | round]{1,2}), background-position (1 to
// 4 values: keywords, lengths, percentages; `right 4px bottom 2px`), background-size (auto | cover | contain |
// [length | % | auto]{1,2}), background-origin and background-clip (border-box | padding-box | content-box), each
// a comma list repeated to the layers' number as in CSS). Wherever a percentage may be there (colour stops,
// the radial size and position, background-position and -size), so may calc() of percentages and lengths,
// calc(100% - 8px): kept as a fraction plus px (ColorStop::plus, …Plus) and worked out against the box where it
// is drawn. box-shadow: none, or a list of [inset] x y [blur
// [spread]] [colour] (the colour: currentColor when left out); text-shadow the same without inset and spread.
// Style::shadows(). border-style and border-top-style…: none hidden solid dashed dotted double groove ridge inset
// outset; a side whose style is none or hidden has a width of 0 (Style::length, edges). opacity: a number or a
// percentage (the whole node: Widget::paintTree). backdrop-filter: none or blur(<length>), only that filter
// (Style::backdropBlur; a window's, ui/toolkit). image-rendering: auto | smooth | high-quality | pixelated |
// crisp-edges. Tiny DAW's own: -tiny-glyph (none | url(): an image in place of a part's drawn glyph),
// -tiny-glyph-size and -tiny-glyph-offset (one or two lengths), -tiny-glyph-tint (none | color). font-smooth
// (auto | never | always | a length) and -webkit-font-smoothing (auto | none | antialiased |
// subpixel-antialiased): never / none draw the node's text without anti-aliasing. declaresOnNodes() tells
// whether a sheet sets a property at all. Transition hints and conic gradients are not supported (reported).
//
// URL()S. In a sheet read from a file (ParseOptions::baseDir set) every url() of a declaration (custom
// properties' too; not @font-face's src, nor @import) is resolved against that sheet's folder when it is parsed,
// so a value keeps naming the right file wherever it is used (var() included): Style::raw() and url() give the
// resolved path. Each file is checked then (ParseOptions::checkImage, else: can it be opened?): one that is
// missing or can't be read is reported at the url()'s line and column (the layer that names it is skipped when
// drawn). Remote URLs (http:, data:…) are reported and never read. imageFiles() lists them all.
//
// PROBLEMS. Nothing in a stylesheet stops it loading: a rule, declaration or value that can't be read is
// left out and reported (line, column, message), as browsers skip what they don't understand; an imported
// sheet's problems name that sheet, and come before the importing sheet's own. Comments /* … */.
//
// THREADS. Parsing and computing are pure functions of their inputs: any thread (parse() calls readFile, and
// compute() the setCharWidth function, on the thread it runs on). An Engine is changed on one thread (the
// GUI's) and read there.
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
    // checks a file a url() names (resolved): false and `why` if it can't be used (missing, not an image…). Null:
    // the file must open. Only with a baseDir (a sheet from a file): other sheets' url()s aren't checked.
    std::function<bool(const std::string& path, std::string& why)> checkImage;
};
// Parse a stylesheet; what can't be read is left out and added to `problems`. Never throws. An @import's rules
// come where the @import is (before the sheet's own rules, as @import must come first).
SheetPtr parse(const std::string& text, const std::string& sourceName, std::vector<Problem>& problems,
               const ParseOptions& options = {});
// the files a sheet's @imports named, at any depth, in order (resolved paths, those that couldn't be read too:
// what to watch for changes)
std::vector<std::string> importedFiles(const SheetPtr& sheet);
// the files its declarations' url()s name (background-image, -tiny-glyph, custom properties…), its imports' too,
// once each, in order (resolved paths; those that couldn't be read too: what to watch for changes)
std::vector<std::string> imageFiles(const SheetPtr& sheet);
// its @font-face rules (its imports' too, whatever their media): a family name and the font file it is in
// (resolved path)
struct FontFace {
    std::string family, file;
    bool bold = false, italic = false;
};
std::vector<FontFace> fontFaces(const SheetPtr& sheet);
// the file a url() (or an @import's "file") of a stylesheet in the folder `dir` names, as the parser resolves it
// ("" for a remote URL: http:, data:…)
std::string resolveUrl(const std::string& dir, const std::string& url);
// a stylesheet's text with the files it names changed: each url() (of declarations, custom properties,
// @font-face's src and @import) and each @import "file" is given to `to` as written (escapes undone), and becomes
// url("what it returns") / "what it returns" ("" or the same: left exactly as it is). Comments, strings and
// everything else stay as they are. (Copying a skin with its files: ui/skin.cpp.)
std::string rewriteUrls(const std::string& text, const std::function<std::string(const std::string& url)>& to);

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
        double plus = 0;       // (a fraction) px added to it: calc(100% - 1px) is 1 and -1
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
        double radiusXPlus = 0, radiusYPlus = 0;  // (…Percent) px added: calc(50% - 2px)
        double centerX = 0.5, centerY = 0.5;  // radial: fractions of the box, or px from its left / top if …Px
        bool centerXPx = false, centerYPx = false;
        double centerXPlus = 0, centerYPlus = 0;  // (fractions) px added: calc(100% - 4px)
        std::vector<ColorStop> stops;
    };
    std::vector<Gradient> gradients(const std::string& property = "background-image") const;
    // background-image's layers (the first on top), each with its background-repeat, -position, -size, -origin
    // and -clip (those lists repeated to the number of layers, as in CSS); `none` layers are there (kind NoImage)
    struct Background {
        enum Kind : uint8_t { NoImage, GradientImage, UrlImage } kind = NoImage;
        Style::Gradient gradient;  // (GradientImage)
        std::string url;           // (UrlImage) the file (resolved against its stylesheet's folder when that was known)
        enum Tiling : uint8_t { Repeat, NoRepeat, Space, Round };
        Tiling repeatX = Repeat, repeatY = Repeat;
        // where: fraction of (the area's size - the image's size), plus px (`right 4px`: 1 and -4)
        double posX = 0, posY = 0, offX = 0, offY = 0;
        enum Sizing : uint8_t { Given, Cover, Contain } sizing = Given;
        double width = -1, height = -1;               // (Given) px, or a fraction if …Percent; -1: auto
        bool widthPercent = false, heightPercent = false;
        double widthPlus = 0, heightPlus = 0;         // (…Percent) px added: calc(100% - 8px) is 1 and -8 (what
                                                      // that comes to is taken as 0 if it is below 0)
        enum Box : uint8_t { BorderBox, PaddingBox, ContentBox };
        Box origin = PaddingBox, clip = BorderBox;
    };
    std::vector<Background> backgrounds() const;
    // the file of a property whose value is a url() (-tiny-glyph): "" if it is none or not set
    std::string url(const std::string& property) const;
    // box-shadow or text-shadow: each shadow, the first on top ("none" or not set: none)
    struct Shadow {
        bool inset = false;
        double x = 0, y = 0, blur = 0, spread = 0;  // px
        Rgba color;
    };
    std::vector<Shadow> shadows(const std::string& property) const;
    // backdrop-filter: blur(<length>)'s length in px (0: none, not set, or another filter)
    double backdropBlur() const;
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
    // bumped by every setSheets (and setMedia, setCharWidth as they say): computed styles kept by callers are
    // stale when it changed
    int generation() const { return generation_; }
    // problems found while computing (var() cycles, values of the wrong type), each reported once
    std::vector<Problem> takeProblems() const;
    // A value with var() can only be checked where it is used: computing a node reports it (takeProblems), but
    // only for the nodes computed. This checks every such declaration (of the rules whose @media applies) at
    // once, with :root's custom properties, as computing would report it: those whose var()s name only
    // properties declared in rules of :root alone (one declared anywhere else may differ on the node). Its
    // problems come with takeProblems(), each once (computing a node later doesn't report them again).
    void checkVarUses() const;
    // what the sheets' selectors (those whose @media applies) look at besides the node, its parts, states,
    // classes and ancestors: a cache of computed styles must tell nodes apart by these too. UsesPosition: its
    // place among its siblings (:first-child, :nth-child()…: its childIndex() and siblingCount()); UsesSiblings:
    // the siblings before it (A + B, A ~ B). With UsesSiblings, a node's style can change while nothing of its
    // own (nor of its ancestors) does: a sibling before it changing its class, type, id or state. The widgets'
    // cache (ui/toolkit/widget.h) follows the earlier siblings' order, types, classes and states, and repaints the
    // later siblings when one's hover / pressed / focus changes (Widget::statesChanged).
    enum Uses { UsesPosition = 1, UsesSiblings = 2 };
    int uses() const { return uses_; }
    // for ch: the width of "0" in a font (family: the first of the node's font-family, "" none; px: its size).
    // Without one: half the size. Bumps the generation.
    void setCharWidth(std::function<double(const std::string& family, double px)> width);

private:
    struct Index;  // the sheets' selectors by their last compound's id / class / type (css.cpp)
    Style computeWith(const Node& node, const Style* parent, const std::string& part) const;
    void rebuild();  // the index of the rules whose @media applies (and uses_)
    std::vector<char> applying() const;  // each sheet's @media blocks, one after another: does it apply?

    std::vector<SheetPtr> sheets_;
    Media media_;
    std::vector<char> active_;  // (applying(), for the current media)
    int uses_ = 0;
    std::function<double(const std::string&, double)> charWidth_;
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
