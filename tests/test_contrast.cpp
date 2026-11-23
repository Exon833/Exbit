// Contrast in every skin (headless; rule 8's worst case for reading the interface): every control in every state,
// the colours the program draws with, the meters, the keyboard focus, the symbols' glyphs, the main window and the
// newer windows. It fails when a pair is under its WCAG 2.2 threshold:
//   text against what is behind it 4.5:1; one or two characters (M, S, ●, ✕) and large text 3:1     (1.4.3)
//   icons, control edges that are the only sign of the control, focus indicators, meters' bars, the slider's
//   handle, selection highlights, the playhead and cursor 3:1                                         (1.4.11)
//   the focus indicator: pixels that change by 3:1 between focused and not, at least a ring's worth  (2.4.13)
//   disabled text: exempt (1.4.3), but still legible (1.8:1) and unlike the enabled text
//   a state against another (checked against unchecked, a hovered row): WCAG asks no ratio of colours that are
//   never side by side, only that they differ (here: 1.25:1 or a visibly different box)
//   grid lines: the ruler carries what they say (1.4.11 asks nothing of them), visible all the same: 1.3:1
// Measured from the colours the stylesheets give each control in each state (its box painted as the toolkit paints
// it: gradients, pictures, bevels; the text against the pixels behind it, the 15th percentile), and from the
// pixels painted where the program draws with its own colours (the meters, the focus).
// Every pair measured: build/ui-samples/contrast-audit/report.md; pictures beside it (each skin's main window,
// a sheet of every control in every state, zoomed, and the focus rings).
#define SKIN_HUNT_NO_MAIN
#include "test_skin_hunt.cpp"

namespace ca {
using namespace hunt;

const std::string OUT = "build/ui-samples/contrast-audit";
std::string skinNow;
std::vector<std::string> fails;
std::ofstream rep;
int pairs = 0;

void judgePair(const std::string& what, double cr, double need, const std::string& detail) {
    pairs++;
    bool bad = cr + 1e-9 < need;
    std::string needs = need > 100 ? "" : " (needs " + r2(need) + ")";  // (a finding of the hunt's: its detail says)
    rep << (bad ? "- **FAIL** " : "- ok ") << skinNow << " · " << what << ": " << r2(cr) << ":1" << needs << " " << detail << "\n";
    if (bad) fails.push_back(skinNow + " · " + what + ": " + r2(cr) + ":1" + needs + " " + detail);
}

// --- a box painted in a style, and the pixels inside it a text would be drawn on ---
struct Box {
    Image img;
    std::vector<Color> px;  // inside its border and padding
    Color mode;
};
Box paintBox(const css::Style& s, int w, int h, Color around) {
    Box b;
    b.img = boxPicture(s, w, h, 1, around);
    Rect c = ui::style::contents(Rect(0, 0, w, h), s).intersected(Rect(0, 0, w, h));
    if (c.w < 4 || c.h < 4) c = Rect(2, 2, w - 4, h - 4);  // (a glyph button's: its whole inside)
    else if (c.w > 8 && c.h > 8) c = c.adjusted(1, 1, -1, -1);  // (a text is never on the inside's outermost pixels)
    for (int y = c.y; y < c.bottom(); y++)
        for (int x = c.x; x < c.right(); x++) {
            Color p = b.img.colorAt(x, y);
            p.a = 255;
            b.px.push_back(p);
        }
    b.mode = modeColor(b.img, c);
    return b;
}
// a colour against the pixels it is drawn on: its 15th percentile (a bevel's line or a gradient's far end may be under a
// letter, not all of it)
double against(Color fg, const std::vector<Color>& px, Color* behind = nullptr) {
    if (px.empty()) return 21;
    std::vector<std::pair<double, Color>> v;
    v.reserve(px.size());
    for (Color p : px) v.push_back({contrast(over(fg, p), p), p});
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first < b.first; });
    auto& q = v[v.size() * 15 / 100];  // (the 15th percentile: a bevel's or a focus line's rows are not under the text)
    if (behind) *behind = q.second;
    return q.first;
}

// a widget's style in these states (instead of its own); `part`: one of its parts in them
css::Style styleIn(const Widget* w, const std::vector<std::string>& states, const std::string& part = "") {
    std::string s;
    for (auto& x : states) s += x + " ";
    panel_style::StateNode node(*w, s);
    css::Style self = theme::styles().compute(node, w->parent() ? &w->parent()->computedStyle() : nullptr);
    if (part.empty()) return self;
    return theme::styles().compute(node, &self, part);
}
std::vector<std::string> statesOf(const Widget* w, std::vector<std::string> extra) {
    std::vector<std::string> st;
    w->styleStates(st);
    st.erase(std::remove_if(st.begin(), st.end(), [](const std::string& x) { return x == "hover" || x == "pressed" || x == "focus"; }),
             st.end());
    for (auto& e : extra)
        if (std::find(st.begin(), st.end(), e) == st.end()) st.push_back(e);
    return st;
}

