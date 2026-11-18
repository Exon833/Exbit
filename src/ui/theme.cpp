#include "ui/theme.h"

#include <cmath>
#include <map>
#include <set>

#include "core/util.h"

namespace theme {

bool rounded = true;

const std::vector<ColorInfo>& colors() {
    static const std::vector<ColorInfo> table = {
        {"window", "#2b2d31", "Window background", "Window"},
        {"window_text", "#dcdde0", "Window text", "Window"},
        {"base", "#1e1f22", "Fields and lists", "Window"},
        {"alternate_base", "#2b2d31", "Lists, every other row", "Window"},
        {"text", "#dcdde0", "Text in fields and lists", "Window"},
        {"button", "#36383d", "Buttons", "Window"},
        {"button_text", "#dcdde0", "Button text", "Window"},
        {"highlight", "#3d7ab8", "Selected items", "Window"},
        {"highlighted_text", "#ffffff", "Selected items' text", "Window"},
        {"tooltip_base", "#1e1f22", "Tooltips", "Window"},
        {"tooltip_text", "#dcdde0", "Tooltip text", "Window"},
        {"disabled_text", "#6b6e75", "Disabled text", "Window"},
        {"hint_text", "#9a9da5", "Hints", "Window"},
        {"dimmed_text", "#70737c", "Dimmed notes (updates)", "Window"},
        {"accent", "#3d7ab8", "Lit buttons (FX with plugins, Preview, Metronome…)", "Window"},
        {"accent_text", "#ffffff", "Text on lit buttons", "Window"},
        {"frame_edge", "#111111", "Thin edges", "Window"},

        {"lane", "#232529", "Track lane", "Timeline"},
        {"lane_alt", "#282a2f", "Track lane, every other", "Timeline"},
        {"below_lanes", "#1d1e22", "Below the last track", "Timeline"},
        {"master_lane", "#1e2530", "Master lane and row", "Timeline"},
        {"master_divider", "#465a78", "Line above the master", "Timeline"},
        {"group_lane", "#33363e", "Group header (no colour of its own)", "Timeline"},
        {"group_accent", "#a0a0af", "Bar beside a group's tracks", "Timeline"},
        {"ruler_bg", "#18191c", "Ruler", "Timeline"},
        {"ruler_text", "#9696a0", "Ruler text", "Timeline"},
        {"grid_fine", "#2d2f35", "Grid: finer lines", "Timeline"},
        {"grid", "#373a40", "Grid: beats (and seconds)", "Timeline"},
        {"grid_bar", "#484c56", "Grid: bars", "Timeline"},
        {"playhead", "#ff5050", "Playback cursor", "Timeline"},
        {"cursor", "#5fafff", "Selection cursor", "Timeline"},
        {"selection_fill", "#78aae62d", "Selected range", "Timeline"},
        {"selection_edge", "#96c3f5c8", "Selected range's edges", "Timeline"},
        {"automation_lane", "#1c1f24", "Automation lane (and its row)", "Timeline"},
        {"automation_line", "#e0a84a", "Automation curve and its points", "Timeline"},
        {"loop", "#e0a33cb4", "Loop range on the ruler (looping on)", "Timeline"},
        {"loop_off", "#e0a33c46", "Loop range on the ruler (looping off)", "Timeline"},
        {"frozen_tint", "#bee1ff46", "Over a frozen track's clips", "Timeline"},
        {"overview_bg", "#141518", "Overview bar", "Timeline"},
        {"overview_view", "#c8d2e1c8", "Overview: outline of the view", "Timeline"},
        {"overview_view_fill", "#c8d2e11c", "Overview: inside the outline", "Timeline"},
        {"overview_edge", "#373a40", "Overview: line under it", "Timeline"},

        {"row", "#232529", "Track rows in the right-hand panel", "Tracks and clips"},
        {"row_alt", "#282a2f", "Track rows in the right-hand panel, every other", "Tracks and clips"},
        {"clip_audio", "#326996", "Audio clips (no colour of their own)", "Tracks and clips"},
        {"clip_midi", "#377d55", "MIDI clips (no colour of their own)", "Tracks and clips"},
        {"accent_audio", "#5096d2", "Audio track stripe", "Tracks and clips"},
        {"accent_midi", "#5abe78", "MIDI track stripe", "Tracks and clips"},
        {"accent_return", "#d0913c", "Return track stripe", "Tracks and clips"},
        {"mute", "#c0392b", "Mute (M) when on", "Tracks and clips"},
        {"solo", "#d4a017", "Solo (S) when on", "Tracks and clips"},
        {"record", "#d0433a", "Recording: ● Rec when on, the take as it grows", "Tracks and clips"},

        {"meter_bg", "#121316", "Meter background", "Meters"},
        {"meter_ok", "#46c86e", "Meter: up to -12 dB (and CPU load)", "Meters"},
        {"meter_warn", "#dcbe3c", "Meter: up to 0 dB", "Meters"},
        {"meter_hot", "#dc3c32", "Meter: at 0 dB", "Meters"},

        {"keys_white", "#e8e8ea", "White keys", "MIDI editor"},
        {"keys_black", "#1b1c1f", "Black keys", "MIDI editor"},
        {"keys_line", "#b8b9be", "Lines between keys", "MIDI editor"},
        {"keys_label", "#3a3c42", "Key names (C4…)", "MIDI editor"},
        {"key_sounding", "#5fafff96", "The key sounding", "MIDI editor"},
        {"roll_shade", "#0000006e", "Outside the clip", "MIDI editor"},
        {"selected_note", "#ffffff", "Selected notes", "MIDI editor"},

        {"sample_wave", "#78afe6", "Sample waveform", "Explorer"},
    };
    return table;
}

namespace {
std::map<std::string, double> metrics;
std::set<std::string> installed;  // font families this computer has, lower case (read again with the sheets)  // sizes asked for (NaN: not set)
std::map<std::string, Color> current;  // colours by name: the table's, then what the stylesheets set
Fonts currentFonts;
Shades currentShades;
css::Engine engine;
bool ready = false;
const Color kMissing(255, 0, 255);

Color toColor(const css::Rgba& c) { return Color(c.r, c.g, c.b, c.a); }

// a variable of :root as a colour; false if there's none or it isn't one
bool rootColor(const std::string& variable, Color& out) {
    std::optional<std::string> v = engine.root().variable(variable);
    css::Rgba rgba;
    if (!v || !css::parseColor(*v, rgba)) return false;
    out = toColor(rgba);
    return true;
}

// "" for the interface's font, "monospace" for the numbers font, "-" for not a generic family
std::string genericFamily(const std::string& name) {
    std::string key = util::lower(util::trim(name));
    if (key == "system-ui" || key == "sans-serif" || key == "serif" || key == "ui-sans-serif" || key == "ui-serif" ||
        key == "-apple-system" || key == "blinkmacsystemfont")
        return "";
    if (key == "monospace" || key == "ui-monospace") return "monospace";
    return "-";
}

void refresh() {
    installed.clear();
    for (const std::string& name : Font::families()) installed.insert(util::lower(name));
    current.clear();
    metrics.clear();
    for (auto& info : colors()) {
        Color col = Color::fromHex(info.def);
        rootColor(variableName(info.name), col);
        current[info.name] = col;
    }
    auto& q = current;
    struct { Color Shades::*field; const char* var; const char* a; const char* b; float t; } mixes[] = {
        {&Shades::hover, "--hover", "button", "button_text", 0.10f},
        {&Shades::pressed, "--pressed", "button", "button_text", 0.18f},
        {&Shades::disabled, "--disabled", "button", "window", 0.5f},
        {&Shades::line, "--line", "window", "window_text", 0.10f},
        {&Shades::fieldEdge, "--field-edge", "base", "text", 0.14f},
        {&Shades::fieldHover, "--field-hover", "base", "text", 0.26f},
        {&Shades::itemHover, "--item-hover", "base", "text", 0.07f},
        {&Shades::panel, "--panel", "window", "base", 0.45f},
        {&Shades::handle, "--handle", "window", "window_text", 0.22f},
        {&Shades::handleHover, "--handle-hover", "window", "window_text", 0.38f},
        {&Shades::accentHover, "--accent-hover", "accent", "accent_text", 0.15f},
    };
    for (auto& m : mixes)
        if (!rootColor(m.var, currentShades.*m.field)) currentShades.*m.field = mixColor(q[m.a], q[m.b], m.t);

    // fonts: font-family's first name ("system-ui" and the generic families: the system's own), font-size,
    // --mono-font
    const css::Style& root = engine.root();
    Fonts f;
    std::string family = pickFamily(root.list("font-family"));
    f.ui = family == "monospace" ? Font::mono().family() : family;
    if (root.has("font-size")) {
        double pt = root.fontSizePx(12) * 0.75;  // (px are 1/96 in, pt 1/72)
        if (pt > 0 && pt <= 40) f.uiSize = std::round(pt * 100) / 100;
    }
    if (std::optional<std::string> mono = root.variable("--mono-font")) {
        std::string m = css::unquote(*mono);
        if (util::lower(m) == "monospace") m = "Consolas";  // (Consolas falls back to a monospaced font)
        if (!m.empty()) f.mono = m;
    }
    currentFonts = f;
    Font::setDefaults(f.ui, f.uiSize > 0 ? f.uiSize : 9.0, f.mono.empty() ? "Consolas" : f.mono);
    ready = true;
}
}  // namespace

std::vector<std::string> colorVariables() {
    std::vector<std::string> out;
    for (auto& info : colors()) out.push_back(variableName(info.name));
    for (const char* v : {"--hover", "--pressed", "--disabled", "--line", "--field-edge", "--field-hover", "--item-hover",
                          "--panel", "--handle", "--handle-hover", "--accent-hover"})
        out.push_back(v);
    return out;
}

std::string variableName(const std::string& name) {
    std::string v = "--" + name;
    for (char& ch : v)
        if (ch == '_') ch = '-';
    return v;
}

const Color& c(const std::string& name) {
    if (!ready) setSkin();
    auto it = current.find(name);
    if (it != current.end()) return it->second;
    Color col;  // a variable the skin added (kept once found; the table's names are all there already)
    if (!engine.sheets().empty() && rootColor(variableName(name), col)) return current.emplace(name, col).first->second;
    return kMissing;
}

css::SheetPtr builtinSheet() {
    static const css::SheetPtr sheet = [] {
        static const char* const text =
#include "default_css.inc"
            ;
        std::vector<css::Problem> problems;
        return css::parse(text, "default.css (built in)", problems);
    }();
    return sheet;
}

void setStyles(std::vector<css::SheetPtr> sheets, std::vector<css::Problem>* problems) {
    if (sheets.empty()) sheets.push_back(builtinSheet());
    // (ch: the width of "0" in the node's font; no font-family: the interface's)
    engine.setCharWidth([](const std::string& family, double px) {
        return (double)Font(family.empty() ? Font::ui().family() : family, (float)px).charWidth('0');
    });
    engine.setSheets(std::move(sheets));
    refresh();
    std::vector<css::Problem> found = engine.takeProblems();
    if (problems) problems->insert(problems->end(), found.begin(), found.end());
}

void setSkin() { setStyles({}); }

double metricF(const std::string& name, double def) {
    if (!ready) setSkin();
    std::string var = name.compare(0, 2, "--") == 0 ? name : "--" + name;
    auto it = metrics.find(var);
    if (it == metrics.end()) {
        double v = std::nan("");
        if (std::optional<std::string> text = engine.root().variable(var)) {
            css::Style probe;  // (the variable's text as a property, read as a length)
            probe.props_["width"] = *text;
            probe.emPx_ = engine.root().emPx_;
            if (probe.has("width")) v = probe.length("width", std::nan(""));
        }
        it = metrics.emplace(var, v).first;
    }
    return std::isnan(it->second) ? def : it->second;
}

std::string pickFamily(const std::vector<std::string>& families) {
    if (families.empty()) return "";
    if (installed.empty())
        for (const std::string& name : Font::families()) installed.insert(util::lower(name));
    for (const std::string& name : families) {
        std::string generic = genericFamily(name);
        if (generic != "-") return generic;
        if (installed.count(util::lower(util::trim(name)))) return util::trim(name);
    }
    std::string generic = genericFamily(families[0]);
    return generic != "-" ? generic : util::trim(families[0]);
}

int metric(const std::string& name, int def) { return (int)std::lround(metricF(name, def)); }

bool setMedia(const css::Media& media) {
    if (!ready) setSkin();
    int before = engine.generation();
    engine.setMedia(media);
    if (engine.generation() == before) return false;
    refresh();
    return true;
}

const css::Engine& styles() {
    if (!ready) setSkin();
    return engine;
}

const Fonts& fonts() {
    if (!ready) setSkin();
    return currentFonts;
}

const Shades& shades() {
    if (!ready) setSkin();
    return currentShades;
}

Color toggleColor(const std::string& role) { return c(role); }
Color toggleHover(const std::string& role) { return mixColor(c(role), c("accent_text"), 0.15f); }

Font uiFont() { return Font::ui(); }

Font monoFont(double points) {
    const std::string& m = fonts().mono;
    return Font::fromPoints(m.empty() ? "Consolas" : m, (float)points);
}

Font dimmedFont() { return Font::ui().withPointSize(8); }

}  // namespace theme
