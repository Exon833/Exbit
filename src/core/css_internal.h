// The CSS engine's insides (css.h is the interface): the tokenizer, the parsed form of a stylesheet, and the
// value helpers the parser, the cascade and Style's getters share. Not for the rest of the program.
//
// The files: css_syntax.cpp turns text into tokens and tokens into rules (selectors, declarations, shorthands
// expanded); css_values.cpp reads values (colours, lengths, calc(), var() substitution) and holds Style's
// getters; css.cpp matches selectors and runs the cascade (Engine).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/css.h"

namespace css {
namespace detail {

// --- tokens (CSS Syntax 3, §4) ---
enum class Tok : uint8_t {
    Ident, Function, AtKeyword, Hash, String, BadString, Url, BadUrl, Number, Percentage, Dimension,
    Whitespace, CDO, CDC, Colon, Semicolon, Comma, LBracket, RBracket, LParen, RParen, LBrace, RBrace, Delim
};

struct Token {
    Tok type = Tok::Delim;
    bool idHash = false;          // a hash that can be an id (#name, not #123)
    char delim = 0;               // Delim's character
    uint32_t start = 0, end = 0;  // the bytes of the source it was read from
    uint32_t close = 0;           // openers (Function ( [ {): the matching closer's index, or the count if unclosed
    double num = 0;               // Number / Percentage / Dimension
    std::string text;             // the name (ident, function, at-keyword, hash), a string's value, a unit (lower case)
};

struct Tokens {
    std::string_view src;  // (the text tokenized: it must outlive these)
    std::shared_ptr<const std::string> own;  // (when the text is theirs: kept here)
    std::vector<Token> list;
    std::vector<std::pair<uint32_t, std::string>> problems;  // byte offset, message
    size_t size() const { return list.size(); }
    const Token& operator[](size_t i) const { return list[i]; }
    // the text of tokens [b, e) as written: runs of whitespace and comments become one space, ends trimmed
    std::string text(size_t b, size_t e) const;
    // past the component value at i: an opener's whole block, else the token (never beyond `e`)
    size_t skip(size_t i, size_t e) const {
        const Token& k = list[i];
        if (k.type == Tok::Function || k.type == Tok::LParen || k.type == Tok::LBracket || k.type == Tok::LBrace)
            return k.close >= e ? e : k.close + 1;
        return i + 1;
    }
    void trim(size_t& b, size_t& e) const {  // whitespace off both ends of [b, e)
        while (b < e && list[b].type == Tok::Whitespace) b++;
        while (e > b && list[e - 1].type == Tok::Whitespace) e--;
    }
};
void tokenize(std::string_view src, Tokens& out);

// ASCII case-insensitive equality, lower case
bool ieq(std::string_view a, std::string_view b);
std::string lower(std::string_view s);
// the ranges of [b, e) between top-level `sep` tokens, each trimmed (an empty range stays in)
std::vector<std::pair<size_t, size_t>> splitTop(const Tokens& t, size_t b, size_t e, Tok sep);
// the component values of [b, e) separated by whitespace (a function or a block is one)
std::vector<std::pair<size_t, size_t>> components(const Tokens& t, size_t b, size_t e);
// does [b, e) use var()?
bool usesVar(const Tokens& t, size_t b, size_t e);
bool isCssWide(std::string_view text);  // inherit / initial / unset / revert

// --- values ---
struct LenCtx {
    double em = 12;          // px per em
    double percentOf = 0;    // what 100% is, in px
    bool percent = true;     // are percentages allowed?
    // px per rem, vw, vh (vmin / vmax: of those), ch (< 0: half an em). The engine makes these units px on the
    // node (resolveUnits); the defaults are what they are for a node of the default size in the default media.
    double rem = 12, vw = 12.8, vh = 8, ch = -1;
};
// a length in px: 12px, 9pt, 1.5em, 50%, 0, calc(); also rem, vw, vh, vmin, vmax, ch (the LenCtx's) and in, cm,
// mm, Q, pc (96 px to the inch); false if [b, e) is something else
bool evalLength(const Tokens& t, size_t b, size_t e, const LenCtx& ctx, double& px);
bool lengthText(std::string_view text, const LenCtx& ctx, double& px);
// a number: 1.5, 50% (= 0.5), calc() of numbers
bool evalNumber(const Tokens& t, size_t b, size_t e, double& v);
// a colour: [b, e) is exactly one; `current` is currentColor's (null: currentColor isn't one)
bool evalColor(const Tokens& t, size_t b, size_t e, const Rgba* current, Rgba& out);
bool colorText(std::string_view text, const Rgba* current, Rgba& out);
// a number as CSS text ("13.5")
std::string formatNumber(double v);

// var() substituted in `text`: `lookup` gives a custom property's value (null: none, or invalid). False with
// `why` if a var() has neither a value nor a fallback, or is malformed.
using VarLookup = std::function<const std::string*(const std::string& name)>;
bool substitute(std::string_view text, const VarLookup& lookup, std::string& out, std::string& why, int depth = 0);

// --- what paints (css_paint.cpp) ---
bool isGradientFunction(const Token& k);  // linear-gradient( radial-gradient( and their repeating- forms
// one gradient: [b, e) is exactly one gradient function; `current` is currentColor (null: not one)
bool parseGradient(const Tokens& t, size_t b, size_t e, const Rgba* current, double em, Style::Gradient& g,
                   std::string& why);
// a url(): [b, e) is exactly one (url(x), url("x") or url('x')); `url` its text (false: not one, or empty)
bool urlAt(const Tokens& t, size_t b, size_t e, std::string& url);
bool startsUrl(const Token& k);  // a url token, or a url( function
// background-image's value: none, or a comma list of gradients, url()s and none (each layer's kind, gradient and
// url into `out`, which may be null: checking only)
bool parseImages(const Tokens& t, size_t b, size_t e, const Rgba* current, double em,
                 std::vector<Style::Background>* out, std::string& why);
// one layer's background-repeat, -position, -size (the components of its value) and a box keyword (-origin,
// -clip), into `out`'s fields; false if they aren't one
bool repeatValue(const Tokens& t, const std::vector<std::pair<size_t, size_t>>& c, Style::Background& out);
bool positionValue(const Tokens& t, const std::vector<std::pair<size_t, size_t>>& c, double em, Style::Background& out);
bool sizeValue(const Tokens& t, const std::vector<std::pair<size_t, size_t>>& c, double em, Style::Background& out);
bool boxValue(const Tokens& t, std::pair<size_t, size_t> r, Style::Background::Box& out);
// a comma list of those (property: background-repeat, -position, -size, -origin, -clip): each into a layer of
// `out` (null: checking only)
bool parseLayerList(const std::string& property, const Tokens& t, size_t b, size_t e, double em,
                    std::vector<Style::Background>* out, std::string& why);
// box-shadow (box) or text-shadow's value: none, or a comma list of shadows
bool parseShadows(const Tokens& t, size_t b, size_t e, bool box, const Rgba* current, double em,
                  std::vector<Style::Shadow>* out, std::string& why);
// backdrop-filter's value: none, or blur(<length>) (px: the length, 0 for none; may be null)
bool parseBackdropBlur(const Tokens& t, size_t b, size_t e, double em, double* px, std::string& why);

// --- properties ---
bool isShorthand(const std::string& name);
const std::vector<std::string>& longhands(const std::string& shorthand);
// a shorthand's longhands (name, text) from its value [b, e); "" or why it can't be read
std::string expand(const std::string& shorthand, const Tokens& t, size_t b, size_t e,
                   std::vector<std::pair<std::string, std::string>>& out);
// "" if [b, e) is a valid value for the (longhand) property, else why not; properties it doesn't know take anything
std::string checkValue(const std::string& property, const Tokens& t, size_t b, size_t e);
bool isInherited(const std::string& property);

// --- units worked out on the node (css.cpp): rem, vw / vh (vmin / vmax: both), ch ---
enum Unit : uint8_t { URem = 1, UVw = 2, UVh = 4, UCh = 8 };
uint8_t unitsOf(const Tokens& t, size_t b, size_t e);  // the units [b, e) uses (in functions too)
struct UnitPx {  // px per unit
    double rem = 12, vw = 12.8, vh = 8, ch = 6;
};
// `text` with every length in the units `which` made px (each dimension token replaced, the rest as it is)
std::string resolveUnits(const std::string& text, const UnitPx& px, uint8_t which);

// --- a parsed stylesheet ---
struct Selector;

// a pseudo-class that isn't a state: :not() / :is() / :where() of selector lists, or a place among siblings
struct Pseudo {
    enum Kind : uint8_t { Not, Is, FirstChild, LastChild, OnlyChild, NthChild, NthLastChild } kind = Is;
    int a = 0, b = 0;            // :nth-child(an+b)
    std::vector<Selector> args;  // :not() / :is() / :where()
};

struct Compound {
    std::string type;  // "" any (*)
    std::vector<std::string> ids, classes, states;
    std::vector<Pseudo> pseudos;
    bool root = false;      // :root
    char combinator = ' ';  // how it is joined to the compound before it: ' ' descendant, '>' child, '+' the next
                            // sibling, '~' a later sibling
};

struct Selector {
    std::vector<Compound> compounds;  // left to right
    std::string part;                 // ::part, "" none
    uint32_t specificity = 0;         // ids << 20 | classes, states << 10 | types, parts
    uint8_t uses = 0;                 // Engine::UsesPosition | UsesSiblings (its arguments' too)
};

struct Decl {
    std::string name;       // lower case (custom properties as written)
    std::string value;      // the text
    std::string shorthand;  // set when the value has var(): the shorthand this longhand is expanded from at compute
                            // time, `value` being the shorthand's text
    bool important = false, hasVar = false, custom = false;
    uint8_t units = 0;      // Unit: what is worked out on the node (with var(): found after substitution)
    const std::string* source = nullptr;  // the stylesheet's name
    int line = 0, column = 0;
};

struct Rule {
    std::vector<Selector> selectors;
    std::vector<Decl> decls;
    int media = -1;  // the @media block it is in (Stylesheet::media), -1 none
};

// @media: a feature of a query, a query, a block (its query list)
struct MediaFeature {
    enum Name : uint8_t { Width, Height, Resolution, Orientation } name = Width;
    // Width / Height (px), Resolution (dppx): lo <= value <= hi (or < with the strict flags)
    double lo = -1e300, hi = 1e300;
    bool loStrict = false, hiStrict = false;
    int orientation = 0;  // (Orientation) 1 portrait, 2 landscape, 0 either
};
struct MediaQuery {
    bool negated = false;  // `not`
    bool never = false;    // a type other than all / screen, or a query that can't be read (then not negated)
    std::vector<MediaFeature> features;  // all of them
};
struct MediaBlock {
    std::vector<MediaQuery> queries;  // any of them (none: always)
    int parent = -1;                  // the @media block it is in
};
bool mediaMatches(const MediaQuery& q, const Media& m);
bool mediaMatches(const std::vector<MediaQuery>& list, const Media& m);

}  // namespace detail

class Stylesheet {
public:
    std::string name;
    std::vector<detail::Rule> rules;
    std::vector<detail::MediaBlock> media;  // its @media blocks (its imports' too)
    std::vector<std::string> imports;       // the files it imported, at any depth (resolved)
    std::vector<FontFace> fonts;            // its @font-face rules (its imports' too)
    std::vector<std::string> images;        // the files its declarations' url()s name (its imports' too; resolved)
    std::vector<SheetPtr> imported;         // the imported sheets (their rules moved here; kept for their names)
    bool viewport = false;                  // does it use vw / vh (its imports too)?
};

}  // namespace css