// --- the sheet of every control in every state (a picture to look at) ---
struct Sheet {
    Image img;
    int y = 4;
    Sheet() : img(1300, 2200, Color(128, 128, 128)) {}
    int x = 4, rowH = 0;
    void put(const Image& cell, const std::string& label) {
        (void)label;
        if (x + cell.width() > img.width() - 4) newRow();
        paste(img, cell, x, y);
        x += cell.width() + 4;
        rowH = std::max(rowH, cell.height());
    }
    void newRow() {
        if (x > 4) y += rowH + 4;
        x = 4;
        rowH = 0;
    }
};
Sheet* sheet = nullptr;

// a control in one state: its box, its text (or icon) on it, measured; and a cell of the sheet
struct Look {
    Box box;
    Color fg;
    double cr = 0;
};
Look stateCell(Widget* w, const std::string& text, const std::string& icon, const std::string& stateName,
               const std::vector<std::string>& states, Color around, double need) {
    Look l;
    css::Style s = styleIn(w, states);
    l.box = paintBox(s, w->width(), w->height(), around);
    l.fg = ui::style::foreground(icon.empty() ? s : styleIn(w, states, "icon"));
    Color behind;
    l.cr = against(l.fg, l.box.px, &behind);
    bool disabled = std::find(states.begin(), states.end(), "disabled") != states.end();
    std::string what = w->type() + (w->objectName.empty() ? "" : "#" + w->objectName);
    for (const char* p : {"toggle", "accent", "autoRaise", "hint", "dimmed"}) {
        std::string v = w->property(p);
        if (!v.empty() && v != "false") what += "." + std::string(p) + (v == "true" ? "" : "-" + v);
    }
    what += " \"" + (icon.empty() ? text : icon + " icon") + "\" :" + stateName;
    judgePair(what, l.cr, disabled ? 1.8 : need,
              std::string(icon.empty() ? "text " : "icon ") + hx(l.fg) + " on " + hx(behind));
    if (sheet) {
        Image cell = l.box.img;
        Painter p(cell, 1);
        if (!icon.empty()) {
            const Image& ic = transport_bar::tinted(icon, l.fg, std::min(w->width(), w->height()) * 2 / 3);
            if (!ic.empty()) p.drawImage(Point((cell.width() - ic.width()) / 2, (cell.height() - ic.height()) / 2), ic);
        } else {
            Font f = ui::style::font(s, theme::uiFont());
            p.drawText(RectF(ui::style::contents(Rect(0, 0, w->width(), w->height()), s)), AlignCenter, text, f, l.fg);
        }
        sheet->put(cell, what);
    }
    return l;
}

