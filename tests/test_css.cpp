// The CSS engine (core/css.h): the tokenizer's edge cases, every colour form (color-mix() against the
// program's own mixColor, byte for byte), selectors and specificity, !important, the order of sheets,
// combinators, parts, states, :root variables, inheritance, var() (fallbacks, nesting, cycles), calc() and
// units, shorthands, error recovery and where problems are reported, random and cut-short stylesheets that
// must never crash, and the speed of computing many styles.
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "core/css.h"
#include "core/css_internal.h"
#include "core/util.h"
#include "gfx/geom.h"

using namespace css;

namespace {

Rgba rgba(int r, int g, int b, int a = 255) { return Rgba{(uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a}; }

Rgba color(const std::string& text) {
    Rgba c{1, 2, 3, 4};
    if (!parseColor(text, c)) return Rgba{1, 2, 3, 4};
    return c;
}
bool isColor(const std::string& text) {
    Rgba c;
    return parseColor(text, c);
}

// an engine with one stylesheet (problems kept)
struct Sheet {
    Engine engine;
    std::vector<Problem> problems;
    explicit Sheet(const std::string& text, const std::string& name = "test.css") {
        engine.setSheets({parse(text, name, problems)});
    }
    bool reported(const std::string& what) const {
        for (const Problem& p : problems)
            if (p.message.find(what) != std::string::npos) return true;
        return false;
    }
};

bool anyProblem(const std::vector<Problem>& ps, const std::string& what) {
    for (const Problem& p : ps)
        if (p.message.find(what) != std::string::npos) return true;
    return false;
}

void testTokenizer() {
    using namespace css::detail;
    Tokens t;
    tokenize("a/* c */b /* unterminated", t);
    CHECK(t.size() == 3);  // a, b, whitespace (comments make no tokens)
    CHECK(t[0].type == Tok::Ident && t[0].text == "a");
    CHECK(t[1].type == Tok::Ident && t[1].text == "b");
    CHECK(t.problems.size() == 1 && t.problems[0].second.find("comment") != std::string::npos);
    CHECK(t.text(0, 2) == "a b");  // (a comment keeps tokens apart in the text)

    tokenize("\\41 BC \\-x \\31 23 'it\\'s' \"a\\\nb\" '\\66 oo'", t);
    CHECK(t[0].type == Tok::Ident && t[0].text == "ABC");
    CHECK(t[2].type == Tok::Ident && t[2].text == "-x");
    CHECK(t[4].type == Tok::Ident && t[4].text == "123");
    CHECK(t[6].type == Tok::String && t[6].text == "it's");
    CHECK(t[8].type == Tok::String && t[8].text == "ab");  // (an escaped line break continues the string)
    CHECK(t[10].type == Tok::String && t[10].text == "foo");
    CHECK(t.problems.empty());

    tokenize("'open\nx", t);
    CHECK(t[0].type == Tok::BadString);
    CHECK(t.problems.size() == 1);
    tokenize("\"to the end", t);
    CHECK(t.size() == 1 && t[0].type == Tok::String && t[0].text == "to the end");
    CHECK(t.problems.size() == 1);

    tokenize("12 -3.5px +.5em 50% 1e2 1E-1x #fff #1ab #-a --var -moz-x u+1 @media url( a.png ) url('b') rgb(", t);
    std::vector<Token> k;
    for (const Token& x : t.list)
        if (x.type != Tok::Whitespace) k.push_back(x);
    CHECK(k[0].type == Tok::Number && k[0].num == 12);
    CHECK(k[1].type == Tok::Dimension && k[1].num == -3.5 && k[1].text == "px");
    CHECK(k[2].type == Tok::Dimension && k[2].num == 0.5 && k[2].text == "em");
    CHECK(k[3].type == Tok::Percentage && k[3].num == 50);
    CHECK(k[4].type == Tok::Number && k[4].num == 100);
    CHECK(k[5].type == Tok::Dimension && std::fabs(k[5].num - 0.1) < 1e-12 && k[5].text == "x");
    CHECK(k[6].type == Tok::Hash && k[6].idHash && k[6].text == "fff");
    CHECK(k[7].type == Tok::Hash && !k[7].idHash && k[7].text == "1ab");
    CHECK(k[8].type == Tok::Hash && k[8].idHash && k[8].text == "-a");
    CHECK(k[9].type == Tok::Ident && k[9].text == "--var");
    CHECK(k[10].type == Tok::Ident && k[10].text == "-moz-x");
    CHECK(k[11].type == Tok::Ident && k[11].text == "u");
    CHECK(k[12].type == Tok::Number && k[12].num == 1);
    CHECK(k[13].type == Tok::AtKeyword && k[13].text == "media");
    CHECK(k[14].type == Tok::Url && k[14].text == "a.png");
    CHECK(k[15].type == Tok::Function && k[15].text == "url");
    CHECK(k[16].type == Tok::String && k[16].text == "b");
    CHECK(k.back().type == Tok::Function && k.back().close == t.size());  // (unclosed)

    // blocks know their closers; a closer of the wrong kind doesn't close
    tokenize("a(b[c)d]e)", t);
    CHECK(t.size() == 9);
    CHECK(t[0].type == Tok::Function && t[0].close == 8);
    CHECK(t[2].type == Tok::LBracket && t[2].close == 6);
    CHECK(t.skip(0, t.size()) == 9);

    // NUL and UTF-8 pass through as name characters
    tokenize(std::string("caf\xC3\xA9 a\0b", 9), t);
    CHECK(t[0].text == "caf\xC3\xA9");
    CHECK(t[2].text == "a\xEF\xBF\xBD" "b");
}

void testColors() {
    // names: all 148, any case
    CHECK(color("red") == rgba(255, 0, 0));
    CHECK(color("RebeccaPurple") == rgba(0x66, 0x33, 0x99));
    CHECK(color("lightgoldenrodyellow") == rgba(0xfa, 0xfa, 0xd2));
    CHECK(color("grey") == color("gray") && color("gray") == rgba(128, 128, 128));
    CHECK(color("yellowgreen") == rgba(0x9a, 0xcd, 0x32));
    CHECK(color("aliceblue") == rgba(0xf0, 0xf8, 0xff));
    CHECK(color("transparent") == rgba(0, 0, 0, 0));
    CHECK(!isColor("blu"));
    CHECK(!isColor("currentColor"));  // (only where a node gives it)
    CHECK(!isColor(""));
    CHECK(!isColor("red blue"));
    // hex: 3, 4, 6, 8 digits
    CHECK(color("#f80") == rgba(255, 0x88, 0));
    CHECK(color("#f808") == rgba(255, 0x88, 0, 0x88));
    CHECK(color("#1A2b3C") == rgba(0x1a, 0x2b, 0x3c));
    CHECK(color("#1a2b3c80") == rgba(0x1a, 0x2b, 0x3c, 0x80));
    CHECK(!isColor("#12") && !isColor("#12345") && !isColor("#1234567") && !isColor("#ggg"));
    CHECK(hex(rgba(1, 2, 255)) == "#0102ff");
    CHECK(hex(rgba(1, 2, 255, 16)) == "#0102ff10");
    // rgb(): commas or spaces, numbers or %, alpha after ',' or '/'
    CHECK(color("rgb(10, 20, 30)") == rgba(10, 20, 30));
    CHECK(color("rgb(10 20 30)") == rgba(10, 20, 30));
    CHECK(color("rgba(10, 20, 30, 0.5)") == rgba(10, 20, 30, 128));  // (127.5: halves to even)
    CHECK(color("rgb(10 20 30 / 25%)") == rgba(10, 20, 30, 64));
    CHECK(color("rgb(100%, 50%, 0%)") == rgba(255, 128, 0));
    CHECK(color("RGB(300, -5, 127.5)") == rgba(255, 0, 128));  // (clamped; rounded)
    CHECK(color("rgb(1 2 3 / 2)") == rgba(1, 2, 3, 255));
    CHECK(color("rgba(1 2 3)") == rgba(1, 2, 3));
    CHECK(color("rgb(none 2 3)") == rgba(0, 2, 3));
    CHECK(!isColor("rgb(1, 2)") && !isColor("rgb(1, 2, 3, 4, 5)") && !isColor("rgb(1 2 3 /)") && !isColor("rgb(1,2,3"));
    CHECK(!isColor("rgb(1px, 2, 3)") && !isColor("rgb(1 2 / 3 4)"));
    // hsl()
    CHECK(color("hsl(0, 100%, 50%)") == rgba(255, 0, 0));
    CHECK(color("hsl(120 100% 50%)") == rgba(0, 255, 0));
    CHECK(color("hsl(240deg 100% 50% / 50%)") == rgba(0, 0, 255, 128));
    CHECK(color("hsla(0.5turn, 100%, 25%, 1)") == rgba(0, 128, 128));  // (127.5 to even)
    CHECK(color("hsl(-120 100% 50%)") == rgba(0, 0, 255));
    CHECK(color("hsl(30 50% 60%)") == rgba(204, 153, 102));
    CHECK(color("hsl(3.14159265rad 100% 50%)") == rgba(0, 255, 255));
    CHECK(color("hsl(0 0% 100%)") == rgba(255, 255, 255));
    CHECK(!isColor("hsl(0, 100%)") && !isColor("hsl(0px, 1%, 1%)"));
    // color-mix(): CSS Color 5's percentages, mixColor's arithmetic
    CHECK(color("color-mix(in srgb, red 50%, blue)") == rgba(128, 0, 128));
    CHECK(color("color-mix(in srgb, red, blue)") == rgba(128, 0, 128));
    CHECK(color("color-mix(in srgb, 50% red, blue 50%)") == rgba(128, 0, 128));
    CHECK(color("color-mix(in srgb, red 25%, blue)") == rgba(64, 0, 191));
    CHECK(color("color-mix(in srgb, red, blue 75%)") == rgba(64, 0, 191));
    CHECK(color("color-mix(in srgb, red 20%, blue 20%)") == rgba(128, 0, 128, 102));  // (sum 40%: alpha 0.4)
    CHECK(color("color-mix(in srgb, red 60%, blue 60%)") == rgba(128, 0, 128));       // (sum 120%: scaled)
    CHECK(color("color-mix(in srgb, transparent, blue)") == rgba(0, 0, 255, 128));    // (premultiplied)
    CHECK(color("color-mix(in srgb, #ff000080 50%, #0000ff80)") == rgba(128, 0, 128, 128));
    CHECK(color("color-mix(in srgb, white 90%, black)") == rgba(229, 229, 229));
    CHECK(color("color-mix(in srgb, color-mix(in srgb, red, blue), white)") == rgba(192, 128, 192));
    CHECK(color("color-mix(in srgb, rgb(0 0 0), hsl(0 0% 100%) 100%)") == rgba(255, 255, 255));
    CHECK(!isColor("color-mix(in srgb, red 0%, blue 0%)"));
    CHECK(!isColor("color-mix(in srgb, red 150%, blue)"));
    CHECK(!isColor("color-mix(in hsl, red, blue)"));
    CHECK(!isColor("color-mix(red, blue)"));
    CHECK(!isColor("color-mix(in srgb, red, blue, green)"));
    CHECK(!isColor("color-mix(in srgb, red 10% 20%, blue)"));

    // the same shades as the program's mixColor(), byte for byte
    std::mt19937 rng(12345);
    int same = 0, tries = 4000;
    const float shares[] = {0.10f, 0.25f, 0.5f, 0.3f, 0.07f, 0.66f, 0.9f, 0.15f};
    for (int i = 0; i < tries; i++) {
        Color a(rng() & 255, rng() & 255, rng() & 255), b(rng() & 255, rng() & 255, rng() & 255);
        float t = shares[i % 8];
        int p = (int)std::lround((1 - t) * 100);  // A's share in %
        char text[160];
        snprintf(text, sizeof text, "color-mix(in srgb, rgb(%d %d %d) %d%%, #%02x%02x%02x)", a.r, a.g, a.b, p, b.r, b.g, b.b);
        Color m = mixColor(a, b, (float)((100 - p) / 100.0));
        Rgba c = color(text);
        if (c == rgba(m.r, m.g, m.b, m.a)) same++;
    }
    CHECK(same == tries);
    // and t = 0.10f exactly as the skins' 90% shades
    int same10 = 0;
    for (int i = 0; i < 2000; i++) {
        Color a(rng() & 255, rng() & 255, rng() & 255), b(rng() & 255, rng() & 255, rng() & 255);
        Color m = mixColor(a, b, 0.10f);
        Rgba c = color("color-mix(in srgb, " + hex(rgba(a.r, a.g, a.b)) + " 90%, " + hex(rgba(b.r, b.g, b.b)) + ")");
        if (c == rgba(m.r, m.g, m.b)) same10++;
    }
    CHECK(same10 == 2000);
}

void testSelectors() {
    Sheet s(
        "Button { color: red; padding: 1px }\n"
        "* { z-index: 300 }\n"
        ".accent { color: green }\n"
        "Button.accent { color: blue }\n"
        "#play { color: yellow }\n"
        "Button { color: black }\n"              // (later, same specificity as the first: wins over it)
        "Button:hover { color: orange }\n"
        "Button:hover:pressed { color: purple }\n"
        "Panel Button { margin-top: 3px }\n"
        "Panel > Button { margin-left: 4px }\n"
        "Slider::handle { color: white }\n"
        "::thumb { color: silver }\n"
        "Slider::handle:hover { color: pink }\n"
        "Lane, Clip { font-style: italic }\n"
        "Button { font-family: Mono !important }\n"
        "#play { font-family: Sans }\n"
        ":root { --x: 1 }\n");
    CHECK(s.problems.empty());
    SimpleNode win("Window");
    SimpleNode panel("Panel", &win);
    SimpleNode box("Box", &panel);
    SimpleNode b("Button", &box);
    Style st = s.engine.compute(b);
    CHECK(st.raw("color") == "black");
    CHECK(st.raw("z-index") == "300");
    CHECK(st.raw("margin-top") == "3px");   // (a descendant)
    CHECK(!st.has("margin-left"));           // (not a child of Panel)
    SimpleNode b2("Button", &panel);
    CHECK(s.engine.compute(b2).raw("margin-left") == "4px");
    b.classes = {"accent"};
    CHECK(s.engine.compute(b).raw("color") == "blue");
    b.states = {"hover"};
    CHECK(s.engine.compute(b).raw("color") == "orange");  // (Button:hover and Button.accent: equal, the later wins)
    b.classes.clear();
    CHECK(s.engine.compute(b).raw("color") == "orange");
    b.states = {"hover", "pressed"};
    CHECK(s.engine.compute(b).raw("color") == "purple");
    b.idName = "play";
    CHECK(s.engine.compute(b).raw("color") == "yellow");
    CHECK(s.engine.compute(b).raw("font-family") == "Mono");  // (!important beats the id)
    // a type it counts as
    SimpleNode tool("ToolButton", &panel);
    CHECK(!s.engine.compute(tool).has("padding-top"));
    tool.types = {"Button"};
    CHECK(s.engine.compute(tool).raw("padding-top") == "1px");
    // parts: only with the part asked; part-less rules never match a part
    SimpleNode slider("Slider", &panel);
    CHECK(!s.engine.compute(slider).has("color"));
    CHECK(s.engine.compute(slider, nullptr, "handle").raw("color") == "white");
    CHECK(!s.engine.compute(slider, nullptr, "handle").has("z-index"));  // (* has no part)
    CHECK(s.engine.compute(slider, nullptr, "thumb").raw("color") == "silver");
    CHECK(!s.engine.compute(slider, nullptr, "groove").has("color"));
    slider.states = {"hover"};
    CHECK(s.engine.compute(slider, nullptr, "handle").raw("color") == "pink");
    CHECK(s.engine.compute(slider).raw("color").empty());
    // lists
    CHECK(s.engine.compute(SimpleNode("Lane")).raw("font-style") == "italic");
    CHECK(s.engine.compute(SimpleNode("Clip")).raw("font-style") == "italic");
    CHECK(!s.engine.compute(SimpleNode("Track")).has("font-style"));
    // :root only on a node without a parent
    CHECK(s.engine.root().raw("--x") == "1");

    // specificity: id > class/state > type, whatever the order
    Sheet sp(
        "#a { color: red }\n"
        ".c.d { color: green }\n"
        "T.c { color: blue }\n"
        "T { color: black }\n");
    SimpleNode n("T");
    n.idName = "a";
    n.classes = {"c", "d"};
    CHECK(sp.engine.compute(n).raw("color") == "red");
    n.idName.clear();
    CHECK(sp.engine.compute(n).raw("color") == "green");  // (0,2,0 beats 0,1,1)
    n.classes = {"c"};
    CHECK(sp.engine.compute(n).raw("color") == "blue");
    // !important: beats any specificity; among importants, specificity then order
    Sheet imp(
        "T { color: red !important; width: 1px ! IMPORTANT }\n"
        "#i { color: blue }\n"
        "T.c { color: green !important }\n"
        "T { color: white !important }\n");
    SimpleNode m("T");
    m.idName = "i";
    CHECK(imp.engine.compute(m).raw("color") == "white");
    CHECK(imp.engine.compute(m).raw("width") == "1px");
    m.classes = {"c"};
    CHECK(imp.engine.compute(m).raw("color") == "green");

    // the order of sheets: later wins at equal specificity, not over higher specificity
    std::vector<Problem> ps;
    auto defaults = parse("T { color: red; margin-top: 1px } T.c { padding-top: 9px }", "defaults.css", ps);
    auto skin = parse("T { color: blue } .c { padding-top: 2px }", "skin.css", ps);
    Engine e;
    int g0 = e.generation();
    e.setSheets({defaults, skin});
    CHECK(e.generation() != g0);
    SimpleNode t("T");
    t.classes = {"c"};
    Style r = e.compute(t);
    CHECK(r.raw("color") == "blue");
    CHECK(r.raw("margin-top") == "1px");
    CHECK(r.raw("padding-top") == "9px");
    e.setSheets({skin, defaults});
    CHECK(e.compute(t).raw("color") == "red");
    CHECK(e.sheets().size() == 2);

    // selectors that can't be read drop their rule (the whole list), and nothing else
    Sheet bad(
        "A | B { color: red }\n"
        "A, :has(B) { color: red }\n"
        "A::x B { color: red }\n"
        "A::x.c { color: red }\n"
        "#1a { color: red }\n"
        "A > { color: red }\n"
        ". { color: red }\n"
        ", A { color: red }\n"
        "A { color: green }\n");
    CHECK(bad.problems.size() == 8);
    CHECK(bad.reported("unexpected '|' in a selector"));
    CHECK(bad.reported("':has()' is not supported"));
    CHECK(bad.reported("must be on the last compound"));
    CHECK(bad.engine.compute(SimpleNode("A")).raw("color") == "green");

    // long selectors on a deep tree stay quick (a descendant search that fails gives up at once)
    std::string sel;
    for (int i = 0; i < 30; i++) sel += "D ";
    Sheet deep("X " + sel + "{ color: red }");
    std::vector<std::unique_ptr<SimpleNode>> chain;
    const Node* up = nullptr;
    for (int i = 0; i < 200; i++) {
        chain.push_back(std::make_unique<SimpleNode>("D", up));
        up = chain.back().get();
    }
    auto t0 = std::chrono::steady_clock::now();
    CHECK(!deep.engine.compute(*chain.back()).has("color"));
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.5);
}

// a style that sets no custom property shares its parent's (thousands of widgets, one map, not a copy each);
// one that sets its own gets a copy, and the parent's stays as it was
void testVariablesShared() {
    Sheet s(":root { --fg: red; --pad: 4px }\nPanel { --fg: blue }\nButton { color: var(--fg) }\n");
    CHECK(s.problems.empty());
    const Style& root = s.engine.root();
    SimpleNode top("Window");
    SimpleNode panel("Panel", &top);
    SimpleNode button("Button", &panel);
    Style ps = s.engine.compute(panel);
    Style bs = s.engine.compute(button, &ps);
    CHECK(&bs.variables() == &ps.variables());
    CHECK(&ps.variables() != &root.variables());
    CHECK(ps.variable("--fg") == std::string("blue"));
    CHECK(root.variable("--fg") == std::string("red"));
    CHECK(bs.variable("--pad") == std::string("4px"));
    CHECK(bs.raw("color") == "blue");
    Style copy = bs;  // (a copy of a style shares them too)
    CHECK(&copy.variables() == &bs.variables());
}

// lengthText reads plain pixel values ("12px", the form computed styles keep their lengths in) without the
// tokenizer; every value, plain or not, must read exactly as the tokenizer reads it
void testLengthTextExact() {
    using namespace css::detail;
    struct Case {
        const char* text;
        bool ok;
        double px;
    };
    const Case cases[] = {{"12px", true, 12},      {"12.5px", true, 12.5},    {"-3px", true, -3},    {"+4px", true, 4},
                          {"0", true, 0},          {"0px", true, 0},          {"1e2px", true, 100},  {"12PX", true, 12},
                          {"12 px", false, 0},     {" 4px", true, 4},         {"4px ", true, 4},     {".5px", true, 0.5},
                          {"5.px", false, 0},      {"007px", true, 7},        {"-0px", true, -0.0},  {"12", false, 0},
                          {"1em", true, 10},       {"calc(2px + 3px)", true, 5}, {"", false, 0}, {"px", false, 0},
                          {"-px", false, 0},       {"1.2.3px", false, 0},     {"nanpx", false, 0},   {"1e400px", true, 1e300},
                          {"50%", true, 100}};
    LenCtx ctx;
    ctx.em = 10;
    ctx.percentOf = 200;
    for (const Case& c : cases) {
        double px = -999;
        bool ok = lengthText(c.text, ctx, px);
        CHECK(ok == c.ok);
        if (ok && c.ok) CHECK(px == c.px && std::signbit(px) == std::signbit(c.px));
    }
    // plain values read the same as through the tokenizer, to the last bit
    for (const char* text : {"0.1px", "12.345px", "1234567.891px", "-0.3px", "99999999999px", "3.14159px"}) {
        double plain = -1, tokenized = -2;
        CHECK(lengthText(text, ctx, plain));
        Tokens tokens;
        tokenize(text, tokens);
        CHECK(evalLength(tokens, 0, tokens.size(), ctx, tokenized));
        CHECK(plain == tokenized);
    }
}

// a value with var() is worked out once per declaration and set of variables, not again for every node that
// uses it; other variables, or new sheets, work it out again
void testVarValuesCached() {
    Sheet s(":root { --fg: red; --pad: 4px }\nPanel { --fg: blue }\nButton { color: var(--fg); padding: var(--pad) 2px }\n");
    CHECK(s.problems.empty());
    SimpleNode top("Window");
    SimpleNode panel("Panel", &top);
    SimpleNode firstButton("Button", &panel), secondButton("Button", &panel), buttonOnTop("Button", &top);
    Style panelStyle = s.engine.compute(panel);
    Style first = s.engine.compute(firstButton, &panelStyle);
    size_t hitsBefore = s.engine.varCacheHits();
    Style second = s.engine.compute(secondButton, &panelStyle);
    CHECK(s.engine.varCacheHits() == hitsBefore + 5);  // (color and padding's four sides: the first button's)
    CHECK(first.raw("color") == "blue" && second.raw("color") == "blue");
    CHECK(second.raw("padding-top") == "4px" && second.raw("padding-left") == "2px");
    Style onTop = s.engine.compute(buttonOnTop);  // (the root's variables: worked out for them)
    CHECK(onTop.raw("color") == "red");
    std::vector<Problem> problems;
    s.engine.setSheets({parse(":root { --fg: green }\nButton { color: var(--fg) }\n", "new.css", problems)});
    CHECK(s.engine.compute(buttonOnTop).raw("color") == "green");  // (new sheets: nothing kept from the old ones)
}

void testVariablesAndInheritance() {
    Sheet s(
        ":root { --fg: #102030; --pad: 4px; --both: var(--pad) 6px; --a: var(--b); --b: var(--a); --c: var(--a, 7px);"
        "  color: var(--fg); font-size: 10px; --lane: red }\n"
        "Panel { --fg: blue; background-color: green; font-size: 2em; letter-spacing: 0.1em }\n"
        "Button { padding: var(--both); margin: var(--nope, var(--pad)); border-color: currentColor }\n"
        "Label { color: var(--undefined); width: var(--a, 3px); height: var(--a) }\n"
        "Text { font-size: 150%; padding-left: 1em; margin-left: 9pt }\n"
        "Text.big { font-size: calc(1em + 2px) }\n"
        "Hint { color: color-mix(in srgb, currentColor 50%, white); outline-color: currentColor }\n"
        "Deep { --x: var(--y); --y: var(--z); --z: 1px; padding-top: var(--x) }\n");
    CHECK(s.problems.empty());
    const Style& root = s.engine.root();
    CHECK(root.raw("color") == "#102030");
    CHECK(root.variable("--pad") == std::string("4px"));
    CHECK(root.variable("pad") == std::string("4px"));
    CHECK(root.variable("--both") == std::string("4px 6px"));
    CHECK(!root.variable("--a") && !root.variable("--b"));  // (a cycle: both invalid)
    CHECK(root.variable("--c") == std::string("7px"));       // (its fallback)
    CHECK(root.variables().count("--lane") == 1);
    std::vector<Problem> found = s.engine.takeProblems();
    CHECK(anyProblem(found, "cycle"));
    CHECK(found.size() == 2);  // (--a and --b, once each)
    CHECK(s.engine.takeProblems().empty());
    CHECK(root.fontSizePx(0) == 10);

    SimpleNode top("Window");  // (a node without a parent is a :root itself)
    SimpleNode panel("Panel", &top);
    Style ps = s.engine.compute(panel);  // (no parent style: the root's)
    CHECK(ps.raw("color") == "#102030");  // (inherited; var() was resolved where it was declared)
    CHECK(ps.raw("--fg") == "blue");
    CHECK(ps.raw("background-color") == "green");
    CHECK_NEAR(ps.fontSizePx(0), 20, 1e-9);  // (2em of the root's 10px)
    CHECK(ps.raw("font-size") == "20px");
    CHECK(ps.raw("letter-spacing") == "2px");  // (em of its own font size, made px to inherit)

    SimpleNode button("Button", &panel);
    Style bs = s.engine.compute(button, &ps);
    CHECK(!bs.has("background-color"));  // (not inherited)
    CHECK(bs.raw("color") == "#102030");
    CHECK(bs.raw("letter-spacing") == "2px");
    CHECK_NEAR(bs.fontSizePx(0), 20, 1e-9);
    Edges pad = bs.edges("padding", Edges{});
    CHECK(pad.top == 4 && pad.right == 6 && pad.bottom == 4 && pad.left == 6);
    Edges mar = bs.edges("margin", Edges{});
    CHECK(mar.top == 4 && mar.left == 4);
    CHECK(bs.raw("border-top-color") == "currentColor");
    CHECK(bs.color("border-top-color", Rgba{}) == rgba(0x10, 0x20, 0x30));

    SimpleNode label("Label", &panel);
    Style ls = s.engine.compute(label, &ps);
    CHECK(ls.raw("color") == "#102030");  // (var() with no value: unset, so inherited)
    CHECK(ls.raw("width") == "3px");
    CHECK(!ls.has("height"));
    found = s.engine.takeProblems();
    CHECK(anyProblem(found, "var(--undefined) has no value"));
    CHECK(anyProblem(found, "var(--a) has no value"));
    for (const Problem& p : found) CHECK(p.source == "test.css" && p.line == 4 && p.column > 1);

    SimpleNode text("Text", &panel);
    Style ts = s.engine.compute(text, &ps);
    CHECK_NEAR(ts.fontSizePx(0), 30, 1e-9);  // (150% of 20px)
    CHECK_NEAR(ts.length("padding-left", 0), 30, 1e-9);  // (em: its own font size)
    CHECK_NEAR(ts.length("margin-left", 0), 12, 1e-9);   // (9pt)
    text.classes = {"big"};
    CHECK_NEAR(s.engine.compute(text, &ps).fontSizePx(0), 22, 1e-9);

    SimpleNode hint("Hint", &panel);
    Style hs = s.engine.compute(hint, &ps);
    CHECK(hs.color("color", Rgba{}) == rgba(136, 144, 152));  // (the parent's colour, mixed)
    CHECK(hs.raw("color") == "#889098");
    CHECK(hs.color("outline-color", Rgba{}) == rgba(136, 144, 152));

    Style ds = s.engine.compute(SimpleNode("Deep"));
    CHECK(ds.raw("padding-top") == "1px");
    CHECK(ds.raw("--x") == "1px");

    // var() in a shorthand: expanded on the node; a bad result leaves its longhands unset
    Sheet sh(":root { --w: 2px; --bad: 1px 2px 3px 4px 5px }\n"
             "A { border: var(--w) solid; padding: var(--bad); background: var(--bg, url(x.png) #fff) }\n");
    Style as = sh.engine.compute(SimpleNode("A"));
    CHECK(as.raw("border-left-width") == "2px" && as.raw("border-left-style") == "solid");
    CHECK(as.raw("border-left-color") == "currentcolor");
    CHECK(!as.has("padding-top"));
    CHECK(anyProblem(sh.engine.takeProblems(), "takes 1 to 4 values"));
    CHECK(as.raw("background") == "url(x.png) #fff");
    CHECK(as.color("background-color", Rgba{}) == rgba(255, 255, 255));

    // a typed property's value checked after var()
    Sheet ty(":root { --n: 12px } A { color: var(--n); background-color: red; background-color: var(--n) }");
    Style tys = ty.engine.compute(SimpleNode("A"));
    CHECK(!tys.has("color"));
    CHECK(!tys.has("background-color"));  // (invalid at computed-value time: unset, not the earlier red)
    CHECK(anyProblem(ty.engine.takeProblems(), "is not a colour"));

    // inherit / initial / unset
    Sheet kw("A { color: red; padding-top: 1px } B { color: initial; padding-top: inherit } C { color: unset }");
    SimpleNode a("A");
    Style sa = kw.engine.compute(a);
    SimpleNode b("B", &a), c("C", &a);
    CHECK(!kw.engine.compute(b, &sa).has("color"));
    CHECK(kw.engine.compute(b, &sa).raw("padding-top") == "1px");
    CHECK(kw.engine.compute(c, &sa).raw("color") == "red");

    // an exponential var() blow-up is cut off, not computed
    std::string boom = ":root { --v0: x";
    for (int i = 1; i < 40; i++) boom += "; --v" + std::to_string(i) + ": var(--v" + std::to_string(i - 1) + ") var(--v" + std::to_string(i - 1) + ")";
    boom += "; width: var(--v39) }";
    Sheet bm(boom);
    CHECK(!bm.engine.root().has("width"));
    CHECK(anyProblem(bm.engine.takeProblems(), "too long"));
}

void testValues() {
    Sheet s(
        "A {\n"
        "  width: calc(10px + 2em); height: calc(100% - 4px); min-width: calc(2 * 3px); max-width: calc((1px + 2px) * 2);\n"
        "  left: calc(1px + 2); top: calc(1px+2px); right: calc(1px / 0); bottom: calc(6px / 4);\n"
        "  opacity: 50%; flex: calc(1 / 4); z-index: 3; font-weight: bold; line-height: calc(1px * 2px);\n"
        "  font-family: 'DejaVu Sans', \"Noto, Sans\", monospace;\n"
        "  text-transform: UpperCase; content: 'x';\n"
        "  border-radius: 1px 2px; gap: 0; outline-width: thick;\n"
        "}\n");
    CHECK(s.problems.empty());
    Style st = s.engine.compute(SimpleNode("A"));
    CHECK_NEAR(st.length("width", -1), 34, 1e-9);  // (em: 12px by default)
    CHECK_NEAR(st.length("height", -1, 50), 46, 1e-9);
    CHECK_NEAR(st.length("height", -1), -4, 1e-9);
    CHECK_NEAR(st.length("min-width", -1), 6, 1e-9);
    CHECK_NEAR(st.length("max-width", -1), 6, 1e-9);
    CHECK(st.length("left", -1) == -1);    // (a length plus a number)
    CHECK(st.length("top", -1) == -1);     // (+ needs spaces around it)
    CHECK(st.length("right", -1) == -1);   // (division by zero)
    CHECK_NEAR(st.length("bottom", -1), 1.5, 1e-9);
    CHECK(st.length("line-height", -1) == -1);
    CHECK(st.length("gap", -1) == 0);
    CHECK(st.length("outline-width", -1) == 5);
    CHECK(st.length("missing", 7) == 7);
    CHECK_NEAR(st.number("opacity", -1), 0.5, 1e-9);
    CHECK_NEAR(st.number("flex", -1), 0.25, 1e-9);
    CHECK(st.number("z-index", -1) == 3);
    CHECK(st.number("font-weight", -1) == 700);
    CHECK(st.number("width", -1) == -1);
    CHECK(st.keyword("text-transform", "") == "uppercase");
    CHECK(st.keyword("missing", "def") == "def");
    auto fam = st.list("font-family");
    CHECK(fam.size() == 3 && fam[0] == "DejaVu Sans" && fam[1] == "Noto, Sans" && fam[2] == "monospace");
    CHECK(st.raw("content") == "'x'");
    Edges rd = st.radii(Edges{});
    CHECK(rd.top == 1 && rd.right == 2 && rd.bottom == 1 && rd.left == 2);
    CHECK(st.color("missing", rgba(1, 1, 1)) == rgba(1, 1, 1));
    CHECK(st.color("width", rgba(1, 1, 1)) == rgba(1, 1, 1));
    CHECK(st.fontSizePx(11) == 11);
}

void testShorthands() {
    Sheet s(
        "P1 { padding: 1px }\n"
        "P2 { padding: 1px 2px }\n"
        "P3 { padding: 1px 2px 3px }\n"
        "P4 { padding: 1px 2px 3px 4px; margin: auto 5% }\n"
        "R1 { border-radius: 3px }\n"
        "R3 { border-radius: 1px 2px 3px }\n"
        "R4 { border-radius: 1px 2px 3px 4px }\n"
        "B1 { border: red 2px dashed }\n"
        "B2 { border: solid }\n"
        "B3 { border-top: thin #00f; border-width: 1px 2px; border-style: none solid; border-color: red green blue }\n"
        "BG { background: url(a.png) no-repeat rgb(1 2 3) }\n"
        "BN { background: none }\n"
        "X { padding: 1px 2px 3px 4px 5px }\n"
        "X { margin: red }\n"
        "X { border: 1px 2px }\n"
        "X { border: solid dashed }\n"
        "X { border-radius: 1px / 2px }\n"
        "X { padding: -1px }\n"
        "X { padding: inherit }\n");
    CHECK(s.problems.size() == 6);
    auto edges = [&](const char* type, const char* prefix) { return s.engine.compute(SimpleNode(type)).edges(prefix, Edges{-1, -1, -1, -1}); };
    auto same = [](Edges e, double t, double r, double b, double l) { return e.top == t && e.right == r && e.bottom == b && e.left == l; };
    CHECK(same(edges("P1", "padding"), 1, 1, 1, 1));
    CHECK(same(edges("P2", "padding"), 1, 2, 1, 2));
    CHECK(same(edges("P3", "padding"), 1, 2, 3, 2));
    CHECK(same(edges("P4", "padding"), 1, 2, 3, 4));
    Style p4 = s.engine.compute(SimpleNode("P4"));
    CHECK(p4.raw("margin-top") == "auto" && p4.raw("margin-left") == "5%");
    CHECK(p4.length("margin-left", 0, 200) == 10);
    CHECK(!p4.has("padding"));
    auto radii = [&](const char* type) { return s.engine.compute(SimpleNode(type)).radii(Edges{}); };
    CHECK(same(radii("R1"), 3, 3, 3, 3));
    CHECK(same(radii("R3"), 1, 2, 3, 2));
    CHECK(same(radii("R4"), 1, 2, 3, 4));
    Style b1 = s.engine.compute(SimpleNode("B1"));
    CHECK(same(b1.edges("border-width", Edges{}), 2, 2, 2, 2));
    CHECK(b1.keyword("border-left-style", "") == "dashed");
    CHECK(b1.color("border-bottom-color", Rgba{}) == rgba(255, 0, 0));
    Style b2 = s.engine.compute(SimpleNode("B2"));
    CHECK(b2.length("border-top-width", 0) == 3);  // (medium)
    CHECK(b2.raw("border-top-color") == "currentcolor");
    Style b3 = s.engine.compute(SimpleNode("B3"));
    CHECK(same(b3.edges("border-width", Edges{}), 0, 2, 0, 2));  // (a side whose style is none has no width)
    CHECK(b3.raw("border-top-style") == "none" && b3.raw("border-right-style") == "solid");
    CHECK(b3.raw("border-top-color") == "red" && b3.raw("border-right-color") == "green" &&
          b3.raw("border-bottom-color") == "blue" && b3.raw("border-left-color") == "green");
    Style bg = s.engine.compute(SimpleNode("BG"));
    CHECK(bg.raw("background") == "url(a.png) no-repeat rgb(1 2 3)");
    CHECK(bg.color("background-color", Rgba{}) == rgba(1, 2, 3));
    CHECK(s.engine.compute(SimpleNode("BN")).color("background-color", rgba(9, 9, 9)) == rgba(0, 0, 0, 0));
    Style x = s.engine.compute(SimpleNode("X"));
    CHECK(x.raw("padding-top") == "inherit" || !x.has("padding-top"));  // (inherit: the parent has none)
    CHECK(!x.has("margin-top") && !x.has("border-top-width") && !x.has("border-top-left-radius"));
}

void testRecovery() {
    const char* text =
        "@charset \"utf-8\";\n"                                // 1 (silently)
        "@supports (x) { A { color: red } }\n"                 // 2
        "A { color: blu; color: green; width 3px; height: 2px }\n"  // 3
        "B { nested { color: red } padding-top: 1px; ; ; }\n"  // 4
        "C { color: 'oops\n"                                   // 5 (a bad string: to the next ';')
        "; margin-top: 4px }\n"                                // 6
        "D { --empty:; --keep: { a b } ; font: 12px/1 'X' }\n" // 7
        "E { color: red; ) ; width: 1px }\n"                   // 8
        "F{color:#abc}G{color:#def} H { unknown-prop: anything(1) ; color: rgb(1 2) }\n"  // 9
        "\xC3\xA9t\xC3\xA9 { @foo bar; z: 1 }\n"               // 10
        "J { width: 1px) }\n"
        "I { color: red\n";                                     // 11 (no '}' at the end)
    std::vector<Problem> ps;
    auto sheet = parse(text, "skin.css", ps);
    Engine e;
    e.setSheets({sheet});
    auto at = [&](const std::string& what, int line, int col) {
        for (const Problem& p : ps)
            if (p.message.find(what) != std::string::npos) return p.line == line && p.column == col;
        return false;
    };
    CHECK(at("@supports", 2, 1));
    CHECK(at("unknown colour 'blu'", 3, 5));
    CHECK(at("expected ':' after 'width'", 3, 31));
    CHECK(at("nested rules", 4, 5));
    CHECK(at("unterminated string", 5, 12));
    CHECK(at("broken value for 'color'", 5, 5));
    CHECK(at("expected a declaration, not ')'", 8, 17));
    CHECK(at("a bad value", 1000, 1000) == false);
    CHECK(at("'rgb(1 2)' is not a colour", 9, 60));
    CHECK(at("@foo", 10, 7));  // (columns count characters: é is one)
    CHECK(at("unmatched ')' in the value of 'width'", 11, 15));
    CHECK(at("missing '}'", 13, 1));
    CHECK(!anyProblem(ps, "charset"));
    for (const Problem& p : ps) CHECK(p.source == "skin.css");
    CHECK(ps.front().text() == "skin.css:2:1: @supports rules are not supported (skipped)");  // (in source order)
    for (size_t i = 1; i < ps.size(); i++) CHECK(ps[i - 1].line <= ps[i].line);
    CHECK(ps.size() == 11);

    Style a = e.compute(SimpleNode("A"));
    CHECK(a.raw("color") == "green");
    CHECK(a.raw("height") == "2px");
    CHECK(!a.has("width"));
    Style b = e.compute(SimpleNode("B"));
    CHECK(b.raw("padding-top") == "1px" && !b.has("color"));
    Style c = e.compute(SimpleNode("C"));
    CHECK(!c.has("color") && c.raw("margin-top") == "4px");
    Style d = e.compute(SimpleNode("D"));
    CHECK(d.has("--empty") && d.raw("--empty").empty());
    CHECK(d.raw("--keep") == "{ a b }");
    CHECK(d.raw("font") == "12px/1 'X'");  // (an unknown property, kept as written)
    Style ee = e.compute(SimpleNode("E"));
    CHECK(ee.raw("color") == "red" && ee.raw("width") == "1px");
    CHECK(e.compute(SimpleNode("F")).color("color", Rgba{}) == rgba(0xaa, 0xbb, 0xcc));
    CHECK(e.compute(SimpleNode("G")).color("color", Rgba{}) == rgba(0xdd, 0xee, 0xff));
    Style h = e.compute(SimpleNode("H"));
    CHECK(h.raw("unknown-prop") == "anything(1)" && !h.has("color"));
    CHECK(e.compute(SimpleNode("\xC3\xA9t\xC3\xA9")).raw("z") == "1");
    CHECK(e.compute(SimpleNode("I")).raw("color") == "red");

    // a rule with no block; a stray '}'
    std::vector<Problem> p2;
    Engine e2;
    e2.setSheets({parse("A { color: red } } B { color: blue } C { color: green } D", "x.css", p2)});
    CHECK(e2.compute(SimpleNode("A")).raw("color") == "red");
    CHECK(!e2.compute(SimpleNode("B")).has("color"));  // ("} B": not a selector, as in browsers)
    CHECK(e2.compute(SimpleNode("C")).raw("color") == "green");
    CHECK(anyProblem(p2, "without a { block"));
    // an empty stylesheet, an empty name
    std::vector<Problem> p3;
    CHECK(parse("", "", p3)->rules.empty() && p3.empty());
    Problem pr{"a.css", 3, 7, "msg"};
    CHECK(pr.text() == "a.css:3:7: msg");
    // an engine without sheets
    Engine none;
    CHECK(!none.compute(SimpleNode("A")).has("color"));
    CHECK(none.root().variables().empty());
}

// a parent's children, each knowing its place (as widgets do)
struct Family {
    SimpleNode parent;
    std::vector<std::unique_ptr<SimpleNode>> kids;
    std::vector<const Node*> list;
    explicit Family(const std::vector<std::string>& types, const Node* up = nullptr, const std::string& type = "List")
        : parent(type, up) {
        for (const std::string& t : types) kids.push_back(std::make_unique<SimpleNode>(t, &parent));
        for (auto& k : kids) list.push_back(k.get());
        for (size_t i = 0; i < kids.size(); i++) {
            kids[i]->family = &list;
            kids[i]->index = (int)i;
        }
    }
    SimpleNode& operator[](size_t i) { return *kids[i]; }
};

// :first-child, :last-child, :only-child, :nth-child(), :nth-last-child()
void testPositions() {
    Sheet s(
        "Item:first-child { --first: y }\n"
        "Item:last-child { --last: y }\n"
        "Item:only-child { --only: y }\n"
        "Item:nth-child(2n+1) { --odd: y }\n"
        "Item:nth-child( even ) { --even: y }\n"
        "Item:nth-child(-n + 3) { --upto3: y }\n"
        ":nth-child(3) { --third: y }\n"
        "Item:nth-child(n) { --any: y }\n"
        "Item:nth-child(3n-1) { --m: y }\n"
        "Item:nth-child(+n+4) { --from4: y }\n"
        "Item:nth-child(-2n+5) { --odd-upto5: y }\n"
        "Item:nth-child(0n+2) { --second: y }\n"
        "Item:nth-last-child(2) { --butlast: y }\n"
        "Item:nth-last-child(odd) { --oddend: y }\n"
        "Item:NTH-CHILD(ODD):First-Child { --both: y }\n");
    CHECK(s.problems.empty());
    CHECK(s.engine.uses() == Engine::UsesPosition);
    Family f({"Item", "Item", "Item", "Item", "Item"});
    auto has = [&](size_t i, const char* var) { return s.engine.compute(f[i]).has(var); };
    CHECK(f[0].childIndex() == 0 && f[4].childIndex() == 4 && f[2].siblingCount() == 5 && f[1].sibling(3) == &f[3]);
    CHECK(!f[1].sibling(5) && !f[1].sibling(-1) && f.parent.childIndex() == -1 && f.parent.siblingCount() == 0);
    const char* vars[] = {"--first", "--last", "--only", "--odd", "--even", "--upto3", "--third", "--any", "--m",
                          "--from4", "--odd-upto5", "--second", "--butlast", "--oddend", "--both"};
    // which of the five each matches
    const char* want[] = {"10000", "00001", "00000", "10101", "01010", "11100", "00100", "11111", "01001",
                          "00011", "10101", "01000", "00010", "10101", "10000"};
    for (size_t v = 0; v < sizeof vars / sizeof vars[0]; v++)
        for (size_t i = 0; i < 5; i++) CHECK(has(i, vars[v]) == (want[v][i] == '1'));
    Family one({"Item"});
    Style o = s.engine.compute(one[0]);
    CHECK(o.has("--first") && o.has("--last") && o.has("--only") && o.has("--odd") && !o.has("--even"));
    // a node whose place isn't known matches none of them (nor :nth-child(n))
    SimpleNode alone("Item");
    Style a = s.engine.compute(alone);
    for (const char* v : vars) CHECK(!a.has(v));
    CHECK(s.engine.root().variables().empty());

    // specificity: as a class
    Sheet sp(
        "Item:first-child { color: red }\n"
        ".c { color: blue }\n"
        ":first-child { width: 1px }\n"
        ".c { width: 2px }\n"
        ":nth-child(1) { height: 1px }\n"
        "Item { height: 2px }\n");
    f[0].classes = {"c"};
    Style st = sp.engine.compute(f[0]);
    CHECK(st.raw("color") == "red");   // (0,1,1) over (0,1,0)
    CHECK(st.raw("width") == "2px");   // (equal: the later)
    CHECK(st.raw("height") == "1px");  // (0,1,0) over (0,0,1)
    f[0].classes.clear();

    // arguments that can't be read
    Sheet bad(
        "Item:nth-child(2n+) { color: red }\n"
        "Item:nth-child(n2) { color: red }\n"
        "Item:nth-child(x) { color: red }\n"
        "Item:nth-child() { color: red }\n"
        "Item:nth-child(2n of .a) { color: red }\n"
        "Item:nth-of-type(1) { color: red }\n"
        "Item { color: green }\n");
    CHECK(bad.reported("a bad argument for :nth-child(): '2n+'"));
    CHECK(bad.reported("'n2'") && bad.reported("'x'") && bad.reported("''"));
    CHECK(bad.reported("':nth-child(… of …)' is not supported"));
    CHECK(bad.reported("':nth-of-type()' is not supported"));
    CHECK(bad.engine.compute(f[1]).raw("color") == "green");
    // huge numbers don't overflow
    Sheet huge("Item:nth-child(99999999999n+99999999999) { color: red } Item:nth-child(-99999999999n) { width: 1px }");
    CHECK(huge.problems.empty());
    CHECK(!huge.engine.compute(f[0]).has("color") && !huge.engine.compute(f[0]).has("width"));
}

// :not(), :is(), :where(): matching, specificity, nesting
void testNotIs() {
    Sheet s(
        "Item:not(.a) { --not-a: y }\n"
        "Item:not(.a, .b) { --neither: y }\n"
        "Item:is(.a, .b) { --either: y }\n"
        ":where(.a) { --where-a: y }\n"
        "Item:not(:is(.a, .b)) { --not-is: y }\n"
        "Item:not(:not(.c)) { --c: y }\n"
        ":is(Panel Item) { --in-panel: y }\n"
        "Item:not(Panel > *) { --not-child-of-panel: y }\n"
        ":is(:first-child, :last-child) { --end: y }\n"
        ":is(Item:hover, #x) { --hover-or-x: y }\n"
        ":is(Thing, Item).a { --typed: y }\n"
        ":not(:is(:not(:is(.a)))) { --deep-a: y }\n");
    CHECK(s.problems.empty());
    CHECK(s.engine.uses() == Engine::UsesPosition);  // (an argument's :first-child counts)
    SimpleNode panel("Panel");
    Family f({"Item", "Item", "Item"}, &panel, "Box");
    SimpleNode direct("Item", &panel);
    f[0].classes = {"a"};
    f[1].classes = {"b", "c"};
    auto has = [&](const Node& n, const char* var) { return s.engine.compute(n).has(var); };
    CHECK(!has(f[0], "--not-a") && has(f[1], "--not-a") && has(f[2], "--not-a"));
    CHECK(!has(f[0], "--neither") && !has(f[1], "--neither") && has(f[2], "--neither"));
    CHECK(has(f[0], "--either") && has(f[1], "--either") && !has(f[2], "--either"));
    CHECK(has(f[0], "--where-a") && !has(f[1], "--where-a"));
    CHECK(!has(f[0], "--not-is") && !has(f[1], "--not-is") && has(f[2], "--not-is"));
    CHECK(!has(f[0], "--c") && has(f[1], "--c"));
    CHECK(has(f[0], "--in-panel") && has(direct, "--in-panel") && !has(SimpleNode("Item"), "--in-panel"));
    CHECK(has(f[0], "--not-child-of-panel") && !has(direct, "--not-child-of-panel"));
    CHECK(has(f[0], "--end") && !has(f[1], "--end") && has(f[2], "--end"));
    CHECK(!has(f[1], "--hover-or-x"));
    f[1].states = {"hover"};
    CHECK(has(f[1], "--hover-or-x"));
    f[2].idName = "x";
    CHECK(has(f[2], "--hover-or-x"));
    CHECK(has(f[0], "--typed") && !has(f[1], "--typed"));
    CHECK(has(f[0], "--deep-a") && !has(f[1], "--deep-a") && !has(f[2], "--deep-a"));

    // specificity: :not() and :is() take their most specific argument, :where() none
    Sheet sp(
        ":is(#i, .k) { color: red }\n"
        ".a.b.c { color: blue }\n"
        ":where(#i) { width: 1px }\n"
        "T { width: 2px }\n"
        ":not(#zz) { height: 1px }\n"
        ".a.b.c.a { height: 2px }\n"
        "T:not(.q):not(.r) { margin-top: 1px }\n"
        "T.a.b { margin-top: 2px }\n"
        ".a:where(.b, #i) { padding-top: 1px }\n"
        ".b { padding-top: 2px }\n"
        ":is(T) { z-index: 1 }\n"
        "T { z-index: 2 }\n"
        ":is(.a, T, #i) { opacity: 1 }\n"
        ".a.a.a.a { opacity: 0 }\n");
    CHECK(sp.problems.empty());
    SimpleNode t("T");
    t.idName = "i";
    t.classes = {"a", "b", "c"};
    Style st = sp.engine.compute(t);
    CHECK(st.raw("color") == "red");          // (1,0,0) whichever argument matched
    CHECK(st.raw("width") == "2px");          // (0,0,0) under (0,0,1)
    CHECK(st.raw("height") == "1px");         // (1,0,0) over (0,4,0)
    CHECK(st.raw("margin-top") == "2px");     // (0,2,1) both: the later
    CHECK(st.raw("padding-top") == "2px");    // (0,1,0) both
    CHECK(st.raw("z-index") == "2");          // (0,0,1) both
    CHECK(st.raw("opacity") == "1");          // (1,0,0)
    t.idName.clear();
    t.classes.push_back("k");
    CHECK(sp.engine.compute(t).raw("color") == "red");      // (still 1,0,0: .k matched, #i counts)
    CHECK(sp.engine.compute(t).raw("opacity") == "1");

    // what can't be read: the rule is dropped (the whole list), and nothing else
    std::string deep = ".a";
    for (int i = 0; i < 20; i++) deep = ":is(" + deep + ")";
    Sheet bad(
        "A:not() { color: red }\n"
        "A:is(.a,) { color: red }\n"
        "A:not(::part) { color: red }\n"
        "A:is(B::x) { color: red }\n"
        "A:not(> B) { color: red }\n"
        "A:is(B +) { color: red }\n"
        "A:where(#1) { color: red }\n"
        "A" + deep + " { color: red }\n"
        "A { color: green }\n"
        "A:is(.a { color: red }\n");
    CHECK(bad.problems.size() == 9);
    CHECK(bad.reported("an empty selector in a selector list"));
    CHECK(bad.reported("can't be inside :not(), :is() or :where()"));
    CHECK(bad.reported("unexpected '>' in a selector"));
    CHECK(bad.reported("a selector ends with '+'"));
    CHECK(bad.reported("nested too deeply"));
    CHECK(bad.reported("a rule without a { block"));
    SimpleNode an("A");
    an.classes = {"a"};
    CHECK(bad.engine.compute(an).raw("color") == "green");
    // 16 levels are fine
    deep = ".a";
    for (int i = 0; i < 16; i++) deep = ":is(" + deep + ")";
    Sheet ok("A" + deep + " { color: red }");
    CHECK(ok.problems.empty() && ok.engine.compute(an).raw("color") == "red");
}

// the sibling combinators A + B and A ~ B
void testSiblingCombinators() {
    Sheet s(
        "A + B { --next: y }\n"
        "A ~ B { --later: y }\n"
        "B+B { --bb: y }\n"
        "C ~ D { --cd: y }\n"
        "A + B + C { --abc: y }\n"
        "A ~ C ~ D { --acd: y }\n"
        "List > A ~ B { --in-list: y }\n"
        "Wrap A ~ D { --wrapped: y }\n"
        "A + B > X { --child-of-b: y }\n"
        "A ~ B X { --inside-b: y }\n"
        ".k ~ * { --after-k: y }\n"
        "B:not(A + B) { --b-not-after-a: y }\n");
    CHECK(s.problems.empty());
    CHECK(s.engine.uses() == Engine::UsesSiblings);
    SimpleNode wrap("Wrap");
    Family f({"A", "B", "C", "B", "D"}, &wrap);
    auto has = [&](const Node& n, const char* var) { return s.engine.compute(n).has(var); };
    const char* vars[] = {"--next", "--later", "--bb", "--cd", "--abc", "--acd", "--in-list", "--wrapped", "--b-not-after-a"};
    const char* want[] = {"01000", "01010", "00000", "00001", "00100", "00001", "01010", "00001", "00010"};
    for (size_t v = 0; v < sizeof vars / sizeof vars[0]; v++)
        for (size_t i = 0; i < 5; i++) CHECK(has(f[i], vars[v]) == (want[v][i] == '1'));
    f[2].classes = {"k"};
    CHECK(!has(f[2], "--after-k") && has(f[3], "--after-k") && has(f[4], "--after-k") && !has(f[1], "--after-k"));
    f[2].classes.clear();
    // into a sibling's children
    Family inB({"X", "Y"}, &f.parent, "B");
    inB.parent.family = &f.list;  // (it stands in for f[1]: the same place)
    inB.parent.index = 1;
    CHECK(has(inB[0], "--child-of-b") && has(inB[0], "--inside-b") && !has(inB[1], "--child-of-b"));
    // without a place (no siblings known): none
    SimpleNode a("A"), b("B");
    CHECK(!has(b, "--next") && !has(b, "--later"));

    // a + search that fails at the nearest ancestor goes on to the next one: B1 (after an A) > B2 (first) > C
    Sheet up("A + B C { color: red }");
    SimpleNode top("Top");
    Family outer({"A", "B"}, &top);
    Family inner({"B"}, &outer[1]);  // (outer[1] > List > B)
    Family leaf({"C"}, &inner[0]);
    CHECK(up.engine.compute(leaf[0]).raw("color") == "red");
    // likewise a ~ search: A ~ B C, where the nearest B has no A before it
    Sheet up2("A ~ B C { color: red } X ~ B C { width: 1px }");
    CHECK(up2.engine.compute(leaf[0]).raw("color") == "red");
    CHECK(!up2.engine.compute(leaf[0]).has("width"));

    // long ~ chains over many siblings stay quick (a search that ran out of siblings gives up at once)
    std::vector<std::string> many(400, "A");
    Family lots(many);
    std::string chain = "B";
    for (int i = 0; i < 12; i++) chain += " ~ A";
    std::string plus = "B";
    for (int i = 0; i < 12; i++) plus += " + A";
    Sheet longs(chain + " { color: red } " + plus + " { width: 1px } Z A ~ A ~ A ~ A ~ A { height: 1px }");
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < 400; i += 7) {
        Style st = longs.engine.compute(lots[i]);
        CHECK(!st.has("color") && !st.has("width") && !st.has("height"));
    }
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 0.5);
    lots[3].typeName = "B";
    CHECK(longs.engine.compute(lots[15]).raw("color") == "red");
    CHECK(longs.engine.compute(lots[15]).raw("width") == "1px");
    CHECK(!longs.engine.compute(lots[16]).has("width") && longs.engine.compute(lots[16]).has("color"));
    CHECK(!longs.engine.compute(lots[14]).has("color"));

    // specificity: combinators add nothing
    Sheet sp("A + B { color: red } B.x { color: blue } A ~ B { width: 1px } B { width: 2px }");
    f[1].classes = {"x"};
    CHECK(sp.engine.compute(f[1]).raw("color") == "blue" && sp.engine.compute(f[1]).raw("width") == "1px");
    f[1].classes.clear();
    // what an engine looks at
    CHECK(Sheet("A B > C { color: red } :root { --x: 1 }").engine.uses() == 0);
    CHECK(Sheet("A:first-child + B { color: red }").engine.uses() == (Engine::UsesPosition | Engine::UsesSiblings));
    CHECK(Sheet("@media print { A ~ B { color: red } }").engine.uses() == 0);  // (an @media that doesn't apply)
}

