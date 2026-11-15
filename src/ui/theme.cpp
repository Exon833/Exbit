#include "ui/theme.h"

#include <map>

namespace theme {

const char* const FONT_LABEL_UI = "Interface font";
const char* const FONT_LABEL_UI_SIZE = "Interface font size (pt)";
const char* const FONT_LABEL_MONO = "Numbers font (times, LUFS)";

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
std::map<std::string, Color> current;
Fonts currentFonts;
Shades currentShades;
const Color kMissing(255, 0, 255);
}  // namespace

const Color& c(const std::string& name) {
    if (current.empty()) setSkin();
    auto it = current.find(name);
    return it == current.end() ? kMissing : it->second;
}

void setSkin(const std::vector<std::pair<std::string, std::string>>& cols, const Fonts& f) {
    current.clear();
    for (auto& info : colors()) current[info.name] = Color::fromHex(info.def);
    for (auto& kv : cols) {
        bool ok = false;
        Color col = Color::fromHex(kv.second, &ok);
        auto it = current.find(kv.first);
        if (ok && it != current.end() && (kv.second.size() == 7 || kv.second.size() == 9)) it->second = col;
    }
    currentFonts = f;
    auto& q = current;
    Shades& s = currentShades;
    s.hover = mixColor(q["button"], q["button_text"], 0.10f);
    s.pressed = mixColor(q["button"], q["button_text"], 0.18f);
    s.disabled = mixColor(q["button"], q["window"], 0.5f);
    s.line = mixColor(q["window"], q["window_text"], 0.10f);
    s.fieldEdge = mixColor(q["base"], q["text"], 0.14f);
    s.fieldHover = mixColor(q["base"], q["text"], 0.26f);
    s.itemHover = mixColor(q["base"], q["text"], 0.07f);
    s.panel = mixColor(q["window"], q["base"], 0.45f);
    s.handle = mixColor(q["window"], q["window_text"], 0.22f);
    s.handleHover = mixColor(q["window"], q["window_text"], 0.38f);
    s.accentHover = mixColor(q["accent"], q["accent_text"], 0.15f);
    Font::setDefaults(f.ui, f.uiSize > 0 ? f.uiSize : 9.0, f.mono.empty() ? "Consolas" : f.mono);
}

const Fonts& fonts() {
    if (current.empty()) setSkin();
    return currentFonts;
}

const Shades& shades() {
    if (current.empty()) setSkin();
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
