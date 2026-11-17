// The CSS engine (core/css.h): the tokenizer's edge cases, every colour form (color-mix() against the
// program's own mixColor, byte for byte), selectors and specificity, !important, the order of sheets,
// combinators, parts, states, :root variables, inheritance, var() (fallbacks, nesting, cycles), calc() and
// units, shorthands, error recovery and where problems are reported, random and cut-short stylesheets that
// must never crash, and the speed of computing many styles.
#include <chrono>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "core/css.h"
#include "core/css_internal.h"
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
        "A + B { color: red }\n"
        "A, :not(B) { color: red }\n"
        "A::x B { color: red }\n"
        "A::x.c { color: red }\n"
        "#1a { color: red }\n"
        "A > { color: red }\n"
        ". { color: red }\n"
        ", A { color: red }\n"
        "A { color: green }\n");
    CHECK(bad.problems.size() == 8);
    CHECK(bad.reported("'+' combinator is not supported"));
    CHECK(bad.reported("':not()' is not supported"));
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
    CHECK(same(b3.edges("border-width", Edges{}), 1, 2, 1, 2));
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
        "@media screen { A { color: red } }\n"                 // 2
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
    CHECK(at("@media", 2, 1));
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
    CHECK(ps.front().text() == "skin.css:2:1: @media rules are not supported (skipped)");  // (in source order)
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
    testValues();
    testShorthands();
    testRecovery();
    testFuzz();
    testSpeed();
    TEST_MAIN_END();
}