// @media: features, query lists, nesting, and the generation
void testMedia() {
    Sheet s(
        "A { color: red }\n"
        "@media (min-width: 1000px) { A { color: blue } }\n"
        "@media screen and (max-width: 600px) { A { color: green } }\n"
        "@media print { A { --print: y } }\n"
        "@media not print { A { --not-print: y } }\n"
        "@media all { A { --all: y } }\n"
        "@media (orientation: portrait) { A { --portrait: y } }\n"
        "@media (orientation: landscape) { A { --landscape: y } }\n"
        "@media (min-resolution: 2dppx) { A { --hidpi: y } }\n"
        "@media (resolution: 1x) { A { --res1: y } }\n"
        "@media (max-resolution: 96dpi) { A { --upto96dpi: y } }\n"
        "@media (min-width: 50em) { A { --em: y } }\n"
        "@media (min-width: 50rem) and (max-height: 50em) { A { --rem: y } }\n"
        "@media (width >= 900px) and (height < 801px) { A { --range: y } }\n"
        "@media (400px < width < 1300px) { A { --between: y } }\n"
        "@media (1300px > width > 400px) { A { --between2: y } }\n"
        "@media (width = 1280px) { A { --exact: y } }\n"
        "@media (1280px = width) { A { --exact2: y } }\n"
        "@media (width: 1280px) { A { --exact3: y } }\n"
        "@media (height <= 799px) { A { --short: y } }\n"
        "@media only screen and (min-width: 1px), print { A { --list: y } }\n"
        "@media print, (max-width: 10px) { A { --list-none: y } }\n"
        "@media (min-width: 1px) { @media (max-width: 500px) { A { --nested: y } } A { --outer: y } }\n"
        "@media not (min-width: 2000px) { A { --not-cond: y } }\n"
        "@media not screen and (min-width: 2000px) { A { --not-query: y } }\n"
        "@media (width) { A { --bool: y } }\n"
        "@media (min-width: calc(500px + 10em)) { A { --calc: y } }\n"
        "@media (min-width: 1in) { A { --in: y } }\n"
        "@media SCREEN AND (MIN-WIDTH: 1PX) { A { --case: y } }\n"
        "@media { A { --empty-list: y } }\n"
        "@media tv { A { --tv: y } }\n"
        "B { width: 1px }\n");
    CHECK(s.problems.empty());
    SimpleNode a("A");
    auto on = [&](const char* var) { return s.engine.compute(a).has(var); };
    // the default media: 1280 x 800, scale 1
    CHECK(s.engine.media().width == 1280 && s.engine.media().height == 800 && s.engine.media().scale == 1);
    CHECK(s.engine.compute(a).raw("color") == "blue");
    const char* yes[] = {"--not-print", "--all", "--landscape", "--res1", "--upto96dpi", "--em", "--rem", "--range",
                         "--between", "--between2", "--exact", "--exact2", "--exact3", "--list", "--outer",
                         "--not-cond", "--not-query", "--bool", "--calc", "--in", "--case", "--empty-list"};
    const char* no[] = {"--print", "--portrait", "--hidpi", "--short", "--list-none", "--nested", "--tv"};
    for (const char* v : yes) CHECK(on(v));
    for (const char* v : no) CHECK(!on(v));

    // the generation: bumped only when the set of @media blocks that apply changes
    int g = s.engine.generation();
    s.engine.setMedia(Media{1280, 800, 1});
    CHECK(s.engine.generation() == g);
    s.engine.setMedia(Media{1300, 800, 1});
    CHECK(s.engine.generation() != g && !on("--between") && !on("--exact"));
    g = s.engine.generation();
    s.engine.setMedia(Media{500, 800, 1});
    CHECK(s.engine.generation() != g);
    CHECK(s.engine.compute(a).raw("color") == "green");
    CHECK(on("--portrait") && !on("--landscape") && on("--nested") && !on("--em") && !on("--range"));
    g = s.engine.generation();
    s.engine.setMedia(Media{500, 800, 2});
    CHECK(s.engine.generation() != g && on("--hidpi") && !on("--res1") && !on("--upto96dpi"));
    g = s.engine.generation();
    s.engine.setMedia(Media{500, 800, 2.5});  // (min-resolution: 2dppx still; nothing else asks)
    CHECK(s.engine.generation() == g);
    s.engine.setMedia(Media{800, 799, 1});
    CHECK(on("--short") && on("--em") && on("--rem") && s.engine.compute(a).raw("color") == "red");
    s.engine.setMedia(Media{799, 801, 1});
    CHECK(!on("--short") && !on("--em") && !on("--rem") && !on("--range"));
    // only a change in what applies bumps the generation (no vw / vh here)
    Sheet few("@media (min-width: 1000px) { A { color: blue } }");
    g = few.engine.generation();
    few.engine.setMedia(Media{1100, 800, 1});
    CHECK(few.engine.generation() == g && few.engine.media().width == 1100);
    few.engine.setMedia(Media{900, 800, 1});
    CHECK(few.engine.generation() != g && !few.engine.compute(a).has("color"));
    g = few.engine.generation();
    few.engine.setMedia(Media{950, 300, 4});
    CHECK(few.engine.generation() == g);
    few.engine.setMedia(Media{1000, 300, 4});
    CHECK(few.engine.generation() != g && few.engine.compute(a).raw("color") == "blue");
    // setSheets keeps the media
    s.engine.setSheets(s.engine.sheets());
    CHECK(s.engine.media().width == 799 && !on("--em") && on("--portrait"));
    // a sheet without @media and without vw / vh: never bumped by the media
    Sheet plain("A { color: red; width: 10px }");
    g = plain.engine.generation();
    plain.engine.setMedia(Media{100, 100, 3});
    CHECK(plain.engine.generation() == g);
    // the root's variables follow the media too
    Sheet rootMedia(":root { --w: narrow } @media (min-width: 1000px) { :root { --w: wide } }");
    CHECK(rootMedia.engine.root().raw("--w") == "wide");
    rootMedia.engine.setMedia(Media{900, 800, 1});
    CHECK(rootMedia.engine.root().raw("--w") == "narrow");

    // the cascade inside @media: source order still counts
    Sheet order("@media all { A { color: red } } A { color: blue } @media all { B { color: red } } B { color: blue } @media all { B { color: green } }");
    CHECK(order.engine.compute(a).raw("color") == "blue");
    CHECK(order.engine.compute(SimpleNode("B")).raw("color") == "green");

    // queries that can't be read are false (reported); the others in the list still count
    Sheet bad(
        "@media (hover: hover) { A { --hover: y } }\n"                               // 1
        "@media screen and (prefers-color-scheme: dark), (min-width: 1px) { A { --second: y } }\n"
        "@media not (hover) { A { --not-unknown: y } }\n"                            // 3
        "@media screen and { A { --x1: y } }\n"
        "@media (min-width: 10vw) { A { --x2: y } }\n"                               // 5
        "@media (orientation: up) { A { --x3: y } }\n"
        "@media (width: red) { A { --x4: y } }\n"                                    // 7
        "@media (min-width: 1px) or (max-width: 2px) { A { --x5: y } }\n"
        "@media screen (min-width: 1px) { A { --x6: y } }\n"                        // 9
        "@media (resolution: 2) { A { --x7: y } }\n"
        "@media (min-orientation: portrait) { A { --x8: y } }\n"                    // 11
        "@media (400px < width > 300px) { A { --x9: y } }\n"
        "@media only (min-width: 1px) { A { --x10: y } }\n"                         // 13
        "@media screen;\n"
        "@media (orientation < 3px) { A { --x11: y } }\n"                           // 15
        "A { @media all { color: red } --kept: y }\n"
        "@media all { @import 'x.css'; A { --after-import: y } }\n"                 // 17
        "@media (min-width: 1px) { A { --last: y }\n");
    SimpleNode b("A");
    Style st = bad.engine.compute(b);
    CHECK(st.has("--second") && st.has("--kept") && st.has("--after-import") && st.has("--last"));
    for (const char* v : {"--hover", "--not-unknown", "--x1", "--x2", "--x3", "--x4", "--x5", "--x6", "--x7", "--x8",
                          "--x9", "--x10", "--x11"})
        CHECK(!st.has(v));
    CHECK(bad.reported("unknown media feature 'hover' (that query is false)"));
    CHECK(bad.reported("unknown media feature 'prefers-color-scheme'"));
    CHECK(bad.reported("a media query ends with 'and'"));
    CHECK(bad.reported("vw, vh and ch can't be used in a media query"));
    CHECK(bad.reported("orientation is portrait or landscape, not 'up'"));
    CHECK(bad.reported("'red' is not a length"));
    CHECK(bad.reported("'or' in media queries is not supported"));
    CHECK(bad.reported("expected 'and' in a media query"));
    CHECK(bad.reported("a resolution is in dppx, x, dpi or dpcm"));
    CHECK(bad.reported("unknown media feature 'min-orientation'"));
    CHECK(bad.reported("must point the same way"));
    CHECK(bad.reported("expected a media type after 'only'"));
    CHECK(bad.reported("@media without a { block"));
    CHECK(bad.reported("orientation can't be a range"));
    CHECK(bad.reported("@media rules are not supported (skipped)"));  // (inside a rule's block)
    CHECK(bad.reported("@import can't be inside @media"));
    CHECK(bad.reported("missing '}' at the end"));
    int missing = 0;
    for (const Problem& p : bad.problems) missing += p.message.find("missing '}'") != std::string::npos;
    CHECK(missing == 1);
    auto line = [&](const std::string& what) {
        for (const Problem& p : bad.problems)
            if (p.message.find(what) != std::string::npos) return p.line;
        return -1;
    };
    CHECK(line("'hover' (that") == 1 && line("not 'up'") == 6 && line("'or' in") == 8 && line("must point") == 12);
    for (size_t i = 1; i < bad.problems.size(); i++) CHECK(bad.problems[i - 1].line <= bad.problems[i].line);
}