// the states of a button (or check box); and against its other look (checked / unchecked)
void sweepButton(Widget* w, const std::string& text, const std::string& icon, Color around) {
    double need = icon.empty() ? needed(text, ui::style::font(w->computedStyle(), theme::uiFont())) : 3.0;
    auto* b = dynamic_cast<Button*>(w);
    auto* cb = dynamic_cast<CheckBox*>(w);
    bool checkable = (b && b->isCheckable()) || cb;
    for (int checked = 0; checked < (checkable ? 2 : 1); checked++) {
        if (b && checkable) b->setCheckedSilently(checked);
        if (cb) cb->setCheckedSilently(checked);
        std::string pre = checkable ? (checked ? "checked" : "unchecked") : "";
        auto name = [&](const char* s) { return pre.empty() ? std::string(s) : pre + (*s ? ":" + std::string(s) : ""); };
        if (cb) {  // (a check box's text is on the window; its box is its indicator)
            Look n = stateCell(w, text, "", name(""), statesOf(w, {}), around, need);
            css::Style is = styleIn(w, statesOf(w, {}), "indicator");
            Box ib = paintBox(is, 16, 16, around);
            // the box's edge (or its fill, checked) against the window: the only sign of the control
            double edge = 1;
            for (int y = 0; y < ib.img.height(); y++)
                for (int x = 0; x < ib.img.width(); x++) edge = std::max(edge, contrast(ib.img.colorAt(x, y), around));
            judgePair("CheckBox::indicator :" + name(""), edge, 3.0, "its edge or fill against " + hx(around));
            if (checked) {
                double mark = contrast(ui::style::foreground(is), ib.mode);
                judgePair("CheckBox::indicator check mark :checked", mark, 3.0,
                          hx(ui::style::foreground(is)) + " on " + hx(ib.mode));
            }
            (void)n;
            continue;
        }
        Look n = stateCell(w, text, icon, name("normal"), statesOf(w, {}), around, need);
        Look h = stateCell(w, text, icon, name("hover"), statesOf(w, {"hover"}), around, need);
        stateCell(w, text, icon, name("pressed"), statesOf(w, {"hover", "pressed"}), around, need);
        // a lit toggle under the mouse keeps its role's colour (a hovered M must not look like an S): its hue
        if (checked && !w->property("toggle").empty()) {
            auto hue = [](Color c) {
                double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0, mx = std::max({r, g, b}), mn = std::min({r, g, b});
                if (mx - mn < 0.08) return -1.0;  // (grey: no hue)
                double d = mx - mn, hh = mx == r ? std::fmod((g - b) / d, 6.0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
                return std::fmod(hh * 60 + 360, 360.0);
            };
            double a = hue(n.box.mode), b = hue(h.box.mode);
            double dh = a < 0 || b < 0 ? 0 : std::min(std::fabs(a - b), 360 - std::fabs(a - b));
            judgePair(w->type() + " \"" + text + "\" lit, under the mouse: the same colour", 30.0 / std::max(1.0, dh), 1.0,
                      "fills " + hx(n.box.mode) + " / " + hx(h.box.mode) + ", hues " + r2(dh) + "° apart (at most 30)");
        }
        if (!checked) {
            // disabled: legible, and unlike the enabled control
            w->setEnabled(false);
            Look d = stateCell(w, text, icon, name("disabled"), statesOf(w, {}), around, need);
            w->setEnabled(true);
            double crDiff = std::max(n.cr, d.cr) / std::max(0.01, std::min(n.cr, d.cr));
            double boxDiff = meanDiff(n.box.img, d.box.img);
            judgePair(w->type() + " \"" + text + icon + "\" disabled against enabled", std::max(crDiff, contrast(n.fg, d.fg)), 1.25,
                      "text " + r2(n.cr) + ":1 enabled, " + r2(d.cr) + ":1 disabled; boxes " + r2(boxDiff) + "/255 apart");
        }
    }
    if (b && checkable) {
        b->setCheckedSilently(false);
        css::Style u = styleIn(w, statesOf(w, {}));
        Box ub = paintBox(u, w->width(), w->height(), around);
        Color uf = ui::style::foreground(icon.empty() ? u : styleIn(w, statesOf(w, {}), "icon"));
        b->setCheckedSilently(true);
        css::Style c = styleIn(w, statesOf(w, {}));
        Box cbx = paintBox(c, w->width(), w->height(), around);
        Color cf = ui::style::foreground(icon.empty() ? c : styleIn(w, statesOf(w, {}), "icon"));
        b->setCheckedSilently(false);
        double d = std::max({contrast(ub.mode, cbx.mode), contrast(uf, cf), meanDiff(ub.img, cbx.img) >= 12 ? 9.0 : 1.0});
        judgePair(w->type() + " \"" + text + icon + "\" checked against unchecked", d, 1.25,
                  "fills " + hx(cbx.mode) + " / " + hx(ub.mode) + ", text " + hx(cf) + " / " + hx(uf) + ", boxes " +
                      r2(meanDiff(ub.img, cbx.img)) + "/255 apart");
    }
    if (sheet) sheet->newRow();
}

Sheet* focusSheet = nullptr;  // (a picture of each: unfocused over focused)
// --- keyboard focus: the pixels around the control, focused against not ---
void sweepFocus(Window& win, Widget* w, const std::string& what) {
    App::get().doLayouts();
    auto grab = [&] {
        win.invalidate(win.outerRect());
        win.flush();
        Rect d = deviceRectOf(w).adjusted(-3, -3, 3, 3).intersected(Rect(0, 0, win.framebuffer().width(), win.framebuffer().height()));
        Image img(d.w, d.h);
        for (int y = 0; y < d.h; y++)
            for (int x = 0; x < d.w; x++) img.row(y)[x] = win.framebuffer().pixel(d.x + x, d.y + y);
        return img;
    };
    w->clearFocus();
    Image a = grab();
    w->setFocus();
    Image b = grab();
    bool focused = w->hasFocus() || (w->focusProxy && w->focusProxy->hasFocus());
    w->clearFocus();
    int n = 0;
    double best = 1;
    for (int y = 0; y < a.height(); y++)
        for (int x = 0; x < a.width(); x++) {
            double c = contrast(a.colorAt(x, y), b.colorAt(x, y));
            best = std::max(best, c);
            if (c >= 3) n++;
        }
    int ring = std::min(40, w->width() + w->height());
    // (a picture: unfocused over focused, 3x)
    Image z(a.width() * 3, a.height() * 6 + 3, Color(255, 0, 255));
    for (int y = 0; y < a.height() * 3; y++)
        for (int x = 0; x < a.width() * 3; x++) {
            z.row(y)[x] = a.pixel(x / 3, y / 3);
            z.row(y + a.height() * 3 + 3)[x] = b.pixel(x / 3, y / 3);
        }
    if (focusSheet) focusSheet->put(z, what);
    judgePair("focus ring: " + what, focused ? (n >= ring ? 3.0 : (double)n / ring * 3.0) : 0, 3.0,
              std::to_string(n) + " pixels change by 3:1 or more (a ring needs " + std::to_string(ring) + "); the most: " + r2(best) +
                  ":1" + (focused ? "" : "; it took no focus"));
}

// --- the specimen: every kind of control, in a window of the skin ---
void sweepControls() {
    Window win(plat::WindowOptions{});
    win.resizeWindow(900, 400);
    Color around = theme::c("window");
    struct Item { Widget* w; std::string text, icon; };
    std::vector<Item> items;
    auto at = [](Widget* w, int x, int y, int ww, int hh) { w->setGeometry(Rect(x, y, ww, hh)); return w; };
    auto* push = win.add<Button>("Apply");
    at(push, 10, 10, 90, 26);
    items.push_back({push, "Apply", ""});
    auto* def = win.add<Button>("OK");
    def->setDefault(true);
    at(def, 110, 10, 90, 26);
    items.push_back({def, "OK", ""});
    auto tool = [&](const std::string& text, std::function<void(Button*)> set, int x, int y, int ww = 80) {
        auto* b = win.add<Button>(text);
        b->setStyle(Button::Style::Tool);
        set(b);
        at(b, x, y, ww, 24);
        items.push_back({b, text, ""});
        return b;
    };
    tool("Tool", [](Button*) {}, 210, 10);
    tool("Grid", [](Button* b) { b->setCheckable(true); }, 300, 10);
    tool("Tap", [](Button* b) { b->setProperty("autoRaise", "true"); }, 390, 10);
    tool("\xe2\x99\xa9 Beats", [](Button* b) { b->setProperty("autoRaise", "true"); b->setCheckable(true); }, 480, 10);
    tool("Metronome", [](Button* b) { b->setProperty("accent", "true"); b->setCheckable(true); }, 570, 10, 90);
    tool("Write", [](Button* b) { b->setCheckable(true); b->setProperty("toggle", "record"); }, 670, 10, 50);
    int tx = 10;
    for (auto [t, role] : std::vector<std::pair<const char*, const char*>>{{"M", "mute"}, {"S", "solo"}, {"\xe2\x97\x8f", "record"}, {"I", "accent"}}) {
        auto* b = widgets::toggleButton(&win, t, role, false, nullptr);
        at(b, tx, 46, 26, 22);
        items.push_back({b, t, ""});
        tx += 32;
    }
    int ix = 150;
    for (const char* icon : {"play", "record", "loop", "follow"}) {
        auto* b = win.add<IconButton>(icon, "?");
        b->setProperty("autoRaise", "true");
        if (std::string(icon) == "loop" || std::string(icon) == "follow") b->setCheckable(true);
        b->move(ix, 42);
        items.push_back({b, "", icon});
        ix += b->width() + 6;
    }
    {  // a MIDI track's instrument button: its icon (the piano keyboard) in the button's text colour, a split ▾
        auto* inst = widgets::instrumentButton(&win, nullptr, nullptr, nullptr, -1, nullptr, nullptr, false);
        inst->move(ix + 6, 46);
        items.push_back({inst, "", ""});
    }
    auto* check = win.add<CheckBox>("Snap");
    at(check, 10, 80, 120, 22);
    items.push_back({check, "Snap", ""});
    // labels, fields, a slider and a list: in their normal state, and focused
    auto* label = win.add<Label>("A label");
    at(label, 10, 110, 120, 20);
    auto* hint = win.add<Label>("A hint");
    hint->setProperty("hint", "true");
    at(hint, 140, 110, 120, 20);
    auto* dim = win.add<Label>("A dimmed note");
    dim->setProperty("dimmed", "true");
    at(dim, 270, 110, 140, 20);
    auto* edit = win.add<LineEdit>("Text");
    at(edit, 10, 140, 140, 24);
    auto* ph = win.add<LineEdit>("");
    ph->setPlaceholder("Search samples...");
    at(ph, 160, 140, 160, 24);
    auto* spin = win.add<SpinBox>();
    at(spin, 330, 140, 100, 24);
    auto* combo = win.add<ComboBox>();
    combo->addItem("Music");
    combo->addItem("Drums");
    at(combo, 440, 140, 120, 24);
    auto* slider = win.add<Slider>(Orientation::Horizontal);
    slider->setRange(0, 100);
    slider->setValue(60);
    at(slider, 570, 142, 140, 20);
    auto* list = win.add<ListWidget>();
    for (const char* t : {"Drums 1.wav", "Bass.wav", "Rhythm guitar.wav", "Pad.wav"}) list->addItem(t);
    list->setCurrentRow(1);
    at(list, 10, 180, 220, 100);
    win.showWindow();
    App::get().doLayouts();
    win.invalidate(win.outerRect());
    win.flush();
    savePng(OUT + "/" + fileSafe(skinNow) + "-controls.png", win.framebuffer());

    // every button and check box in every state
    Sheet sh;
    sheet = &sh;
    for (auto& it : items) sweepButton(it.w, it.text, it.icon, around);
    sheet = nullptr;
    // (the sheet cut to what was drawn, 2x)
    {
        int h = std::min(sh.img.height(), sh.y + sh.rowH + 4);
        Image z(sh.img.width() * 2, h * 2);
        for (int y = 0; y < z.height(); y++)
            for (int x = 0; x < z.width(); x++) z.row(y)[x] = sh.img.pixel(x / 2, y / 2);
        savePng(OUT + "/" + fileSafe(skinNow) + "-states.png", z);
    }

    // labels on the window
    for (auto [l, what] : std::vector<std::pair<Label*, const char*>>{{label, "Label"}, {hint, "Label.hint"}, {dim, "Label.dimmed"}}) {
        Color fg = ui::style::foreground(l->computedStyle());
        judgePair(std::string(what), contrast(over(fg, around), around), needed("text", l->font()), hx(fg) + " on " + hx(around));
        l->setEnabled(false);
        Color dfg = ui::style::foreground(l->computedStyle());
        l->setEnabled(true);
        judgePair(std::string(what) + " :disabled", contrast(over(dfg, around), around), 1.8, hx(dfg) + " on " + hx(around));
    }
    // fields: their text, the placeholder, their edge against the window (the only sign of a field on a window of its colour)
    for (auto [w, what] : std::vector<std::pair<Widget*, const char*>>{{edit, "LineEdit"}, {spin, "SpinBox"}, {combo, "ComboBox"}}) {
        css::Style s = w->computedStyle();
        Box b = paintBox(s, w->width(), w->height(), around);
        Color fg = ui::style::foreground(s);
        Color behind;
        double cr = against(fg, b.px, &behind);
        judgePair(std::string(what) + " text", cr, 4.5, hx(fg) + " on " + hx(behind));
        double edge = 1;
        for (int y = 0; y < b.img.height(); y++)
            for (int x = 0; x < b.img.width(); x++) edge = std::max(edge, contrast(b.img.colorAt(x, y), around));
        judgePair(std::string(what) + " edge or fill against the window", edge, 1.25, "fill " + hx(b.mode) + " on " + hx(around));
    }
    {
        PartStyles parts(*ph);
        Color pc = ui::style::foreground(parts.get("placeholder"));
        Box b = paintBox(ph->computedStyle(), ph->width(), ph->height(), around);
        judgePair("LineEdit::placeholder", against(pc, b.px), 4.5, hx(pc) + " on " + hx(b.mode));
    }
    // the slider: its handle against what is around it; the part below the value against the groove's
    {
        // (each part painted as a box, a picture's too: its most distinct pixel, an edge or its fill)
        auto most = [](const css::Style& s, int w, int h, Color base) {
            Image img = boxPicture(s, std::max(2, w), std::max(2, h), 1, base);
            double m = 1;
            for (int y = 0; y < img.height(); y++)
                for (int x = 0; x < img.width(); x++) m = std::max(m, contrast(img.colorAt(x, y), base));
            return m;
        };
        PartStyles parts(*slider);
        const css::Style &hs = parts.get("handle"), &fs = parts.get("fill");
        int hw = ui::style::px(hs, "width", 10), hh = ui::style::px(hs, "height", 10);
        Color fill = paintBox(fs, 40, 8, around).mode;  // (the handle reaches past the groove: against the window, not the fill)
        judgePair("Slider::handle against the window", most(hs, hw, hh, around), 3.0, "its box on " + hx(around));
        // (a skin without a fill, the classic trackbar: the handle alone shows the value)
        if (most(fs, 40, 4, around) > 1.05) judgePair("Slider::fill against the window", most(fs, 40, 4, around), 3.0, hx(fill) + " on " + hx(around));
        slider->setEnabled(false);
        PartStyles dparts(*slider);
        double dh = most(dparts.get("handle"), hw, hh, around);
        slider->setEnabled(true);
        judgePair("Slider::handle :disabled against the window", dh, 1.8, "its box on " + hx(around));
    }
    // scroll bars (ScrollArea's and the windows' own: the same styles): the thumb, its edge or its fill, against the
    // bar's track, in each of its states, both ways
    for (Orientation o : {Orientation::Vertical, Orientation::Horizontal}) {
        auto* sb = win.add<ScrollBar>(o);
        bool vert = o == Orientation::Vertical;
        sb->setGeometry(vert ? Rect(860, 10, theme::scrollbarWidth(), 200) : Rect(300, 370, 200, theme::scrollbarWidth()));
        Box track = paintBox(sb->computedStyle(), sb->width(), sb->height(), around);
        PartStyles parts(*sb);
        for (const char* st : {"", "hover", "pressed"}) {
            std::vector<const char*> sts;
            if (*st) sts.push_back(st);
            const css::Style& ts = parts.get("thumb", sts);
            int tw = vert ? sb->width() : 60, th = vert ? 60 : sb->height();
            Image img(tw + 4, th + 4, track.mode);  // (room around it: an outer shadow is its edge in some skins)
            {
                Painter p(img, 1);
                ui::style::box(p, Rect(2, 2, tw, th), ts);
            }
            double m = 1;
            for (int y = 0; y < img.height(); y++)
                for (int x = 0; x < img.width(); x++) m = std::max(m, contrast(img.colorAt(x, y), track.mode));
            judgePair(std::string("ScrollBar::thumb (") + (vert ? "vertical" : "horizontal") + ") :" + (*st ? st : "normal") +
                          " against its track", m, 3.0, "its box on " + hx(track.mode));
        }
        sb->hide();
    }
    // the list's rows: text on each kind of row, and the selected row against its neighbours (side by side: 3:1, or
    // an edge around it that is)
    {
        panel_style::Parts parts(*list);
        Color listBg = over(ui::style::background(list->computedStyle()), around);
        int rw = list->width() - 4, rh = std::max(8, ui::style::px(parts.get("item"), "height", 22));
        std::map<std::string, Box> rows;
        for (const char* st : {"", "alternate", "hover", "selected", "selected hover", "selected focus", "disabled"}) {
            const css::Style& is = parts.get("item", st);
            Box b = paintBox(is, rw, rh, listBg);
            rows[st] = b;
            Color fg = ui::style::foreground(is), behind;
            bool disabled = std::string(st) == "disabled";
            double cr = against(fg, b.px, &behind);
            judgePair(std::string("ListWidget::item :") + (*st ? st : "normal") + " text", cr, disabled ? 1.8 : 4.5,
                      hx(fg) + " on " + hx(behind));
        }
        for (const char* st : {"selected", "selected focus"}) {
            const Box& s = rows[st];
            double best = 1;
            int strong = 0;
            for (const char* nb : {"", "alternate"}) {
                const Box& n = rows[nb];
                int k = 0;
                for (int y = 0; y < s.img.height(); y++)
                    for (int x = 0; x < s.img.width(); x++)
                        if (contrast(s.img.colorAt(x, y), n.img.colorAt(x, y)) >= 3) k++;
                strong = std::max(strong, k);
                best = std::max(best, contrast(s.mode, n.mode));
            }
            // (a row whose fill is 3:1 from its neighbours', or with an edge (a line along it) that is)
            double v = best >= 3 ? best : (strong >= rw ? 3.0 : best);
            judgePair(std::string("ListWidget::item :") + st + " against the rows beside it", v, 3.0,
                      "fill " + hx(s.mode) + " vs " + hx(rows[""].mode) + "; " + std::to_string(strong) + " pixels 3:1 apart (an edge needs " +
                          std::to_string(rw) + ")");
        }
    }
    // the keyboard focus on everything that takes it
    Sheet fs;
    focusSheet = &fs;
    for (auto [w, what] : std::vector<std::pair<Widget*, std::string>>{
             {push, "PushButton"}, {def, "PushButton (a dialog's default)"}, {items[2].w, "ToolButton"}, {check, "CheckBox"},
             {edit, "LineEdit"}, {spin, "SpinBox"}, {combo, "ComboBox"}, {slider, "Slider"}, {list, "ListWidget (a row selected)"}}) {
        sweepFocus(win, w, what);
    }
    focusSheet = nullptr;
    {
        int h = std::min(fs.img.height(), fs.y + fs.rowH + 4);
        Image cut(fs.img.width(), h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < cut.width(); x++) cut.row(y)[x] = fs.img.pixel(x, y);
        savePng(OUT + "/" + fileSafe(skinNow) + "-focus.png", cut);
    }
    win.hideWindow();
}

// --- the colours the program draws with itself, against what they are drawn on ---
void sweepColours() {
    struct P { const char* fg; const char* bg; double need; const char* why; };
    static const std::vector<P> list{
        {"window_text", "window", 4.5, "text"}, {"text", "base", 4.5, "text"}, {"text", "alternate_base", 4.5, "text"},
        {"button_text", "button", 4.5, "text"}, {"highlighted_text", "highlight", 4.5, "text"},
        {"accent_text", "accent", 4.5, "text on lit buttons"}, {"tooltip_text", "tooltip_base", 4.5, "text"},
        {"hint_text", "window", 4.5, "text"}, {"hint_text", "base", 4.5, "text"}, {"dimmed_text", "window", 4.5, "text"},
        {"ruler_text", "ruler_bg", 4.5, "text"}, {"keys_label", "keys_white", 4.5, "text"},
        {"playhead", "lane", 3, "a line that carries meaning"}, {"playhead", "lane_alt", 3, "a line that carries meaning"},
        {"cursor", "lane", 3, "a line that carries meaning"}, {"cursor", "lane_alt", 3, "a line that carries meaning"},
        {"automation_line", "automation_lane", 3, "a curve"}, {"selected_note", "lane", 3, "a selected note's edge"},
        {"meter_ok", "meter_bg", 3, "a meter's bar"}, {"meter_warn", "meter_bg", 3, "a meter's bar"},
        {"meter_hot", "meter_bg", 3, "a meter's bar"},
        {"grid_bar", "lane", 1.3, "grid (the ruler says the same)"}, {"grid_bar", "lane_alt", 1.3, "grid (the ruler says the same)"},
        {"solo_text", "solo", 4.5, "text on a lit solo"},
        {"focus", "window", 3, "the keyboard focus"}, {"focus", "base", 3, "the keyboard focus"},
        {"master_divider", "master_lane", 1.3, "a dividing line"},
    };
    for (auto& p : list) {
        Color fg = theme::c(p.fg), bg = theme::c(p.bg);
        judgePair(std::string("--") + theme::variableName(p.fg).substr(2) + " on --" + theme::variableName(p.bg).substr(2) + " (" + p.why + ")",
                  contrast(over(fg, bg), bg), p.need, hx(fg) + " on " + hx(bg));
    }
    // the selection's edges and the loop on what they are drawn over
    Color edge = theme::c("selection_edge"), lane = theme::c("lane");
    Color filled = over(theme::c("selection_fill"), lane);
    judgePair("--selection-edge on a selected lane", contrast(over(edge, filled), filled), 3, hx(edge) + " on " + hx(filled));
    Color ruler = theme::c("ruler_bg");
    judgePair("--loop on --ruler-bg", contrast(over(theme::c("loop"), ruler), ruler), 3, hx(theme::c("loop")) + " on " + hx(ruler));
}

// --- the surfaces: the program's own texts (the hunt's list: ruler, lanes, piano keys, menus, tabs, meters…) ---
std::set<std::string> reported;
void sweepSurfaces(const Widget* owner) {
    // the explorer's (and the sampler's) waveform in its well
    {
        css::Style s = paint::nodeStyle(owner, "SampleWaveform", {}, {});
        Color bg = backgroundOver(s, theme::c("window")), fg = ui::style::color(s, "color", theme::c("sample_wave"));
        judgePair("SampleWaveform: the wave in its well", contrast(over(fg, bg), bg), 3.0, hx(fg) + " on " + hx(bg));
    }
    judgeSurfaces(owner);  // (records what is under its threshold in the hunt's findings, by skin)
    for (auto& k : hitOrder) {
        const Hit& h = hits[k];
        if (h.check == "surface" && h.per.count(where()) && reported.insert(k + where()).second)  // (its colour pairs: sweepColours)
            judgePair(h.key, h.per.at(where()), 9e9, "(under its threshold) " + h.perDetail.at(where()));
    }
}

// --- meters: bars against the meter, the RMS bar against the peak bar it is drawn in ---
void sweepMeter() {
    for (double rmsDb : {-20.0, -8.0}) {
        Window win(plat::WindowOptions{});
        win.resizeWindow(240, 30);
        auto* m = win.add<LevelMeter>(2);
        m->setGeometry(Rect(4, 4, 232, 22));
        win.showWindow();
        std::vector<float> peaks{0.5f, 0.5f}, ms{(float)std::pow(10.0, rmsDb / 10), (float)std::pow(10.0, rmsDb / 10)};
        m->push(peaks);
        m->pushRms(&ms);
        win.invalidate(win.outerRect());
        win.flush();
        const Image& fb = win.framebuffer();
        auto bars = m->barRects();
        if (bars.empty()) continue;
        int y0 = bars[0].first + 4, th = bars[0].second;
        // (the bar's body: its lower part, under a gloss's upper half and below the RMS bar along the middle)
        int x = 4 + std::max(1, (int)(m->barsW() * 0.1)), yb = y0 + th * 5 / 6;
        Color mid = fb.colorAt(x, y0 + th / 2), body = fb.colorAt(x, yb), back = fb.colorAt(4 + m->barsW() - 3, yb);
        judgePair("LevelMeter RMS bar inside the peak bar (RMS " + r2(rmsDb) + " dB)", contrast(mid, body), 3.0, hx(mid) + " in " + hx(body));
        if (rmsDb < -10) {
            int xw = 4 + (int)(m->barsW() * level_meter::dbFraction(-8.5));  // (in the -12…0 dB part)
            Color warn = fb.colorAt(xw, yb);
            judgePair("LevelMeter bar (up to -12 dB) against the meter", contrast(body, back), 3.0, hx(body) + " on " + hx(back));
            judgePair("LevelMeter bar (-12…0 dB) against the meter", contrast(warn, back), 3.0, hx(warn) + " on " + hx(back));
        }
        Image z(fb.width() * 4, fb.height() * 4);
        for (int yy = 0; yy < z.height(); yy++)
            for (int xx = 0; xx < z.width(); xx++) z.row(yy)[xx] = fb.pixel(xx / 4, yy / 4);
        savePng(OUT + "/meters/" + fileSafe(skinNow) + "-rms" + std::to_string((int)rmsDb) + ".png", z);
        win.hideWindow();
    }
}

// --- the symbols the interface uses: drawn by a font (not a hollow box) ---
std::map<uint32_t, std::string> uiChars;
void sweepGlyphs() {
    Font ui = theme::uiFont();
    for (auto& [cp, at] : uiChars) {
        if (cp < 0x2000 && cp != 0xB7 && cp != 0xD7) continue;
        if (!ui.hasGlyph(cp))
            fails.push_back(skinNow + " · the symbol " + codepointToUtf8(cp) + " (U+" + util::format("%04X", cp) +
                            ", used at " + at + ") is drawn by no font: a hollow box");
    }
}

// --- the main window and the newer windows: every text as painted (the hunt's look at them) ---
void sweepWindows(Rig& r) {
    std::string pic = OUT + "/" + fileSafe(skinNow) + "-main.png";
    r.w->invalidate(r.w->outerRect());
    r.w->flush();
    savePng(pic, r.w->framebuffer());
    contrastWindow(*r.w, pic);
    r.w->timeline->scroll->scrollTo(0, 1 << 20);
    settle(1);
    {
        std::string down = OUT + "/" + fileSafe(skinNow) + "-main-scrolled.png";
        r.w->invalidate(r.w->outerRect());
        r.w->flush();
        savePng(down, r.w->framebuffer());
        contrastWindow(*r.w, down);
    }
    r.w->timeline->scroll->scrollTo(0, 0);
    settle(1);
    for (auto& [name, open] : tier1()) {
        std::vector<Window*> before = shownOthers(r.w);
        std::string label = name;
        onModal = [&, label](Window* win) {
            std::string wp = OUT + "/windows/" + fileSafe(skinNow + "-" + label) + ".png";
            win->invalidate(win->outerRect());
            win->flush();
            savePng(wp, win->framebuffer());
            contrastWindow(*win, wp);
        };
        open(r);
        onModal = nullptr;
        settle(2);
        for (Window* win : shownOthers(r.w))
            if (std::find(before.begin(), before.end(), win) == before.end() && !win->isPopup()) {
                std::string wp = OUT + "/windows/" + fileSafe(skinNow + "-" + name) + ".png";
                win->invalidate(win->outerRect());
                win->flush();
                savePng(wp, win->framebuffer());
                contrastWindow(*win, wp);
                win->hideWindow();
            }
    }
    App::get().closeAllPopups();
}

}  // namespace ca