// rem, vw, vh, vmin, vmax, ch, and the absolute units
void testUnits() {
    SimpleNode top("Top");  // (a parent: a node without one is the root, which :root rules match)
    Sheet s(
        ":root { font-size: 20px; --gap: 0.5rem; --wide: 10vw; --ch: 3ch; --mixed: calc(1rem + 1vh) }\n"
        "A { width: 2rem; font-size: 1.5rem; height: 10vh; min-width: 50vw; max-width: calc(100vw - 2rem);\n"
        "    margin-top: 1vmin; margin-bottom: 1vmax; padding-top: var(--gap); padding-left: var(--wide) }\n"
        "B { font-size: 10px; width: 2ch; height: var(--ch); letter-spacing: 1ch }\n"
        "B C { font-size: 30px }\n"
        "D { width: 1in; height: 2.54cm; min-width: 25.4mm; max-width: 4Q; min-height: 1pc; max-height: 3pt;\n"
        "    margin-top: calc(1in - 2.54cm); margin-left: 1IN }\n"
        "E { font-size: 2ch }\n"
        "F { border-top-width: 1rem; border: 0.1rem solid red }\n");
    CHECK(s.problems.empty());
    const Style& root = s.engine.root();
    CHECK(root.raw("font-size") == "20px");
    CHECK(root.raw("--gap") == "10px" && root.raw("--wide") == "128px" && root.raw("--mixed") == "calc(20px + 8px)");
    CHECK(root.raw("--ch") == "3ch");  // (ch: of the node that uses it)
    Style a = s.engine.compute(SimpleNode("A", &top));
    CHECK(a.raw("width") == "40px");       // (rem: :root's font size, not A's)
    CHECK(a.raw("font-size") == "30px");
    CHECK(a.raw("height") == "80px" && a.raw("min-width") == "640px");
    CHECK(a.raw("max-width") == "calc(1280px - 40px)" && a.length("max-width", 0) == 1240);
    CHECK(a.raw("margin-top") == "8px" && a.raw("margin-bottom") == "12.8px");
    CHECK(a.raw("padding-top") == "10px" && a.raw("padding-left") == "128px");
    // ch: half the font size without a measure; the node's font (font-size: the parent's)
    SimpleNode b("B", &top);
    Style bs = s.engine.compute(b);
    CHECK(bs.raw("width") == "10px" && bs.raw("height") == "15px" && bs.raw("letter-spacing") == "5px");
    SimpleNode c("C", &b);
    Style cs = s.engine.compute(c, &bs);
    CHECK(cs.raw("letter-spacing") == "5px");  // (inherited as it was worked out)
    Style e = s.engine.compute(SimpleNode("E", &top));
    CHECK(e.raw("font-size") == "20px");       // (2ch of :root's 20px font: 2 * 10)
    Style d = s.engine.compute(SimpleNode("D", &top));
    CHECK_NEAR(d.length("width", 0), 96, 1e-9);
    CHECK_NEAR(d.length("height", 0), 96, 1e-9);
    CHECK_NEAR(d.length("min-width", 0), 96, 1e-9);
    CHECK_NEAR(d.length("max-width", 0), 96 / 25.4, 1e-9);
    CHECK_NEAR(d.length("min-height", 0), 16, 1e-9);
    CHECK_NEAR(d.length("max-height", 0), 4, 1e-9);
    CHECK_NEAR(d.length("margin-top", -1), 0, 1e-9);
    CHECK_NEAR(d.length("margin-left", 0), 96, 1e-9);
    Style fs = s.engine.compute(SimpleNode("F", &top));
    CHECK(fs.raw("border-left-width") == "2px" && fs.length("border-top-width", 0) == 2);
    // rem in :root's own font-size: of the initial size (12px)
    Sheet r(":root { font-size: 2rem; --r: 1rem } A { width: 1rem }");
    CHECK(r.engine.root().raw("font-size") == "24px" && r.engine.root().raw("--r") == "24px");
    CHECK(r.engine.compute(SimpleNode("A", &top)).raw("width") == "24px");
    // a measure of "0" in the node's font
    Engine m;
    int calls = 0;
    m.setCharWidth([&](const std::string& family, double px) {
        calls++;
        return family == "Mono" ? px * 0.6 : family.empty() ? px * 0.25 : px * 0.4;
    });
    std::vector<Problem> ps;
    m.setSheets({parse("A { font-family: 'Mono', sans-serif; font-size: 10px; width: 10ch } B { width: 10ch } "
                       "C { font-family: X; font-size: 20px; width: 1ch } C D { font-size: 2ch; width: 1px }",
                       "ch.css", ps)});
    CHECK(ps.empty());
    CHECK(calls == 0);
    CHECK(m.compute(SimpleNode("A")).raw("width") == "60px");
    CHECK(m.compute(SimpleNode("B")).raw("width") == "30px");  // (no font-family: "", 12px)
    SimpleNode cn("C"), dn("D", &cn);
    Style cst = m.compute(cn);
    CHECK(cst.raw("width") == "8px");
    CHECK(m.compute(dn, &cst).raw("font-size") == "16px");  // (the parent's font: X at 20px)
    calls = 0;
    m.compute(dn, &cst);
    CHECK(calls == 1);
    m.compute(SimpleNode("Z"));
    CHECK(calls == 1);  // (not asked when nothing uses ch)
    int g = m.generation();
    m.setCharWidth(nullptr);
    CHECK(m.generation() != g && m.compute(SimpleNode("A")).raw("width") == "50px");
    // a measure that makes no sense: half the size
    m.setCharWidth([](const std::string&, double) { return std::nan(""); });
    CHECK(m.compute(SimpleNode("A")).raw("width") == "50px");

    // vw / vh: styles that used them are stale (the generation changes) when the size does
    Sheet v("A { width: 50vw; height: 100vh } B { width: 1px }");
    CHECK(v.problems.empty());
    Style before = v.engine.compute(SimpleNode("A"));
    CHECK(before.raw("width") == "640px" && before.raw("height") == "800px");
    int gen = v.engine.generation();
    v.engine.setMedia(Media{1280, 800, 2});  // (the scale alone: no)
    CHECK(v.engine.generation() == gen);
    v.engine.setMedia(Media{1000, 600, 2});
    CHECK(v.engine.generation() != gen);
    Style after = v.engine.compute(SimpleNode("A"));
    CHECK(after.raw("width") == "500px" && after.raw("height") == "600px");
    CHECK(before.raw("width") == "640px");  // (what was computed stays: the generation says it is stale)
    // vw through a variable from another sheet: that sheet says it uses vw
    std::vector<Problem> p2;
    SheetPtr vars = parse(":root { --w: 10vw }", "vars.css", p2);
    SheetPtr use = parse("A { width: var(--w) }", "use.css", p2);
    Engine two;
    two.setSheets({vars, use});
    CHECK(two.compute(SimpleNode("A")).raw("width") == "128px");
    gen = two.generation();
    two.setMedia(Media{640, 800, 1});
    CHECK(two.generation() != gen && two.compute(SimpleNode("A")).raw("width") == "64px");
    // vw in a property value inside a custom property used as var(): resolved after substitution
    Sheet sub(":root { --x: calc(var(--y) * 2); --y: 1vh } A { height: var(--x) }");
    CHECK(sub.engine.compute(SimpleNode("A")).length("height", 0) == 16);
    // the units are checked like any length
    Sheet bad("A { margin-top: 2rems; margin-left: 1vq; font-size: 3chx; padding: 1rem 1vw 1vh 1ch }");
    CHECK(bad.problems.size() == 3);
    Style bst = bad.engine.compute(SimpleNode("A"));
    CHECK(!bst.has("margin-top") && !bst.has("margin-left") && !bst.has("font-size"));
    CHECK(bst.raw("padding-top") == "12px" && bst.raw("padding-right") == "12.8px" && bst.raw("padding-bottom") == "8px" &&
          bst.raw("padding-left") == "6px");
    // the helpers
    detail::UnitPx px;
    px.rem = 10;
    CHECK(detail::resolveUnits("calc(1rem + 2REM) 3px '4rem' url(5rem.png)", px, detail::URem) ==
          "calc(10px + 20px) 3px '4rem' url(5rem.png)");
    CHECK(detail::resolveUnits("1vw 1vh", px, detail::UVw) == "12.8px 1vh");
    CHECK(detail::resolveUnits("no units", px, 15) == "no units");
}

// @import: in place, relative to each file, with media, loops, depth, files that can't be read
void testImport() {
    std::map<std::string, std::string> files = {
        {"/skins/a.css", "A { color: red; width: 1px } X { color: blue } A { color: blu }"},
        {"/skins/sub/b.css", "@import \"../c.css\";\nB { width: 1px }"},
        {"/skins/c.css",
         "C { width: 3px }\n"
         "@font-face { font-family: \"Grand\"; src: local(Grand), url(fonts/grand.woff2) format(\"woff2\"),"
         " url('fonts/grand.ttf') format(\"truetype\"); font-weight: bold }\n"
         "@media (max-width: 1px) { C { height: 1px } }\n"},
        {"/skins/v.css", "V { width: 1vw }"},
        {"/skins/loop.css", "@import 'loop.css'; L { width: 1px }"},
        {"/skins/x.css", "@import 'y.css'; X { width: 1px }"},
        {"/skins/y.css", "@import url(./x.css); Y { width: 1px }"},
        {"/skins/twice.css", "T { width: 1px }"},
        {"/skins/e.css", "E { color: var(--nope) }"},
    };
    for (int i = 0; i < 12; i++) files["/skins/d" + std::to_string(i) + ".css"] = "@import 'd" + std::to_string(i + 1) + ".css'; D" + std::to_string(i) + " { width: 1px }";
    int reads = 0;
    ParseOptions o;
    o.baseDir = "/skins";
    o.readFile = [&](const std::string& path, std::string& text) {
        reads++;
        auto it = files.find(path);
        if (it == files.end()) return false;
        text = it->second;
        return true;
    };
    std::vector<Problem> ps;
    SheetPtr main = parse(
        "@charset \"utf-8\";\n"
        "@import url(\"a.css\");\n"
        "@import \"sub/b.css\" screen and (min-width: 2000px);\n"
        "@import url(missing.css);\n"
        "@import url(v.css) all;\n"
        "@import 'twice.css'; @import 'twice.css';\n"
        "@import 'e.css';\n"
        "X { color: red }\n",
        "main.css", ps, o);
    CHECK(reads == 8);
    std::vector<std::string> imported = importedFiles(main);
    CHECK(imported.size() == 8);
    if (imported.size() == 8) {
        CHECK(imported[0] == "/skins/a.css" && imported[1] == "/skins/sub/b.css" && imported[2] == "/skins/c.css");
        CHECK(imported[3] == "/skins/missing.css" && imported[4] == "/skins/v.css" && imported[5] == "/skins/twice.css");
        CHECK(imported[6] == "/skins/twice.css" && imported[7] == "/skins/e.css");
    }
    CHECK(anyProblem(ps, "@import of 'missing.css': can't read /skins/missing.css (skipped)"));
    for (const Problem& p : ps)
        if (p.message.find("missing.css") != std::string::npos) CHECK(p.source == "main.css" && p.line == 4 && p.column == 1);
    // an import's own problems are its own (and come first)
    CHECK(!ps.empty() && ps[0].source == "a.css" && ps[0].message.find("unknown colour 'blu'") != std::string::npos);
    CHECK(ps.size() == 2);
    Engine e;
    e.setSheets({main});
    CHECK(e.compute(SimpleNode("A")).raw("color") == "red");
    CHECK(e.compute(SimpleNode("X")).raw("color") == "red");   // (the importing sheet's rule comes later)
    CHECK(!e.compute(SimpleNode("B")).has("width"));             // (screen and (min-width: 2000px))
    CHECK(!e.compute(SimpleNode("C")).has("width"));             // (inside that import too)
    CHECK(e.compute(SimpleNode("T")).raw("width") == "1px");
    CHECK(e.compute(SimpleNode("V")).raw("width") == "12.8px");
    e.setMedia(Media{2000, 800, 1});
    CHECK(e.compute(SimpleNode("B")).raw("width") == "1px" && e.compute(SimpleNode("C")).raw("width") == "3px");
    CHECK(!e.compute(SimpleNode("C")).has("height"));
    e.setMedia(Media{1, 800, 1});                                 // (c.css's own @media, but not the import's)
    CHECK(!e.compute(SimpleNode("C")).has("height") && !e.compute(SimpleNode("C")).has("width"));
    // a problem found computing a rule of an import: where it is (the imported sheet is kept alive by `main`)
    e.compute(SimpleNode("E"));
    std::vector<Problem> found = e.takeProblems();
    CHECK(found.size() == 1 && found[0].source == "e.css" && found[0].line == 1 && found[0].column == 5);
    // an @import's @font-face: its path is its own sheet's folder
    std::vector<FontFace> faces = fontFaces(main);
    CHECK(faces.size() == 1 && faces[0].family == "Grand" && faces[0].file == "/skins/fonts/grand.ttf");
    CHECK(faces.size() == 1 && faces[0].bold && !faces[0].italic);

    // loops: a file that imports itself, two that import each other
    std::vector<Problem> lp;
    SheetPtr loop = parse(files["/skins/loop.css"], "loop.css", lp, o);
    CHECK(lp.size() == 1 && lp[0].message.find("a loop") != std::string::npos && lp[0].source == "loop.css");
    CHECK(importedFiles(loop).empty());
    lp.clear();
    SheetPtr x = parse(files["/skins/x.css"], "x.css", lp, o);
    CHECK(lp.size() == 1 && lp[0].source == "y.css" && lp[0].message.find("'./x.css': a loop") != std::string::npos);
    CHECK(importedFiles(x).size() == 1 && importedFiles(x)[0] == "/skins/y.css");
    Engine xe;
    xe.setSheets({x});
    CHECK(xe.compute(SimpleNode("X")).has("width") && xe.compute(SimpleNode("Y")).has("width"));
    // depth: more than maxDepth levels
    lp.clear();
    SheetPtr deep = parse("@import 'd0.css';", "top.css", lp, o);
    CHECK(lp.size() == 1 && lp[0].message.find("nested more than 8 deep") != std::string::npos && lp[0].source == "d7.css");
    CHECK(importedFiles(deep).size() == 8);
    Engine de;
    de.setSheets({deep});
    CHECK(de.compute(SimpleNode("D7")).has("width") && !de.compute(SimpleNode("D8")).has("width"));
    ParseOptions shallow = o;
    shallow.maxDepth = 0;
    lp.clear();
    parse("@import 'd0.css';", "top.css", lp, shallow);
    CHECK(lp.size() == 1 && lp[0].message.find("nested more than 0 deep") != std::string::npos);

    // what isn't an import that can be followed
    lp.clear();
    SheetPtr bad = parse(
        "@import;\n"
        "@import url();\n"
        "@import 'http://example.com/x.css';\n"
        "@import url(a.css) layer(base);\n"
        "@import url(a.css) supports(display: grid);\n"
        "@import 'a.css' { }\n"
        "@import 42;\n"
        "A { color: red }\n"
        "@import 'a.css';\n",
        "bad.css", lp, o);
    CHECK(lp.size() == 8);
    CHECK(anyProblem(lp, "@import needs url(\"file\") or \"file\""));
    CHECK(anyProblem(lp, "only files can be imported"));
    CHECK(anyProblem(lp, "@import with layer is not supported"));
    CHECK(anyProblem(lp, "@import with supports is not supported"));
    CHECK(anyProblem(lp, "@import with a { block"));
    CHECK(anyProblem(lp, "@import must come before the rules"));
    CHECK(importedFiles(bad).empty());
    // no way to read files: reported
    lp.clear();
    parse("@import 'a.css';", "n.css", lp);
    CHECK(lp.size() == 1 && lp[0].message.find("files can't be read here") != std::string::npos);
    // no baseDir: as written; absolute paths as they are; file: URLs
    ParseOptions here = o;
    here.baseDir = "";
    std::vector<std::string> asked;
    here.readFile = [&](const std::string& path, std::string&) {
        asked.push_back(path);
        return false;
    };
    lp.clear();
    parse("@import 'q.css'; @import '/abs/./r.css'; @import 'file:///f/s.css'; @import '../up/t.css'; @import 'a/../../u.css';",
          "h.css", lp, here);
    CHECK(asked.size() == 5);
    if (asked.size() == 5)
        CHECK(asked[0] == "q.css" && asked[1] == "/abs/r.css" && asked[2] == "/f/s.css" && asked[3] == "../up/t.css" &&
              asked[4] == "../u.css");
    here.baseDir = "/";
    asked.clear();
    parse("@import 'q.css'; @import '../q.css';", "h.css", lp, here);
    CHECK(asked.size() == 2 && asked[0] == "/q.css" && asked[1] == "/q.css");
    CHECK(importedFiles(nullptr).empty() && fontFaces(nullptr).empty());
}