int main() {
    using namespace ca;
    plat::initWithoutDisplay();
    skin::setBuiltinDir("data/skins");
    paths::setDataDir(util::absolutePath(ca::OUT + "/tmp/data"));
    App::get().initHeadless();
    theme::setSkin();
    transport_bar::iconFolder = "data/icons";
    std::filesystem::remove_all(ca::OUT);
    for (const char* d : {"", "/tmp", "/focus", "/meters", "/windows"}) util::makeDirs(ca::OUT + d);
    settings::setPath(ca::OUT + "/tmp/settings.json");
    hunt::HOUT = ca::OUT;  // (its crops of what is under its threshold: here)
    App::get().nestedLoop = [](const std::function<bool()>&) {
        settle(2);
        for (Window* w : std::vector<Window*>(App::get().windows()))
            if (w->isWindowShown() && w->isModal() && !w->isPopup()) {
                if (hunt::onModal) hunt::onModal(w);
                w->close();
            }
        App::get().closeAllPopups();
    };
    ca::rep.open(ca::OUT + "/report.md");
    ca::rep << "# Contrast audit (tests/test_contrast.cpp)\n\nEvery pair measured, in every skin.\n\n";
    uiChars = hunt::uiCharacters();
    App::get().setScale(1);
    hunt::scaleName = "100%";
    Rig r = makeMain(1280, 800);
    dressRig(r);
    for (const skin::Skin& s : skin::library()) {
        r.w->chooseSkin(s);
        settle(2);
        skinNow = hunt::skinName = s.name;
        ca::rep << "\n## " << s.name << "\n\n";
        sweepColours();
        sweepSurfaces(r.w);
        sweepControls();
        sweepMeter();
        ca::sweepGlyphs();
        size_t before = hunt::hits.size();
        (void)before;
        sweepWindows(r);
        printf("contrast: %s done (%d pairs so far, %zu under their threshold)\n", s.name.c_str(), pairs, fails.size());
        fflush(stdout);
    }
    // the windows' texts as painted (the hunt's findings: "contrast", in a normal, hover, pressed or checked state)
    ca::rep << "\n## Texts in the main window and the newer windows, as painted\n\n";
    for (auto& k : hunt::hitOrder) {
        const hunt::Hit& h = hunt::hits[k];
        // (not "contrast-disabled": enabled widgets made disabled to look at them; the controls' sweep judges that look)
        if (h.check != "contrast") continue;
        for (auto& [where, v] : h.per) {
            ca::rep << "- **FAIL** " << where << " · " << h.key << ": " << h.perDetail.at(where) << "\n";
            fails.push_back(where + " · " + h.key + " (as painted): " + r2(v) + ":1 — " + h.perDetail.at(where));
        }
    }
    r.w->chooseSkin(skin::find("Dark"));
    closeMain(r);
    ca::rep << "\n## Under their threshold: " << fails.size() << "\n\n";
    for (auto& f : fails) ca::rep << "- " << f << "\n";
    ca::rep.close();
    for (auto& f : fails) fprintf(stderr, "contrast: %s\n", f.c_str());
    printf("contrast: %d pairs in %zu skins, %zu under their threshold; build/ui-samples/contrast-audit/report.md\n", pairs,
           skin::library().size(), fails.size());
    CHECK(fails.empty());
    CHECK(pairs > 500);
    TEST_MAIN_END();
}