// @font-face: into fontFaces(), the first file that can be read, its path resolved
void testFontFace() {
    ParseOptions o;
    o.baseDir = "/home/me/skins";
    std::vector<Problem> ps;
    SheetPtr s = parse(
        "@font-face { font-family: \"My Font\"; src: url(fonts/my.ttf); }\n"
        "@font-face { font-family: Other Name; src: url(\"../x.otf\") format(\"opentype\"); font-style: italic }\n"
        "@font-face { font-family: 'W'; src: url(w.woff2), url(w.woff) format('woff'), url(w.ttf) format(truetype);"
        " font-weight: 700 }\n"
        "@font-face { font-family: Abs; src: url(/usr/share/fonts/a.ttc) format(collection); font-weight: 400 }\n"
        "@font-face { font-family: Net; src: url(https://x.org/a.ttf), url(file:///opt/n.ttf) }\n"
        "@font-face { font-family: V; src: url(v.ttf) format(\"truetype-variations\"); font-weight: bolder;"
        " font-style: oblique 10deg; unicode-range: U+0-7F }\n"
        "@font-face { src: url(none.ttf) }\n"                                     // 7: no family
        "@font-face { font-family: Only Local; src: local(\"Arial\") }\n"         // 8
        "@font-face { font-family: Woff; src: url(a.woff2) format('woff2') }\n"   // 9
        "@font-face;\n"                                                           // 10
        "@media all { @font-face { font-family: M; src: url(m.ttf) } }\n"         // 11
        "A { font-family: \"My Font\" }\n",
        "faces.css", ps, o);
    std::vector<FontFace> f = fontFaces(s);
    CHECK(f.size() == 6);
    if (f.size() == 6) {
        CHECK(f[0].family == "My Font" && f[0].file == "/home/me/skins/fonts/my.ttf" && !f[0].bold && !f[0].italic);
        CHECK(f[1].family == "Other Name" && f[1].file == "/home/me/x.otf" && !f[1].bold && f[1].italic);
        CHECK(f[2].family == "W" && f[2].file == "/home/me/skins/w.ttf" && f[2].bold);
        CHECK(f[3].family == "Abs" && f[3].file == "/usr/share/fonts/a.ttc" && !f[3].bold);
        CHECK(f[4].family == "Net" && f[4].file == "/opt/n.ttf");
        CHECK(f[5].family == "V" && f[5].bold && f[5].italic);
    }
    CHECK(ps.size() == 5);
    CHECK(anyProblem(ps, "@font-face without a font-family"));
    CHECK(anyProblem(ps, "@font-face for 'Only Local' has no font file"));
    CHECK(anyProblem(ps, "@font-face for 'Woff' has no font file"));
    CHECK(anyProblem(ps, "@font-face without a { block"));
    CHECK(anyProblem(ps, "@font-face inside @media is not supported"));
    Engine e;
    e.setSheets({s});
    CHECK(e.compute(SimpleNode("A")).list("font-family")[0] == "My Font");
    // no baseDir: as written
    ps.clear();
    std::vector<FontFace> g = fontFaces(parse("@font-face { font-family: G; src: url(./g.ttf) }", "g.css", ps));
    CHECK(ps.empty() && g.size() == 1 && g[0].file == "g.ttf");
}

// random stylesheets with the new parts, on nodes with siblings: never a crash
void testFuzzMore() {
    const std::string corpus =
        "@import 'a.css' screen and (min-width: 10px); @media (400px < width < 1300px), not print { A + B ~ C { width: 2rem } }"
        " :is(.a, #b):not(:nth-child(2n+1)) > :first-child ~ *:where(.c) { height: calc(10vw - 1ch) }"
        " @font-face { font-family: X; src: url(x.ttf) format('truetype') } :nth-last-child(odd) { --v: 1vh }";
    const std::string alphabet = "{}();:,.#*>+~!@ \n(-n+3)<=>=2n+1odd:not(:is(:where(@media@importurl('rem vw vh ch";
    std::mt19937 rng(4242);
    SimpleNode top("Window");
    Family f({"A", "B", "C", "A"}, &top);
    f[1].classes = {"a", "c"};
    f[2].idName = "b";
    std::map<std::string, std::string> files = {{"a.css", corpus}, {"b.css", "@import 'a.css';"}};
    ParseOptions o;
    o.readFile = [&](const std::string& path, std::string& text) {
        auto it = files.find(path);
        if (it == files.end()) return false;
        text = it->second;
        return true;
    };
    auto exercise = [&](const std::string& text) {
        std::vector<Problem> ps;
        files["a.css"] = text;
        SheetPtr sheet = parse(text, "fuzz.css", ps, o);
        Engine e;
        e.setSheets({sheet});
        e.setMedia(Media{double(rng() % 2000), double(rng() % 2000), double(1 + rng() % 3)});
        e.setCharWidth([](const std::string&, double px) { return px * 0.6; });
        Style r = e.compute(top);
        for (size_t i = 0; i < 4; i++) {
            Style st = e.compute(f[i], &r);
            for (const auto& kv : st.props_) st.length(kv.first, 0, 100);
        }
        fontFaces(sheet);
        importedFiles(sheet);
        e.takeProblems();
        for (const Problem& pr : ps) CHECK(pr.line >= 1 && pr.column >= 1);
    };
    for (size_t cut = 0; cut <= corpus.size(); cut++) exercise(corpus.substr(0, cut));
    for (int i = 0; i < 1500; i++) {
        std::string t;
        if (i % 2 == 0) {
            t = corpus;
            int edits = 1 + rng() % 8;
            for (int k = 0; k < edits && !t.empty(); k++) {
                size_t at = rng() % t.size();
                switch (rng() % 3) {
                    case 0: t.erase(at, 1 + rng() % 5); break;
                    case 1: t.insert(at, 1, alphabet[rng() % alphabet.size()]); break;
                    default: t[at] = (char)(rng() & 255); break;
                }
            }
        } else {
            int n = rng() % 80;
            for (int k = 0; k < n; k++) {
                size_t from = rng() % alphabet.size();
                t += alphabet.substr(from, 1 + rng() % 6);
            }
        }
        exercise(t);
    }
    exercise(std::string(20000, '@') + "media");
    exercise("@media " + std::string(5000, '(') + " { A { color: red } }");
    std::string nest;
    for (int i = 0; i < 3000; i++) nest += "@media all { ";
    exercise(nest + "A { color: red }");
    std::string isDeep;
    for (int i = 0; i < 3000; i++) isDeep += ":is(";
    exercise("A" + isDeep + " { color: red }");
}

// what paints: gradients (every form), box-shadow and text-shadow lists, border styles (alone, per side and in
// the border shorthands), opacity, all through var() too; bad values left out and reported where they are
void testPaintValues() {
    using G = Style::Gradient;
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    Sheet s(
        ":root { --a: red; --g: linear-gradient(to right, var(--a), blue); --sh: 1px 2px 3px green; --t: 45deg }\n"  // 1
        "L1 { background-image: linear-gradient(red, blue) }\n"                                                       // 2
        "L2 { background-image: linear-gradient(90deg, red 10%, lime 20px, blue) }\n"                                 // 3
        "L3 { background-image: linear-gradient(0.25turn, red, blue), linear-gradient(1.5708rad, red, blue) }\n"     // 4
        "L4 { background-image: linear-gradient(100grad, red 0 50%, blue 50% 100%); }\n"                            // 5
        "L5 { background-image: linear-gradient(to left top, red, blue) }\n"                                         // 6
        "L6 { background-image: linear-gradient(to bottom, red, blue); background-image: linear-gradient(0, red, blue) }\n"  // 7
        "L7 { background-image: repeating-linear-gradient(-45deg, red, red 5px, white 5px, white 10px) }\n"         // 8
        "L8 { background-image: linear-gradient(10% red, 90% blue) }\n"                                             // 9
        "R1 { background-image: radial-gradient(red, blue) }\n"                                                       // 10
        "R2 { background-image: radial-gradient(circle closest-side at left top, red, blue) }\n"                     // 11
        "R3 { background-image: radial-gradient(farthest-side ellipse at 30% 10px, red, blue) }\n"                   // 12
        "R4 { background-image: radial-gradient(20px at bottom right, red, blue) }\n"                                // 13
        "R5 { background-image: radial-gradient(40% 2em, red, blue); font-size: 10px }\n"                            // 14
        "R6 { background-image: repeating-radial-gradient(circle at top, red 0 4px, blue 4px 8px) }\n"              // 15
        "R7 { background-image: radial-gradient(closest-corner, red, blue), none }\n"                                // 16
        "B1 { background: linear-gradient(red, blue) #123456 }\n"                                                    // 17
        "B2 { background: #102030 radial-gradient(red, blue) }\n"                                                    // 18
        "B3 { background: linear-gradient(red, blue), url(x.png) no-repeat green }\n"                                // 19
        "B4 { background: blue }\n"                                                                                   // 20
        "V1 { background-image: var(--g) }\n"                                                                         // 21
        "V2 { background: var(--g) var(--a) }\n"                                                                      // 22
        "V3 { background-image: linear-gradient(var(--t), var(--a) 0%, blue) }\n"                                    // 23
        "S1 { box-shadow: 2px 3px }\n"                                                                                // 24
        "S2 { box-shadow: inset 0 1px 2px rgba(0, 0, 0, 0.5), 0 0 0 1px white, 1px 1px 4px 2px blue inset }\n"      // 25
        "S3 { box-shadow: red 1px 2px 3px 4px; color: #00ff00 }\n"                                                   // 26
        "S4 { box-shadow: 1px 1px; color: #00ff00 }\n"                                                               // 27
        "S5 { box-shadow: none; text-shadow: 1px 1px 2px black, 0 0 1em blue; font-size: 10px }\n"                  // 28
        "S6 { box-shadow: var(--sh); text-shadow: var(--sh) }\n"                                                     // 29
        "O1 { opacity: 0.5 } O2 { opacity: 25% } O3 { opacity: var(--o, 0.75) }\n"                                   // 30
        "Y1 { border-style: inset outset; border-top-style: double }\n"                                              // 31
        "Y2 { border: 2px groove #808080; border-left: 3px ridge red; border-bottom: dotted }\n"                     // 32
        "Y3 { border-top: 1px dashed blue; border-right-style: hidden; border-right-width: 4px }\n"                  // 33
        "Y4 { border-width: 2px; border-style: none }\n"                                                             // 34
        "Y5 { border: var(--w, 2px) outset var(--a) }\n"                                                             // 35
        "X { background-image: linear-gradient(red) }\n"                                                             // 36
        "X { background-image: linear-gradient(to middle, red, blue) }\n"                                            // 37
        "X { background-image: radial-gradient(circle 10% , red, blue) }\n"                                          // 38
        "X { background-image: url(a.png) url(b.png) }\n"                                                            // 39
        "X { background-image: linear-gradient(red, 50%, blue) }\n"                                                  // 40
        "X { background: linear-gradient(red, nope) blue }\n"                                                        // 41
        "X { box-shadow: 1px }\n"                                                                                     // 42
        "X { box-shadow: 1px 1px -2px red }\n"                                                                       // 43
        "X { text-shadow: inset 1px 1px red }\n"                                                                     // 44
        "X { text-shadow: 1px 1px 1px 1px red }\n"                                                                   // 45
        "X { opacity: big }\n"                                                                                        // 46
        "X { border-left-style: wavy }\n"                                                                             // 47
        "X { border-style: solid wavy }\n"                                                                            // 48
        "X { background-image: radial-gradient(circle 10px 20px, red, blue) }\n"                                    // 49
        "X { background-image: linear-gradient(45deg 10px, red, blue) }\n"                                           // 50
        "X { box-shadow: 1px 2px red 3px }\n"                                                                         // 51
        "Z1 { background-image: var(--nothing, linear-gradient(red, nope)) }\n");                                    // 52
    auto at = [&](const std::string& what, int line, int col) {
        for (const Problem& p : s.problems)
            if (p.line == line && p.message.find(what) != std::string::npos) return p.column == col;
        return false;
    };
    CHECK(at("a gradient needs two colour stops", 36, 5));
    CHECK(at("is not a side", 37, 5));
    CHECK(at("a circle's size is one length", 38, 5));
    CHECK(at("is not one url(", 39, 5));
    CHECK(at("transition hints", 40, 5));
    CHECK(at("bad value for 'background'", 41, 5));
    CHECK(at("needs its x and y offsets", 42, 5));
    CHECK(at("blur can't be negative", 43, 5));
    CHECK(at("is not part of a shadow", 44, 5));
    CHECK(at("at most 3 lengths", 45, 5));
    CHECK(at("bad value for 'opacity'", 46, 5));
    CHECK(at("bad value for 'border-left-style'", 47, 5));
    CHECK(at("bad value for 'border-style'", 48, 5));
    CHECK(at("a circle's size is one length", 49, 5));
    CHECK(at("is not a colour or a position", 50, 5));
    CHECK(at("is not part of a shadow", 51, 5));
    CHECK(s.problems.size() == 16);
    for (const Problem& p : s.problems) CHECK(p.line >= 36 && p.line <= 51);
    CHECK(!s.engine.compute(SimpleNode("X")).has("background-image") && !s.engine.compute(SimpleNode("X")).has("box-shadow"));

    auto grads = [&](const char* type) { return s.engine.compute(SimpleNode(type)).gradients(); };
    auto stopIs = [&](const Style::ColorStop& st, Rgba c, double pos, bool px) {
        return st.color == c && near(st.position, pos) && st.px == px;
    };
    std::vector<G> g = grads("L1");
    CHECK(g.size() == 1 && g[0].kind == G::Linear && !g[0].repeating && g[0].angle == 180 && !g[0].toCorner);
    CHECK(g.size() == 1 && g[0].stops.size() == 2 && stopIs(g[0].stops[0], rgba(255, 0, 0), -1, false) &&
          stopIs(g[0].stops[1], rgba(0, 0, 255), -1, false));
    g = grads("L2");
    CHECK(g.size() == 1 && g[0].angle == 90 && g[0].stops.size() == 3);
    CHECK(g.size() == 1 && stopIs(g[0].stops[0], rgba(255, 0, 0), 0.1, false) && stopIs(g[0].stops[1], rgba(0, 255, 0), 20, true));
    g = grads("L3");
    CHECK(g.size() == 2 && near(g[0].angle, 90) && std::fabs(g[1].angle - 90) < 0.01);
    g = grads("L4");
    CHECK(g.size() == 1 && near(g[0].angle, 90) && g[0].stops.size() == 4);  // (two-position stops: two each)
    CHECK(g.size() == 1 && stopIs(g[0].stops[0], rgba(255, 0, 0), 0, true) && stopIs(g[0].stops[1], rgba(255, 0, 0), 0.5, false) &&
          stopIs(g[0].stops[2], rgba(0, 0, 255), 0.5, false) && stopIs(g[0].stops[3], rgba(0, 0, 255), 1, false));
    g = grads("L5");
    CHECK(g.size() == 1 && g[0].toCorner && g[0].cornerX == -1 && g[0].cornerY == -1);
    g = grads("L6");
    CHECK(g.size() == 1 && g[0].angle == 0);  // (the later declaration)
    g = grads("L7");
    CHECK(g.size() == 1 && g[0].repeating && near(g[0].angle, 315) && g[0].stops.size() == 4 && g[0].stops[3].px);
    g = grads("L8");  // (a position before its colour)
    CHECK(g.size() == 1 && stopIs(g[0].stops[0], rgba(255, 0, 0), 0.1, false) && stopIs(g[0].stops[1], rgba(0, 0, 255), 0.9, false));
    g = grads("R1");
    CHECK(g.size() == 1 && g[0].kind == G::Radial && !g[0].circle && g[0].size == G::FarthestCorner && g[0].centerX == 0.5 &&
          g[0].centerY == 0.5);
    g = grads("R2");
    CHECK(g.size() == 1 && g[0].circle && g[0].size == G::ClosestSide && g[0].centerX == 0 && g[0].centerY == 0 &&
          !g[0].centerXPx && !g[0].centerYPx);
    g = grads("R3");
    CHECK(g.size() == 1 && !g[0].circle && g[0].size == G::FarthestSide && near(g[0].centerX, 0.3) && !g[0].centerXPx &&
          g[0].centerY == 10 && g[0].centerYPx);
    g = grads("R4");
    CHECK(g.size() == 1 && g[0].circle && g[0].size == G::Explicit && g[0].radiusX == 20 && g[0].radiusY == 20 &&
          g[0].centerX == 1 && g[0].centerY == 1);
    g = grads("R5");
    CHECK(g.size() == 1 && !g[0].circle && g[0].size == G::Explicit && near(g[0].radiusX, 0.4) && g[0].radiusXPercent &&
          g[0].radiusY == 20 && !g[0].radiusYPercent);
    g = grads("R6");
    CHECK(g.size() == 1 && g[0].repeating && g[0].circle && g[0].centerX == 0.5 && g[0].centerY == 0 && g[0].stops.size() == 4);
    g = grads("R7");
    CHECK(g.size() == 1 && g[0].size == G::ClosestCorner);  // (a `none` layer is no gradient)
    Style b1 = s.engine.compute(SimpleNode("B1"));
    CHECK(b1.color("background-color", Rgba{}) == rgba(0x12, 0x34, 0x56) && b1.gradients().size() == 1);
    Style b2 = s.engine.compute(SimpleNode("B2"));
    CHECK(b2.color("background-color", Rgba{}) == rgba(0x10, 0x20, 0x30) && b2.gradients().size() == 1 &&
          b2.gradients()[0].kind == G::Radial);
    Style b3 = s.engine.compute(SimpleNode("B3"));  // (a url() layer; the colour from the last layer)
    CHECK(b3.raw("background-image") == "linear-gradient(red, blue), url(x.png)" && b3.gradients().size() == 1 &&
          b3.color("background-color", Rgba{}) == rgba(0, 128, 0));
    CHECK(b3.raw("background-repeat") == "repeat, no-repeat" && b3.backgrounds().size() == 2 &&
          b3.backgrounds()[1].kind == Style::Background::UrlImage && b3.backgrounds()[1].url == "x.png" &&
          b3.backgrounds()[1].repeatX == Style::Background::NoRepeat);
    Style b4 = s.engine.compute(SimpleNode("B4"));
    CHECK(b4.raw("background-image") == "none" && b4.gradients().empty());
    g = grads("V1");
    CHECK(g.size() == 1 && g[0].angle == 90 && g[0].stops.size() == 2 && g[0].stops[0].color == rgba(255, 0, 0));
    Style v2 = s.engine.compute(SimpleNode("V2"));
    CHECK(v2.gradients().size() == 1 && v2.color("background-color", Rgba{}) == rgba(255, 0, 0));
    g = grads("V3");
    CHECK(g.size() == 1 && g[0].angle == 45 && g[0].stops[0].position == 0);
    CHECK(grads("Z1").empty() && anyProblem(s.engine.takeProblems(), "after var()"));

    auto shadows = [&](const char* type, const char* prop) { return s.engine.compute(SimpleNode(type)).shadows(prop); };
    std::vector<Style::Shadow> sh = shadows("S1", "box-shadow");
    CHECK(sh.size() == 1 && !sh[0].inset && sh[0].x == 2 && sh[0].y == 3 && sh[0].blur == 0 && sh[0].spread == 0 &&
          sh[0].color == rgba(0, 0, 0));
    sh = shadows("S2", "box-shadow");
    CHECK(sh.size() == 3 && sh[0].inset && sh[0].y == 1 && sh[0].blur == 2 && sh[0].color == rgba(0, 0, 0, 128));
    CHECK(sh.size() == 3 && !sh[1].inset && sh[1].spread == 1 && sh[1].color == rgba(255, 255, 255));
    CHECK(sh.size() == 3 && sh[2].inset && sh[2].blur == 4 && sh[2].spread == 2 && sh[2].color == rgba(0, 0, 255));
    sh = shadows("S3", "box-shadow");
    CHECK(sh.size() == 1 && sh[0].color == rgba(255, 0, 0) && sh[0].spread == 4);
    sh = shadows("S4", "box-shadow");
    CHECK(sh.size() == 1 && sh[0].color == rgba(0, 255, 0));  // (currentColor)
    CHECK(shadows("S5", "box-shadow").empty());
    sh = shadows("S5", "text-shadow");
    CHECK(sh.size() == 2 && sh[0].blur == 2 && sh[1].blur == 10 && sh[1].color == rgba(0, 0, 255));  // (1em: 10px)
    sh = shadows("S6", "box-shadow");
    CHECK(sh.size() == 1 && sh[0].x == 1 && sh[0].y == 2 && sh[0].blur == 3 && sh[0].color == rgba(0, 128, 0));
    CHECK(shadows("S6", "text-shadow").size() == 1);
    CHECK(shadows("L1", "box-shadow").empty());

    CHECK(near(s.engine.compute(SimpleNode("O1")).number("opacity", 1), 0.5));
    CHECK(near(s.engine.compute(SimpleNode("O2")).number("opacity", 1), 0.25));
    CHECK(near(s.engine.compute(SimpleNode("O3")).number("opacity", 1), 0.75));

    Style y1 = s.engine.compute(SimpleNode("Y1"));
    CHECK(y1.keyword("border-top-style", "") == "double" && y1.keyword("border-right-style", "") == "outset" &&
          y1.keyword("border-bottom-style", "") == "inset" && y1.keyword("border-left-style", "") == "outset");
    Style y2 = s.engine.compute(SimpleNode("Y2"));
    CHECK(y2.keyword("border-top-style", "") == "groove" && y2.keyword("border-left-style", "") == "ridge" &&
          y2.length("border-left-width", 0) == 3 && y2.color("border-left-color", Rgba{}) == rgba(255, 0, 0));
    CHECK(y2.keyword("border-bottom-style", "") == "dotted" && y2.length("border-bottom-width", 0) == 3);  // (medium)
    Style y3 = s.engine.compute(SimpleNode("Y3"));
    CHECK(y3.keyword("border-top-style", "") == "dashed" && y3.length("border-right-width", 9) == 0);  // (hidden: none)
    Style y4 = s.engine.compute(SimpleNode("Y4"));
    CHECK(y4.edges("border-width", Edges{9, 9, 9, 9}).top == 0 && y4.edges("border-width", Edges{9, 9, 9, 9}).left == 0);
    Style y5 = s.engine.compute(SimpleNode("Y5"));
    CHECK(y5.keyword("border-left-style", "") == "outset" && y5.length("border-left-width", 0) == 2 &&
          y5.color("border-left-color", Rgba{}) == rgba(255, 0, 0));
    // the value helpers directly: every angle unit, every keyword position
    using namespace css::detail;
    auto angle = [](const char* text) {
        Tokens t;
        tokenize(text, t);
        G gr;
        std::string why;
        return parseGradient(t, 0, t.size(), nullptr, 12, gr, why) ? gr.angle : -1.0;
    };
    CHECK(near(angle("linear-gradient(0.5turn, red, blue)"), 180) && near(angle("linear-gradient(200grad, red, blue)"), 180));
    CHECK(std::fabs(angle("linear-gradient(3.14159265rad, red, blue)") - 180) < 1e-5 && near(angle("linear-gradient(-90deg, red, blue)"), 270));
    CHECK(near(angle("linear-gradient(to top, red, blue)"), 0) && near(angle("linear-gradient(to left, red, blue)"), 270));
    CHECK(angle("linear-gradient(1px, red, blue)") == -1 && angle("linear-gradient(red, blue") == -1);
    CHECK(angle("linear-gradient(currentColor, blue)") == -1);  // (no currentColor here)
}

// url() images in background-image and the background shorthand, background-repeat / -position / -size /
// -origin / -clip, url()s resolved against the sheet's folder and checked (problems at their line and column),
// imageFiles(), -tiny-glyph and its properties, font-smooth and image-rendering (inherited)
void testImages() {
    using B = Style::Background;
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    Sheet s(
        "I1 { background-image: url(a.png), linear-gradient(red, blue), none, url(\"b c.svg\") }\n"
        "I2 { background-image: url(a.png); background-repeat: repeat-x, no-repeat; background-position: right 4px bottom 10%;"
        " background-size: 10px auto; background-origin: content-box; background-clip: padding-box }\n"
        "I3 { background: url(t.png) center / cover no-repeat padding-box content-box #102030 }\n"
        "I4 { background: url(a.png) 0 0 / 50% 3px space round, url(b.png) left 2px top 3px repeat-y blue }\n"
        "I5 { background-image: url(a.png), url(b.png), url(c.png); background-repeat: space, round;"
        " background-size: contain }\n"
        "P1 { background-image: url(a.png); background-position: top }\n"
        "P2 { background-image: url(a.png); background-position: 30% bottom }\n"
        "P3 { background-image: url(a.png); background-position: 7px 9px }\n"
        "P4 { background-image: url(a.png); background-position: bottom right 6px }\n"
        "G1 { -tiny-glyph: url(check.svg); -tiny-glyph-size: 7px 8px; -tiny-glyph-offset: 1px; -tiny-glyph-tint: color }\n"
        "G2 { -tiny-glyph: none; -tiny-glyph-size: auto 2em; font-size: 10px }\n"
        ":root { font-smooth: never; image-rendering: pixelated }\n"
        "F2 { -webkit-font-smoothing: none } F3 { font-smooth: 2em }\n"
        "X { background-repeat: sideways }\n"                     // 14
        "X { background-position: left 3px 4px }\n"               // 15
        "X { background-size: -1px }\n"                           // 16
        "X { background-origin: margin-box }\n"                   // 17
        "X { background-clip: border-box padding-box }\n"         // 18
        "X { image-rendering: blurry }\n"                         // 19
        "X { font-smooth: sometimes }\n"                          // 20
        "X { -webkit-font-smoothing: never }\n"                   // 21
        "X { -tiny-glyph: linear-gradient(red, blue) }\n"         // 22
        "X { -tiny-glyph-size: 1px 2px 3px }\n"                   // 23
        "X { -tiny-glyph-offset: auto }\n"                        // 24
        "X { -tiny-glyph-tint: red }\n"                           // 25
        "X { background: url(a.png) / cover }\n"                  // 26
        "X { background: url(a.png) url(b.png) }\n");            // 27
    for (const Problem& p : s.problems) CHECK(p.line >= 14 && p.line <= 27);
    for (int line = 14; line <= 27; line++) {
        bool found = false;
        for (const Problem& p : s.problems) found = found || (p.line == line && p.column == 5);
        CHECK(found);
        if (!found) printf("  no problem on line %d\n", line);
    }
    CHECK(s.problems.size() == 14);
    if (s.problems.size() != 14)
        for (const Problem& p : s.problems) printf("  %s\n", p.text().c_str());
    auto bg = [&](const char* type) { return s.engine.compute(SimpleNode(type)).backgrounds(); };
    std::vector<B> l = bg("I1");
    CHECK(l.size() == 4 && l[0].kind == B::UrlImage && l[0].url == "a.png" && l[1].kind == B::GradientImage &&
          l[2].kind == B::NoImage && l[3].url == "b c.svg");
    CHECK(l.size() == 4 && l[0].repeatX == B::Repeat && l[0].repeatY == B::Repeat && l[0].posX == 0 && l[0].posY == 0 &&
          l[0].sizing == B::Given && l[0].width == -1 && l[0].height == -1 && l[0].origin == B::PaddingBox &&
          l[0].clip == B::BorderBox);
    l = bg("I2");
    CHECK(l.size() == 1 && l[0].repeatX == B::Repeat && l[0].repeatY == B::NoRepeat);
    CHECK(l.size() == 1 && l[0].posX == 1 && l[0].offX == -4 && near(l[0].posY, 0.9) && l[0].offY == 0);
    CHECK(l.size() == 1 && l[0].width == 10 && !l[0].widthPercent && l[0].height == -1);
    CHECK(l.size() == 1 && l[0].origin == B::ContentBox && l[0].clip == B::PaddingBox);
    Style i3 = s.engine.compute(SimpleNode("I3"));
    l = i3.backgrounds();
    CHECK(i3.color("background-color", Rgba{}) == rgba(0x10, 0x20, 0x30));
    CHECK(l.size() == 1 && l[0].url == "t.png" && l[0].sizing == B::Cover && l[0].posX == 0.5 && l[0].posY == 0.5 &&
          l[0].repeatX == B::NoRepeat && l[0].repeatY == B::NoRepeat && l[0].origin == B::PaddingBox &&
          l[0].clip == B::ContentBox);
    CHECK(i3.raw("background-size") == "cover" && i3.raw("background-origin") == "padding-box");
    Style i4 = s.engine.compute(SimpleNode("I4"));
    l = i4.backgrounds();
    CHECK(i4.raw("background-image") == "url(a.png), url(b.png)" && i4.color("background-color", Rgba{}) == rgba(0, 0, 255));
    CHECK(l.size() == 2 && l[0].repeatX == B::Space && l[0].repeatY == B::Round && near(l[0].width, 0.5) &&
          l[0].widthPercent && l[0].height == 3 && l[1].repeatX == B::NoRepeat && l[1].repeatY == B::Repeat);
    CHECK(l.size() == 2 && l[1].posX == 0 && l[1].offX == 2 && l[1].posY == 0 && l[1].offY == 3 && l[1].sizing == B::Given &&
          l[1].width == -1);
    l = bg("I5");  // (the lists repeated to the layers' number)
    CHECK(l.size() == 3 && l[0].repeatX == B::Space && l[1].repeatX == B::Round && l[2].repeatX == B::Space &&
          l[2].sizing == B::Contain);
    l = bg("P1");
    CHECK(l.size() == 1 && l[0].posX == 0.5 && l[0].posY == 0);
    l = bg("P2");
    CHECK(l.size() == 1 && near(l[0].posX, 0.3) && l[0].posY == 1);
    l = bg("P3");
    CHECK(l.size() == 1 && l[0].posX == 0 && l[0].offX == 7 && l[0].posY == 0 && l[0].offY == 9);
    l = bg("P4");
    CHECK(l.size() == 1 && l[0].posY == 1 && l[0].offY == 0 && l[0].posX == 1 && l[0].offX == -6);
    Style g1 = s.engine.compute(SimpleNode("G1"));
    CHECK(g1.url("-tiny-glyph") == "check.svg" && g1.raw("-tiny-glyph-size") == "7px 8px" &&
          g1.keyword("-tiny-glyph-tint", "") == "color");
    CHECK(s.engine.compute(SimpleNode("G2")).url("-tiny-glyph").empty());
    // inherited: font-smooth, image-rendering
    Style child = s.engine.compute(SimpleNode("F1"));
    CHECK(child.keyword("font-smooth", "") == "never" && child.keyword("image-rendering", "") == "pixelated");
    CHECK(s.engine.compute(SimpleNode("F2")).keyword("-webkit-font-smoothing", "") == "none");
    CHECK(!s.engine.compute(SimpleNode("I1")).has("-tiny-glyph"));

    // url()s resolved against the sheet's folder and checked; problems where they are named
    std::string dir = "build/test-css-images";
    CHECK(util::makeDirs(dir + "/90s"));
    CHECK(util::writeFile(dir + "/90s/check.svg", "<svg xmlns='http://www.w3.org/2000/svg' width='7' height='7'/>"));
    CHECK(util::writeFile(dir + "/tile.png", "not checked here"));
    std::map<std::string, std::string> files = {{dir + "/sub/inner.css", "Inner { background-image: url(../90s/check.svg) }"}};
    ParseOptions o;
    o.baseDir = dir;
    o.readFile = [&](const std::string& path, std::string& text) {
        if (!files.count(path)) return util::readFile(path, text);
        text = files[path];
        return true;
    };
    std::vector<Problem> ps;
    SheetPtr sheet = parse(
        "@import url(sub/inner.css);\n"
        "A { background: url(90s/check.svg) no-repeat, url('tile.png') }\n"
        "B {\n  -tiny-glyph: url(missing.svg);\n} :root { --img: url(90s/check.svg) }\n"
        "C { background-image: var(--img); -tiny-glyph: var(--img) }\n"
        "D { background-image: url(http://example.com/x.png) }\n"
        "@font-face { font-family: F; src: url(f.ttf) }\n",
        "skin.css", ps, o);
    CHECK(ps.size() == 2);
    if (ps.size() != 2)
        for (const Problem& p : ps) printf("  %s\n", p.text().c_str());
    CHECK(anyProblem(ps, "can't use missing.svg") && ps.size() == 2 && ps[0].line == 4 && ps[0].column == 16);
    CHECK(anyProblem(ps, "only files can be used") && ps.size() == 2 && ps[1].line == 7 && ps[1].column == 23);
    std::vector<std::string> imgs = imageFiles(sheet);
    CHECK(imgs.size() == 3 && imgs[0] == dir + "/90s/check.svg" && imgs[1] == dir + "/tile.png" &&
          imgs[2] == dir + "/missing.svg");
    CHECK(fontFaces(sheet).size() == 1 && fontFaces(sheet)[0].file == dir + "/f.ttf");  // (not resolved twice)
    Engine e;
    e.setSheets({sheet});
    Style a = e.compute(SimpleNode("A"));
    l = a.backgrounds();
    CHECK(l.size() == 2 && l[0].url == dir + "/90s/check.svg" && l[1].url == dir + "/tile.png" &&
          l[0].repeatX == B::NoRepeat && l[1].repeatX == B::Repeat);
    Style c = e.compute(SimpleNode("C"));  // (through var(): the declaring sheet's folder)
    CHECK(c.backgrounds().size() == 1 && c.backgrounds()[0].url == dir + "/90s/check.svg");
    CHECK(c.url("-tiny-glyph") == dir + "/90s/check.svg");
    CHECK(e.compute(SimpleNode("Inner")).backgrounds().size() == 1 &&
          e.compute(SimpleNode("Inner")).backgrounds()[0].url == dir + "/90s/check.svg");  // (the import's own folder)
    // a checker of the program's own (the skin's: can the image be drawn?)
    ps.clear();
    int asked = 0;
    o.checkImage = [&](const std::string& path, std::string& why) {
        asked++;
        why = "not a PNG or an SVG image";
        return path.find("tile.png") == std::string::npos;
    };
    parse("A { background: url(tile.png) }", "skin.css", ps, o);
    CHECK(asked == 1 && ps.size() == 1 && anyProblem(ps, "can't use tile.png") && anyProblem(ps, "not a PNG or an SVG image") &&
          ps[0].line == 1 && ps[0].column == 17);
    // a sheet not from a file: url()s as written, not checked
    ps.clear();
    sheet = parse("A { background-image: url(nowhere.png) }", "x.css", ps);
    CHECK(ps.empty() && imageFiles(sheet).size() == 1 && imageFiles(sheet)[0] == "nowhere.png");
    util::removeFile(dir + "/90s/check.svg");
    util::removeFile(dir + "/tile.png");
}

// calc() of percentages and lengths together where a percentage may be (background-size, -position, colour stops,
// a radial gradient's centre and size): kept as a fraction plus px (drawn: test_toolkit); through var(), and
// through var() in the background shorthand; what still isn't a value reported (when the sheet is read, or with
// var() by Engine::checkVarUses, once, without computing a node)
void testCalcMixed() {
    using B = Style::Background;
    auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    {
        using namespace css::detail;
        Tokens t;
        double px = 0, frac = 0;
        LenCtx ctx;
        auto lp = [&](const char* text) {
            tokenize(text, t);
            return evalLengthPercent(t, 0, t.size(), ctx, px, frac);
        };
        CHECK(lp("calc(100% - 8px)") && near(frac, 1) && near(px, -8));
        CHECK(lp("calc((50% + 2px) * 2 - 1em / 2)") && near(frac, 1) && near(px, 4 - 6));
        CHECK(lp("calc(2 * (10% - 1pt))") && near(frac, 0.2) && near(px, -2 * 96.0 / 72));
        CHECK(lp("calc(25% / 2)") && near(frac, 0.125) && near(px, 0));
        CHECK(lp("50%") && near(frac, 0.5) && near(px, 0));
        CHECK(lp("3px") && near(frac, 0) && near(px, 3));
        CHECK(lp("0") && frac == 0 && px == 0);
        CHECK(!lp("calc(100% * 2px)") && !lp("calc(10% / 5px)") && !lp("calc(10% + 2)") && !lp("calc(100% / 0)"));
        CHECK(!lp("2") && !lp("red") && !lp("calc(100%-8px)"));
        // (evalLength as it was: a percentage of what it is of)
        ctx.percentOf = 200;
        double whole = 0;
        tokenize("calc(100% - 8px)", t);
        CHECK(evalLength(t, 0, t.size(), ctx, whole) && near(whole, 192));
    }
    Sheet s(
        ":root { --h: calc(100% - 8px); --p: calc(100% - 8px) 0; --e: calc(100% - 1px); --s: 2px calc(100% - 8px) }\n"  // 1
        "S1 { background-image: url(a.png); background-size: 2px calc(100% - 8px) }\n"                                     // 2
        "S2 { background-image: url(a.png); background-size: 2px var(--h) }\n"                                             // 3
        "S3 { background: url(a.png) 0 0 / var(--s) no-repeat }\n"                                                         // 4
        "S4 { background-image: url(a.png); background-size: calc(-10% + 30px) auto }\n"                                   // 5
        "P1 { background-image: url(a.png); background-position: calc(100% - 8px) calc(50% + 1em) }\n"                    // 6
        "P2 { background-image: url(a.png); background-position: var(--p) }\n"                                             // 7
        "P3 { background: url(a.png) var(--p) / 2px 100% no-repeat }\n"                                                    // 8
        "P4 { background-image: url(a.png); background-position: right calc(10% + 2px) bottom calc(5% - 1px) }\n"          // 9
        "G1 { background-image: linear-gradient(red, red calc(100% - 1px), blue calc(100% - 1px)) }\n"                     // 10
        "G2 { background: linear-gradient(red, red var(--e), blue var(--e)) }\n"                                            // 11
        "G3 { background-image: radial-gradient(ellipse calc(50% - 2px) calc(50% + 1px) at calc(100% - 4px) 25%, red, blue) }\n"  // 12
        "X1 { background-size: calc(100% * 2px) }\n"                                                                        // 13
        "X2 { background-size: -10% }\n"                                                                                    // 14
        "X3 { background-image: radial-gradient(circle calc(50% + 1px), red, blue) }\n"                                     // 15
        "X4 { background-image: radial-gradient(ellipse -10% 20%, red, blue) }\n");                                         // 16
    auto bg = [&](const char* type) { return s.engine.compute(SimpleNode(type)).backgrounds(); };
    std::vector<B> l = bg("S1");
    CHECK(l.size() == 1 && near(l[0].width, 2) && !l[0].widthPercent && l[0].heightPercent && near(l[0].height, 1) &&
          near(l[0].heightPlus, -8));
    l = bg("S2");
    CHECK(l.size() == 1 && l[0].heightPercent && near(l[0].height, 1) && near(l[0].heightPlus, -8));
    l = bg("S3");
    CHECK(l.size() == 1 && l[0].heightPercent && near(l[0].heightPlus, -8) && l[0].repeatX == B::NoRepeat);
    l = bg("S4");  // (below 0 for a box over 300 px: 0 there)
    CHECK(l.size() == 1 && l[0].widthPercent && near(l[0].width, -0.1) && near(l[0].widthPlus, 30) && l[0].height == -1);
    l = bg("P1");
    CHECK(l.size() == 1 && near(l[0].posX, 1) && near(l[0].offX, -8) && near(l[0].posY, 0.5) && near(l[0].offY, 12));
    l = bg("P2");
    CHECK(l.size() == 1 && near(l[0].posX, 1) && near(l[0].offX, -8) && near(l[0].posY, 0) && near(l[0].offY, 0));
    l = bg("P3");
    CHECK(l.size() == 1 && near(l[0].posX, 1) && near(l[0].offX, -8) && near(l[0].width, 2) && l[0].heightPercent);
    l = bg("P4");  // (from the right / bottom: inwards)
    CHECK(l.size() == 1 && near(l[0].posX, 0.9) && near(l[0].offX, -2) && near(l[0].posY, 0.95) && near(l[0].offY, 1));
    for (const char* g : {"G1", "G2"}) {
        l = bg(g);
        CHECK(l.size() == 1 && l[0].gradient.stops.size() == 3);
        if (l.size() != 1 || l[0].gradient.stops.size() != 3) continue;
        const auto& st = l[0].gradient.stops;
        CHECK(st[0].position == -1 && !st[1].px && near(st[1].position, 1) && near(st[1].plus, -1) &&
              near(st[2].position, 1) && near(st[2].plus, -1));
    }
    l = bg("G3");
    CHECK(l.size() == 1);
    if (l.size() == 1) {
        const Style::Gradient& g = l[0].gradient;
        CHECK(g.size == Style::Gradient::Explicit && g.radiusXPercent && near(g.radiusX, 0.5) && near(g.radiusXPlus, -2) &&
              g.radiusYPercent && near(g.radiusYPlus, 1) && !g.centerXPx && near(g.centerX, 1) && near(g.centerXPlus, -4) &&
              near(g.centerY, 0.25) && near(g.centerYPlus, 0));
    }
    // what still isn't a value: reported where it is, as before
    CHECK(s.problems.size() == 4);
    bool x1 = false, x2 = false, x3 = false, x4 = false;
    for (const Problem& p : s.problems) {
        x1 = x1 || (p.line == 13 && p.message.find("background-size") != std::string::npos);
        x2 = x2 || (p.line == 14 && p.message.find("background-size") != std::string::npos);
        x3 = x3 || (p.line == 15 && p.message.find("a circle's size is one length") != std::string::npos);
        x4 = x4 || (p.line == 16 && p.message.find("radial-gradient") != std::string::npos);
    }
    CHECK(x1 && x2 && x3 && x4);
    if (s.problems.size() != 4 || !x1 || !x2 || !x3 || !x4)
        for (const Problem& p : s.problems) printf("  %s\n", p.text().c_str());
    CHECK(s.engine.takeProblems().empty());

    // through var(): a bad value is reported by checkVarUses, for every rule, before any node is computed; once
    // (computing a node later doesn't report it again); a var() of a property declared elsewhere isn't checked
    {
        Sheet v(":root { --bad: calc(100% * 2px); --good: calc(100% - 1px); --none: ; }\n"   // 1
                "A { background-size: var(--bad) }\n"                                         // 2
                "B { background-image: linear-gradient(red var(--good), blue) }\n"           // 3
                "C { background: url(x.png) var(--bad) }\n"                                   // 4
                "D { --mine: 2px; background-size: var(--mine) }\n"                          // 5
                "E { background-size: var(--mine) }\n"                                        // 6
                "F { background-size: var(--missing) }\n"                                     // 7
                "@media (max-width: 10px) { G { background-size: var(--bad) } }\n");          // 8
        CHECK(v.problems.empty());
        v.engine.checkVarUses();
        std::vector<Problem> found = v.engine.takeProblems();
        std::set<int> lines;
        for (const Problem& p : found) lines.insert(p.line);
        CHECK((lines == std::set<int>{2, 4, 7}));  // (not 3: fine; not 5, 6: --mine is declared on D; not 8: @media)
        bool after = false;
        for (const Problem& p : found)
            after = after || (p.line == 2 && p.message.find("after var(); 'background-size' is left unset") != std::string::npos);
        CHECK(after);
        if ((lines != std::set<int>{2, 4, 7}) || !after)
            for (const Problem& p : found) printf("  %s\n", p.text().c_str());
        v.engine.compute(SimpleNode("A"));
        v.engine.compute(SimpleNode("F"));
        CHECK(v.engine.takeProblems().empty());  // (reported once)
        v.engine.compute(SimpleNode("E"));
        CHECK(v.engine.takeProblems().size() == 1);  // (E alone: --mine isn't there)
    }
}

// random and cut-short stylesheets: never a crash, never an exception, and quick
void testFuzz() {
    const std::string corpus =
        ":root { --a: #123; --b: var(--a, red); --c: calc(1px + 2em); }\n"
        "Button.x:hover > Slider::handle, #id .c * { color: color-mix(in srgb, var(--a) 30%, hsl(10 20% 30% / .5));"
        " padding: 1px var(--c) 3pt 4em !important; border: 1px solid currentColor; font-size: 120%;"
        " background: url(\"a\\\"b\") rgb(1, 2, 3, 50%); font-family: 'A B', \"C\\41\" }\n"
        "@media x { a { b: c } } /* comment */ A { margin: calc((1px + 2px) * 3 / 2) auto; }\n";
    const std::string alphabet = "{}();:,.#*>+~!@/\\'\"-_ \n\taZ09%()[]var(--x,calc(rgb(color-mix(in srgb,";
    std::mt19937 rng(777);
    SimpleNode root("Window");
    SimpleNode panel("Panel", &root);
    panel.classes = {"x", "c"};
    SimpleNode button("Button", &panel);
    button.idName = "id";
    button.states = {"hover"};
    SimpleNode slider("Slider", &button);
    auto exercise = [&](const std::string& text) {
        std::vector<Problem> ps;
        SheetPtr sheet = parse(text, "fuzz.css", ps);
        Engine e;
        e.setSheets({sheet, sheet});
        Style r = e.compute(root);
        Style p = e.compute(panel, &r);
        Style b = e.compute(button, &p);
        Style s = e.compute(slider, &b, "handle");
        for (const Style* st : {&r, &p, &b, &s}) {
            for (const auto& kv : st->props_) {
                st->color(kv.first, Rgba{});
                st->length(kv.first, 0, 100);
                st->number(kv.first, 0);
                st->keyword(kv.first, "");
                st->list(kv.first);
                Rgba c;
                parseColor(kv.second, c);
            }
            st->edges("padding", Edges{});
            st->edges("border-width", Edges{});
            st->radii(Edges{});
            st->fontSizePx(12);
        }
        e.takeProblems();
        for (const Problem& pr : ps) CHECK(pr.line >= 1 && pr.column >= 1);
    };
    int iterations = 0;
    for (size_t cut = 0; cut <= corpus.size(); cut++, iterations++) exercise(corpus.substr(0, cut));
    for (int i = 0; i < 3000; i++, iterations++) {
        std::string t;
        int kind = i % 3;
        if (kind == 0) {  // the corpus, mutated
            t = corpus;
            int edits = 1 + rng() % 8;
            for (int k = 0; k < edits && !t.empty(); k++) {
                size_t at = rng() % t.size();
                switch (rng() % 3) {
                    case 0: t.erase(at, 1 + rng() % 5); break;
                    case 1: t.insert(at, 1, alphabet[rng() % alphabet.size()]); break;
                    default: t[at] = (char)(rng() & 255); break;
                }
            }
        } else if (kind == 1) {  // pieces of CSS at random
            int n = rng() % 80;
            for (int k = 0; k < n; k++) {
                size_t from = rng() % alphabet.size();
                t += alphabet.substr(from, 1 + rng() % 6);
            }
        } else {  // bytes at random
            int n = rng() % 200;
            for (int k = 0; k < n; k++) t += (char)(rng() & 255);
        }
        exercise(t);
    }
    // deep nesting must not exhaust the stack
    exercise("A { width: " + std::string(100000, '(') + " }");
    exercise("A { width: calc(" + std::string(20000, '(') + "1px" + std::string(20000, ')') + ") }");
    std::string mixes = "red";
    for (int i = 0; i < 200; i++) mixes = "color-mix(in srgb, " + mixes + ", blue)";
    exercise("A { color: " + mixes + " }");
    Rgba c;
    CHECK(!parseColor(mixes, c));  // (too deep: refused, not followed)
    std::string chain = ":root { --v0: 1px";
    for (int i = 1; i < 2000; i++) chain += "; --v" + std::to_string(i) + ": var(--v" + std::to_string(i - 1) + ")";
    exercise(chain + "; width: var(--v1999) }");
    exercise(std::string(50000, '{'));
    exercise(std::string(50000, '}'));
    exercise("A " + std::string(5000, '>') + " B {}");
    iterations += 8;
    CHECK(iterations > 3000);
}

// ~2000 declarations, 1000 nodes
void testSpeed() {
    std::string text = ":root {";
    for (int i = 0; i < 200; i++) text += " --v" + std::to_string(i) + ": #" + std::to_string(100000 + i) + ";";
    text += " }\n";
    int decls = 200;
    const char* types[] = {"Button", "Slider", "Lane", "Clip", "Label", "Panel", "Knob", "Meter"};
    for (int i = 0; decls < 2000; i++) {
        std::string sel;
        switch (i % 5) {
            case 0: sel = types[i % 8]; break;
            case 1: sel = std::string(types[i % 8]) + ".c" + std::to_string(i % 20); break;
            case 2: sel = "Panel " + std::string(types[i % 8]) + ":hover"; break;
            case 3: sel = ".c" + std::to_string(i % 20) + " > " + types[(i + 3) % 8]; break;
            default: sel = "#n" + std::to_string(i % 50) + ", " + types[i % 8] + "::part" + std::to_string(i % 3); break;
        }
        text += sel + " { color: var(--v" + std::to_string(i % 200) + "); padding: " + std::to_string(i % 7) +
                "px 2px; border: 1px solid red; background-color: color-mix(in srgb, red " + std::to_string(i % 100) +
                "%, blue); font-size: calc(1em + 1px) }\n";
        decls += 5 + 3 + 12 - 2;  // (the shorthands' longhands count too)
    }
    std::vector<Problem> ps;
    auto t0 = std::chrono::steady_clock::now();
    SheetPtr sheet = parse(text, "big.css", ps);
    double parseTime = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ps.empty());
    Engine e;
    e.setSheets({sheet});
    std::vector<std::unique_ptr<SimpleNode>> nodes;
    nodes.push_back(std::make_unique<SimpleNode>("Window"));
    for (int i = 1; i < 1000; i++) {
        auto n = std::make_unique<SimpleNode>(types[i % 8], nodes[(i - 1) / 4].get());
        n->classes = {"c" + std::to_string(i % 20), "other"};
        if (i % 3 == 0) n->states = {"hover"};
        if (i % 7 == 0) n->idName = "n" + std::to_string(i % 50);
        nodes.push_back(std::move(n));
    }
    std::vector<Style> styles(nodes.size());
    t0 = std::chrono::steady_clock::now();
    styles[0] = e.compute(*nodes[0]);
    for (size_t i = 1; i < nodes.size(); i++) styles[i] = e.compute(*nodes[i], &styles[(i - 1) / 4]);
    double computeTime = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    int colored = 0;
    for (const Style& s : styles) colored += s.color("color", Rgba{}).a == 255;
    CHECK(colored > 500);
    CHECK(e.takeProblems().empty());
    printf("css speed: %zu bytes parsed in %.1f ms; 1000 styles computed in %.1f ms\n", text.size(), parseTime * 1000,
           computeTime * 1000);
#ifdef NDEBUG
    CHECK(computeTime < 0.1);
    CHECK(parseTime < 0.1);
#endif
}

}  // namespace

int main() {
    testTokenizer();
    testColors();
    testSelectors();
    testVariablesAndInheritance();
    testVariablesShared();
    testVarValuesCached();
    testLengthTextExact();
    testValues();
    testShorthands();
    testRecovery();
    testPositions();
    testNotIs();
    testSiblingCombinators();
    testMedia();
    testUnits();
    testImport();
    testFontFace();
    testPaintValues();
    testImages();
    testCalcMixed();
    testFuzz();
    testFuzzMore();
    testSpeed();
    TEST_MAIN_END();
}
