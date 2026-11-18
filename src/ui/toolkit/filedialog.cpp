// Tiny DAW's own file chooser (the stand-in for QFileDialog's static functions): a path field with
// Up / Home, "places" on the left (Home, Desktop, Music, Documents, Downloads, /, mounted volumes),
// the folder's folders and files (folders first, natural order, size and date columns), the file
// name, the type filter ("Audio and MIDI (*.wav *.mid);;All files (*)"), Open / Save / Choose and
// Cancel. The last folder is remembered per dialog title in the settings.
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "core/util.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/style.h"

namespace ui {

namespace {

// --- filters and globs ---
struct Filter {
    std::string text;                   // as given: "Audio and MIDI (*.wav *.mid)"
    std::vector<std::string> patterns;  // "*.wav", "*.mid"
};

std::vector<Filter> parseFilters(const std::string& spec) {
    std::vector<Filter> out;
    std::string s = spec;
    size_t start = 0;
    while (start <= s.size()) {
        size_t sep = s.find(";;", start);
        std::string part = util::trim(s.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
        if (!part.empty()) {
            Filter f;
            f.text = part;
            size_t open = part.rfind('('), close = part.rfind(')');
            std::string pats = (open != std::string::npos && close != std::string::npos && close > open)
                                   ? part.substr(open + 1, close - open - 1)
                                   : part;
            for (auto& w : util::splitWords(pats)) f.patterns.push_back(w);
            if (f.patterns.empty()) f.patterns.push_back("*");
            out.push_back(f);
        }
        if (sep == std::string::npos) break;
        start = sep + 2;
    }
    if (out.empty()) out.push_back({"All files (*)", {"*"}});
    return out;
}

bool globMatch(const char* p, const char* s) {
    for (; *p; ++p, ++s) {
        if (*p == '*') {
            while (p[1] == '*') ++p;
            if (!p[1]) return true;
            for (; *s; ++s)
                if (globMatch(p + 1, s)) return true;
            return globMatch(p + 1, s);
        }
        if (!*s) return false;
        if (*p == '?') continue;
        char a = *p, b = *s;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return !*s;
}

bool matchesFilter(const Filter& f, const std::string& name) {
    for (auto& p : f.patterns)
        if (globMatch(p.c_str(), name.c_str())) return true;
    return false;
}

std::string firstExtension(const Filter& f) {
    for (auto& p : f.patterns)
        if (p.size() > 2 && p[0] == '*' && p[1] == '.' && p.find_first_of("*?", 2) == std::string::npos) return p.substr(1);
    return "";
}

std::string sizeText(int64_t n) {
    if (n < 0) return "";
    if (n < 1024) return std::to_string(n) + (n == 1 ? " byte" : " bytes");
    if (n < 1024 * 1024) return util::format("%.1f KB", n / 1024.0);
    if (n < 1024LL * 1024 * 1024) return util::format("%.1f MB", n / (1024.0 * 1024));
    return util::format("%.2f GB", n / (1024.0 * 1024 * 1024));
}

std::string timeText(double t) {
    if (t <= 0) return "";
    time_t tt = (time_t)t;
    struct tm tmv;
    localtime_r(&tt, &tmv);
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

std::string normalize(const std::string& path) {
    std::string p = util::absolutePath(util::expandUser(path));
    // collapse "." and ".."
    std::vector<std::string> parts;
    for (auto& seg : util::split(p, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(seg);
    }
    std::string out;
    for (auto& s : parts) out += "/" + s;
    return out.empty() ? "/" : out;
}

const char* kUpSvg =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><path d='M8 13 L8 3.5 M3.8 7.5 L8 3.3 L12.2 7.5' "
    "fill='none' stroke='#000' stroke-width='1.6' stroke-linecap='round' stroke-linejoin='round'/></svg>";
const char* kHomeSvg =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><path d='M2.5 8 L8 3 L13.5 8 M4.5 6.5 L4.5 13 "
    "L11.5 13 L11.5 6.5' fill='none' stroke='#000' stroke-width='1.5' stroke-linecap='round' "
    "stroke-linejoin='round'/></svg>";

void drawFolderGlyph(Painter& p, float x, float y, float s, Color c) {
    // (drawn for a 14 px box: s is its size / 14)
    Path path;
    path.moveTo(x, y + 2 * s);
    path.lineTo(x + 5 * s, y + 2 * s);
    path.lineTo(x + 6.5f * s, y + 3.5f * s);
    path.lineTo(x + 14 * s, y + 3.5f * s);
    path.lineTo(x + 14 * s, y + 12 * s);
    path.lineTo(x, y + 12 * s);
    path.close();
    p.fillPath(path, c);
}

void drawFileGlyph(Painter& p, float x, float y, float s, Color c) {
    std::vector<PointF> pts = {{x + 2.5f * s, y + 0.5f * s},  {x + 8.5f * s, y + 0.5f * s},  {x + 11.5f * s, y + 3.5f * s},
                               {x + 11.5f * s, y + 13.5f * s}, {x + 2.5f * s, y + 13.5f * s}, {x + 2.5f * s, y + 0.5f * s}};
    p.drawPolyline(pts, c, 1.0f * s, LineCap::Flat, LineJoin::Miter);
    p.drawPolyline({{x + 8.5f * s, y + 0.5f * s}, {x + 8.5f * s, y + 3.5f * s}, {x + 11.5f * s, y + 3.5f * s}}, c, 1.0f * s);
}

// --- the list of folders and files. Styles (default.css): FileList (its box; width / height, min-width /
// min-height; --details-from: the detail columns show when its rows are wider than that), FileList::header (the
// column titles: its box, padding around each title), FileList::item (each row: height, :selected, :hover,
// :folder), FileList::icon (the folder / file glyph: width, colour), FileList::detail (the size and date
// columns: padding, --spare: room beyond the widest value, colour), FileList::placeholder ("(empty)": height) ---
class FileList : public Widget {
public:
    struct Entry {
        std::string name;
        bool dir = false;
        int64_t size = -1;
        double mtime = 0;
    };
    std::vector<Entry> entries;
    std::vector<bool> sel;
    int current = -1, hover = -1, anchor = -1;
    bool multi = false;
    ScrollBar* bar;
    int rowH = 22;  // (FileList::item's height, kept up to date)
    Signal<int> activated;
    Signal<> selectionChanged;
    Signal<> goUp;

    FileList() {
        setStyleType("FileList");
        bar = add<ScrollBar>(Orientation::Vertical);
        bar->setVisible(false);
        bar->valueChanged.connect([this](int) { update(); });
        setFocusPolicy(FocusPolicy::Strong);
        setMouseTracking(true);
        hPolicy = vPolicy = SizePolicy::Expanding;
    }
    // the header's height: its font's line, padding and border (default.css: 3 px above and below, a 1 px line)
    int headerH() const {
        const css::Style& h = computedStyle("header");
        return style::textHeight(style::font(h, theme::uiFont())) + style::insets(h, "padding").h() +
               style::insets(h, "border-width").h();
    }
    Point sizeHint() const override {
        const css::Style& st = computedStyle();
        return Point(style::px(st, "width", 420), style::px(st, "height", 300));
    }
    Point minimumSizeHint() const override {
        const css::Style& st = computedStyle();
        return Point(style::px(st, "min-width", 120), style::px(st, "min-height", 80));
    }
    bool readStyle() {
        PartStyles parts(*this);
        int h = std::max(1, style::px(parts.get("item"), "height", 22));
        if (h == rowH) return false;
        rowH = h;
        return true;
    }
    void setEntries(std::vector<Entry> e) {
        entries = std::move(e);
        sel.assign(entries.size(), false);
        current = hover = anchor = -1;
        bar->setValue(0);
        sync();
        selectionChanged.emit();
    }
    void sync() {
        readStyle();
        int total = (int)entries.size() * rowH, vh = height() - headerH();
        bool need = total > vh && vh > 0;
        int bw = bar->thickness();
        bar->setVisible(need);
        bar->setGeometry(width() - bw, headerH(), bw, std::max(0, vh));
        bar->setPageStep(vh);
        bar->setSingleStep(rowH);
        bar->setRange(0, std::max(0, total - vh));
        update();
    }
    void resized() override { sync(); }
    int viewWidth() const { return width() - (bar->isVisible() ? bar->thickness() : 0); }
    int rowAt(Point p) const {
        int vw = viewWidth();
        if (p.x < 0 || p.x >= vw || p.y < headerH() || p.y >= height()) return -1;
        int r = (p.y - headerH() + bar->value()) / rowH;
        return r < (int)entries.size() ? r : -1;
    }
    std::vector<int> selected() const {
        std::vector<int> out;
        for (size_t i = 0; i < sel.size(); ++i)
            if (sel[i]) out.push_back((int)i);
        return out;
    }
    void scrollTo(int r) {
        if (r < 0) return;
        int y = r * rowH, v = bar->value(), vh = height() - headerH();
        if (y < v) v = y;
        else if (y + rowH > v + vh) v = y + rowH - vh;
        bar->setValue(v);
    }
    void select(int r, int mods) {
        if (r < 0 || r >= (int)entries.size()) return;
        bool ctrl = mods & plat::ModCtrl, shift = mods & plat::ModShift;
        if (multi && ctrl) {
            sel[r] = !sel[r];
            anchor = r;
        } else if (multi && shift && anchor >= 0) {
            std::fill(sel.begin(), sel.end(), false);
            for (int i = std::min(anchor, r); i <= std::max(anchor, r); ++i) sel[i] = true;
        } else {
            std::fill(sel.begin(), sel.end(), false);
            sel[r] = true;
            anchor = r;
        }
        current = r;
        scrollTo(r);
        update();
        selectionChanged.emit();
    }
    void paint(Painter& p) override {
        if (readStyle()) sync();
        const css::Style& st = computedStyle();
        const css::Style& head = computedStyle("header");
        PartStyles parts(*this);
        Font base = theme::uiFont();
        Font hf = style::font(head, base);
        int vw = viewWidth();
        int hh = headerH();
        style::box(p, rect(), st);
        // columns: name | size | modified (the detail columns: their widest value, padding and --spare)
        const css::Style& detail = parts.get("detail");
        style::Insets dpad = style::insets(detail, "padding");
        int spare = (int)std::lround(PartStyles::lengthVar(detail, "--spare", 4));
        const Font& df = parts.font(detail, base);
        int dateW = style::textWidth(df, "0000-00-00 00:00") + dpad.w() + spare;
        int sizeW = style::textWidth(df, "000.0 MB") + dpad.w() + spare;
        bool cols = vw > (int)std::lround(PartStyles::lengthVar(st, "--details-from", 360));
        int nameW = cols ? vw - dateW - sizeW : vw;
        style::box(p, Rect(0, 0, width(), hh), head);
        style::Insets hpad = style::insets(head, "padding"), hb = style::insets(head, "border-width");
        Color ht = style::foreground(head);
        int th = hh - hb.h();  // (the titles centred above the header's bottom edge)
        style::text(p, Rect(hb.left + hpad.left, hb.top, nameW - hb.left - hpad.w(), th), AlignLeft | AlignVCenter, "Name",
                    hf, ht);
        if (cols) {
            style::text(p, Rect(nameW + hpad.left, hb.top, sizeW - hpad.w(), th), AlignRight | AlignVCenter, "Size", hf, ht);
            style::text(p, Rect(nameW + sizeW + hpad.left, hb.top, dateW - hpad.w(), th), AlignLeft | AlignVCenter,
                        "Modified", hf, ht);
        }
        p.save();
        p.setClip(Rect(0, hh, vw, height() - hh));
        int scroll = bar->value();
        std::vector<const char*> states;
        for (int r = std::max(0, scroll / rowH); r < (int)entries.size(); ++r) {
            int y = hh + r * rowH - scroll;
            if (y >= height()) break;
            const Entry& e = entries[r];
            states.clear();
            if (sel[r]) states.push_back("selected");
            if (r == hover) states.push_back("hover");
            if (e.dir) states.push_back("folder");
            const css::Style& item = parts.get("item", states);
            Rect row(0, y, vw, rowH);
            style::box(p, row, item);
            Rect c = style::contents(row, item);
            const css::Style& icon = parts.get("icon", states, &item);
            int iw = std::max(0, style::px(icon, "width", 14));
            Color gc = style::foreground(icon);
            float gx = (float)(c.x + style::px(icon, "margin-left")), gy = c.y + (c.h - iw) / 2.f;
            if (e.dir) drawFolderGlyph(p, gx, gy, iw / 14.f, gc);
            else drawFileGlyph(p, gx, gy, iw / 14.f, gc);
            int tx = (int)gx + iw + style::px(icon, "margin-right", 6);
            Rect nameBox = style::contents(Rect(0, y, nameW, rowH), item);
            style::text(p, Rect(tx, c.y, nameBox.right() - tx, c.h), AlignLeft | AlignVCenter, e.name,
                        parts.font(item, base), style::foreground(item), true);
            if (cols) {
                const css::Style& d = parts.get("detail", states, &item);
                const Font& dff = parts.font(d, base);
                Color dc = style::foreground(d);
                if (!e.dir)
                    style::text(p, Rect(nameW + dpad.left, c.y, sizeW - dpad.w(), c.h), AlignRight | AlignVCenter,
                                sizeText(e.size), dff, dc);
                style::text(p, Rect(nameW + sizeW + dpad.left, c.y, dateW - dpad.w(), c.h), AlignLeft | AlignVCenter,
                            timeText(e.mtime), dff, dc);
            }
        }
        p.restore();
        if (entries.empty()) {
            const css::Style& ph = parts.get("placeholder");
            style::text(p, Rect(0, hh, vw, std::min(style::px(ph, "height", 60), height() - hh)), AlignCenter, "(empty)",
                        parts.font(ph, base), style::foreground(ph));
        }
    }
    bool mousePress(MouseEvent& e) override {
        if (e.button != plat::LeftButton && e.button != plat::RightButton) return false;
        int r = rowAt(e.pos);
        if (r >= 0) select(r, e.mods);
        return true;
    }
    bool mouseDoubleClick(MouseEvent& e) override {
        int r = rowAt(e.pos);
        if (r >= 0 && e.button == plat::LeftButton) {
            select(r, 0);
            activated.emit(r);
        }
        return true;
    }
    bool mouseMove(MouseEvent& e) override {
        int r = rowAt(e.pos);
        if (r != hover) {
            hover = r;
            update();
        }
        return isPressed();
    }
    void leave() override {
        hover = -1;
        update();
    }
    bool wheel(WheelEvent& e) override {
        int old = bar->value();
        bar->setValue(old - (int)std::lround(e.dy * 3 * rowH));
        return bar->value() != old;
    }
    bool keyPress(KeyEvent& e) override {
        int n = (int)entries.size();
        int page = std::max(1, (height() - headerH()) / rowH - 1);
        int r = current;
        switch (e.key) {
        case plat::Key_Up: r = std::max(0, r - 1); break;
        case plat::Key_Down: r = std::min(n - 1, r + 1); break;
        case plat::Key_PageUp: r = std::max(0, r - page); break;
        case plat::Key_PageDown: r = std::min(n - 1, std::max(0, r) + page); break;
        case plat::Key_Home: r = 0; break;
        case plat::Key_End: r = n - 1; break;
        case plat::Key_Backspace: goUp.emit(); return true;
        case plat::Key_Return:
        case plat::Key_Enter:
            if (current >= 0 && current < n && entries[current].dir) {
                activated.emit(current);
                return true;
            }
            return false;  // (the dialog's default button)
        default: {
            // jump to the next name starting with the typed letter
            if (!e.text.empty() && (unsigned char)e.text[0] > 0x20 && !(e.mods & (plat::ModCtrl | plat::ModAlt))) {
                std::string ch = util::lower(e.text);
                for (int k = 1; k <= n; ++k) {
                    int i = ((current < 0 ? -1 : current) + k) % n;
                    if (util::startsWith(util::lower(entries[i].name), ch)) {
                        select(i, 0);
                        return true;
                    }
                }
                return true;
            }
            return false;
        }
        }
        if (n && r >= 0) select(r, e.mods & plat::ModShift);
        return true;
    }
};

enum class Mode { Open, OpenMany, Save, Folder };

class FileDialog : public Dialog {
public:
    FileDialog(Window* parent, const std::string& title, Mode m, const std::string& dir, const std::string& filter)
        : Dialog(parent, title, 760, 480), mode(m), key(title) {
        filters = mode == Mode::Folder ? std::vector<Filter>{{"Folders", {"*"}}} : parseFilters(filter);
        // its look (default.css: FileDialog, its size and room; ::path-row, ::nav-button, ::middle, ::places, ::form)
        setStyleType("FileDialog");
        const css::Style& st = computedStyle();
        int w = std::max(1, style::px(st, "width", 760)), h = std::max(1, style::px(st, "height", 480));
        if (w != 760 || h != 480) resizeWindow(w, h);  // (760 x 480: as the window was made)
        auto* v = styleRoom(vbox(this, 10, 8), st);
        // the path row
        auto top = std::make_unique<HBox>();
        HBox* t = top.get();
        t->spacing = 4;
        styleRoom(t, computedStyle("path-row"));
        v->addLayout(std::move(top));
        const css::Style& nav = computedStyle("nav-button");
        Point navSize(std::max(0, style::px(nav, "width", 26)), std::max(0, style::px(nav, "height", 24)));
        upBtn = toolButton(this, "", [this] { goUp(); }, "Up one folder (Backspace)");
        upBtn->setIconSvg(kUpSvg, 16);
        upBtn->setFixedSize(navSize.x, navSize.y);
        upBtn->setFocusPolicy(FocusPolicy::None);
        homeBtn = toolButton(this, "", [this] { go(plat::homeDir()); }, "Your home folder");
        homeBtn->setIconSvg(kHomeSvg, 16);
        homeBtn->setFixedSize(navSize.x, navSize.y);
        homeBtn->setFocusPolicy(FocusPolicy::None);
        pathEdit = add<LineEdit>();
        t->addWidget(upBtn);
        t->addWidget(homeBtn);
        t->addWidget(pathEdit, 1);
        pathEdit->returnPressed.connect([this] {
            std::string p = normalize(pathEdit->text());
            if (util::isDir(p)) go(p);
            else if (mode != Mode::Folder && util::fileExists(p) && mode != Mode::Save) {
                result = {p};
                finish();
            } else {
                plat_beep();
                pathEdit->setTextSilently(folder);
            }
        });
        // places and the list
        auto mid = std::make_unique<HBox>();
        HBox* mh = mid.get();
        mh->spacing = 8;
        styleRoom(mh, computedStyle("middle"));
        v->addLayout(std::move(mid), 1);
        places = add<ListWidget>();
        places->setFixedWidth(std::max(0, style::px(computedStyle("places"), "width", 150)));
        places->vPolicy = SizePolicy::Expanding;
        places->setFocusPolicy(FocusPolicy::Tab);
        fillPlaces();
        places->itemClicked.connect([this](int r) {
            std::string p = places->item(r).data;
            if (util::isDir(p)) go(p);
        });
        list = add<FileList>();
        list->multi = mode == Mode::OpenMany;
        mh->addWidget(places);
        mh->addWidget(list, 1);
        list->activated.connect([this](int r) { activate(r); });
        list->goUp.connect([this] { goUp(); });
        list->selectionChanged.connect([this] { selectionToName(); });
        // name and type
        auto* fw = add<Widget>();
        auto* fl = styleRoom(form(fw, 0, 6), computedStyle("form"));
        nameEdit = fw->add<LineEdit>();
        fl->addRow(mode == Mode::Folder ? "Folder:" : "File name:", nameEdit);
        typeCombo = fw->add<ComboBox>();
        for (auto& f : filters) typeCombo->addItem(f.text);
        if (mode != Mode::Folder) fl->addRow("Type:", typeCombo);
        else typeCombo->hide();
        v->addWidget(fw);
        typeCombo->currentIndexChanged.connect([this](int) { refresh(); });
        nameEdit->returnPressed.connect([] {});
        // the buttons
        bb = add<ButtonBox>(ButtonBox::Cancel);
        okBtn = bb->addButton(mode == Mode::Save ? "Save" : mode == Mode::Folder ? "Choose" : "Open", 1);
        bb->accepted.connect([this] { tryAccept(); });
        bb->rejected.connect([this] { reject(); });
        v->addWidget(bb);
        const css::Style& own = computedStyle();
        setMinimumWindowSize(std::max(0, style::px(own, "min-width", 480)), std::max(0, style::px(own, "min-height", 320)));
        // where to start
        std::string start, name;
        if (!dir.empty()) {
            std::string d = normalize(dir);
            if (util::isDir(d)) start = d;
            else {
                if (util::isDir(util::dirName(d)) && dir.find('/') != std::string::npos) start = util::dirName(d);
                name = util::baseName(dir);
            }
        }
        if (start.empty() && !key.empty()) {
            std::string last = settings::get("file_dialog/" + key).toString("");
            if (!last.empty() && util::isDir(last)) start = last;
        }
        if (start.empty()) start = plat::homeDir();
        go(start);
        if (!name.empty()) nameEdit->setTextSilently(name);
        setFocusWidget(mode == Mode::Save ? (Widget*)nameEdit : (Widget*)list);
        if (mode == Mode::Save) nameEdit->selectAll();
    }

    Mode mode;
    std::string key, folder;
    std::vector<Filter> filters;
    std::vector<std::string> result;
    std::string chosenFilter;
    Button *upBtn, *homeBtn, *okBtn;
    LineEdit *pathEdit, *nameEdit;
    ListWidget* places;
    FileList* list;
    ComboBox* typeCombo;
    ButtonBox* bb;

    static void plat_beep() {
        if (!App::get().headless()) plat::beep();
    }

    const Filter& filter() const {
        int i = std::max(0, std::min(typeCombo->currentIndex(), (int)filters.size() - 1));
        return filters[i];
    }

    void fillPlaces() {
        std::string home = plat::homeDir();
        auto addPlace = [this](const std::string& label, const std::string& path) {
            if (!util::isDir(path)) return;
            ListWidget::Item it;
            it.text = label;
            it.data = path;
            it.tooltip = path;
            places->addItem(it);
        };
        addPlace("Home", home);
        addPlace("Desktop", util::joinPath(home, "Desktop"));
        addPlace("Music", util::joinPath(home, "Music"));
        addPlace("Documents", util::joinPath(home, "Documents"));
        addPlace("Downloads", util::joinPath(home, "Downloads"));
        addPlace("/", "/");
        const char* user = getenv("USER");
        std::string u = user ? user : util::baseName(home);
        for (std::string base : {std::string("/media/") + u, std::string("/run/media/") + u}) {
            if (!util::isDir(base)) continue;
            for (auto& e : util::listDir(base))
                if (e.isDir) addPlace(e.name, util::joinPath(base, e.name));
        }
    }

    void go(const std::string& path) {
        std::string p = normalize(path);
        if (!util::isDir(p)) return;
        folder = p;
        pathEdit->setTextSilently(folder);
        upBtn->setEnabled(folder != "/");
        // the place it is in, lit
        int best = -1;
        size_t bestLen = 0;
        for (int i = 0; i < places->count(); ++i) {
            const std::string& d = places->item(i).data;
            if (folder == d && d.size() >= bestLen) {
                best = i;
                bestLen = d.size();
            }
        }
        if (best >= 0) places->setRowSelected(best, true);
        else places->clearSelection();
        refresh();
    }

    void goUp() {
        if (folder == "/") return;
        std::string from = util::baseName(folder);
        go(util::dirName(folder).empty() ? "/" : util::dirName(folder));
        for (int i = 0; i < (int)list->entries.size(); ++i)
            if (list->entries[i].name == from) {
                list->select(i, 0);
                break;
            }
    }

    void refresh() {
        std::vector<FileList::Entry> dirs, fileEntries;
        const Filter& f = filter();
        for (auto& e : util::listDir(folder)) {
            if (e.name.empty() || e.name[0] == '.') continue;
            std::string full = util::joinPath(folder, e.name);
            bool isDir = e.isDir || util::isDir(full);
            if (isDir) {
                FileList::Entry x;
                x.name = e.name;
                x.dir = true;
                x.mtime = util::fileMtime(full);
                dirs.push_back(x);
            } else if (mode != Mode::Folder && matchesFilter(f, e.name)) {
                FileList::Entry x;
                x.name = e.name;
                x.size = util::fileSize(full);
                x.mtime = util::fileMtime(full);
                fileEntries.push_back(x);
            }
        }
        auto byName = [](const FileList::Entry& a, const FileList::Entry& b) { return util::compareNatural(a.name, b.name) < 0; };
        std::sort(dirs.begin(), dirs.end(), byName);
        std::sort(fileEntries.begin(), fileEntries.end(), byName);
        dirs.insert(dirs.end(), fileEntries.begin(), fileEntries.end());
        list->setEntries(std::move(dirs));
    }

    void selectionToName() {
        std::vector<int> s = list->selected();
        std::vector<std::string> names;
        for (int i : s) {
            const auto& e = list->entries[i];
            if (mode == Mode::Folder ? e.dir : !e.dir) names.push_back(e.name);
        }
        if (names.empty()) return;
        if (names.size() == 1) nameEdit->setTextSilently(names[0]);
        else {
            std::string t;
            for (auto& n : names) t += (t.empty() ? "\"" : " \"") + n + "\"";
            nameEdit->setTextSilently(t);
        }
    }

    void activate(int r) {
        if (r < 0 || r >= (int)list->entries.size()) return;
        const auto& e = list->entries[r];
        if (e.dir) {
            go(util::joinPath(folder, e.name));
            if (mode == Mode::Folder) nameEdit->setTextSilently("");
            return;
        }
        nameEdit->setTextSilently(e.name);
        tryAccept();
    }

    std::vector<std::string> typedNames() const {
        std::string t = util::trim(nameEdit->text());
        std::vector<std::string> out;
        if (t.empty()) return out;
        if (t.find('"') != std::string::npos) {
            size_t i = 0;
            while ((i = t.find('"', i)) != std::string::npos) {
                size_t j = t.find('"', i + 1);
                if (j == std::string::npos) break;
                out.push_back(t.substr(i + 1, j - i - 1));
                i = j + 1;
            }
        } else {
            out.push_back(t);
        }
        return out;
    }

    std::string full(const std::string& name) const {
        std::string n = util::expandUser(name);
        if (!n.empty() && n[0] == '/') return normalize(n);
        return normalize(util::joinPath(folder, n));
    }

    void finish() {
        if (!key.empty()) settings::set("file_dialog/" + key, Json(folder));
        chosenFilter = filter().text;
        accept();
    }

    void tryAccept() {
        std::vector<std::string> names = typedNames();
        if (mode == Mode::Folder) {
            std::string pick = folder;
            if (!names.empty()) {
                std::string p = full(names[0]);
                if (util::isDir(p)) pick = p;
            }
            result = {pick};
            if (!key.empty()) settings::set("file_dialog/" + key, Json(util::isDir(pick) ? util::dirName(pick) : folder));
            chosenFilter.clear();
            accept();
            return;
        }
        if (names.empty()) {
            // a folder chosen in the list: go in
            int c = list->current;
            if (c >= 0 && c < (int)list->entries.size() && list->entries[c].dir) activate(c);
            else plat_beep();
            return;
        }
        if (names.size() == 1) {
            std::string p = full(names[0]);
            if (util::isDir(p)) {
                go(p);
                nameEdit->setTextSilently("");
                return;
            }
            // a pattern typed: use it as the filter
            if (names[0].find_first_of("*?") != std::string::npos) {
                Filter f;
                f.text = names[0];
                f.patterns = util::splitWords(names[0]);
                filters.push_back(f);
                typeCombo->addItem(f.text);
                typeCombo->setCurrentIndex(typeCombo->count() - 1);
                nameEdit->setTextSilently("");
                return;
            }
        }
        if (mode == Mode::Save) {
            std::string name = names[0];
            std::string p = full(name);
            if (util::extension(p).empty()) {
                std::string ext = firstExtension(filter());
                if (!ext.empty()) p += ext;
            }
            if (!util::isDir(util::dirName(p))) {
                msg::warning(this, title(), util::dirName(p) + "\nThis folder doesn't exist.");
                return;
            }
            if (util::fileExists(p)) {
                int a = msg::question(this, title(), util::baseName(p) + " already exists.\nDo you want to replace it?",
                                      msg::Yes | msg::No, msg::No);
                if (a != msg::Yes) return;
            }
            result = {p};
            finish();
            return;
        }
        std::vector<std::string> out;
        for (auto& n : names) {
            std::string p = full(n);
            if (!util::fileExists(p) || util::isDir(p)) {
                msg::warning(this, title(), n + "\nFile not found.\nCheck the file name and try again.");
                return;
            }
            out.push_back(p);
            if (mode == Mode::Open) break;
        }
        result = out;
        finish();
    }
};

}  // namespace

namespace files {

std::string openFile(Window* parent, const std::string& title, const std::string& dir, const std::string& filter) {
    FileDialog d(parent, title, Mode::Open, dir, filter);
    if (d.exec() != Dialog::Accepted || d.result.empty()) return "";
    return d.result.front();
}

std::vector<std::string> openFiles(Window* parent, const std::string& title, const std::string& dir,
                                   const std::string& filter) {
    FileDialog d(parent, title, Mode::OpenMany, dir, filter);
    if (d.exec() != Dialog::Accepted) return {};
    return d.result;
}

std::string saveFile(Window* parent, const std::string& title, const std::string& dir, const std::string& filter,
                     std::string* chosenFilter) {
    FileDialog d(parent, title, Mode::Save, dir, filter);
    if (d.exec() != Dialog::Accepted || d.result.empty()) return "";
    if (chosenFilter) *chosenFilter = d.chosenFilter;
    return d.result.front();
}

std::string folder(Window* parent, const std::string& title, const std::string& dir) {
    FileDialog d(parent, title, Mode::Folder, dir, "");
    if (d.exec() != Dialog::Accepted || d.result.empty()) return "";
    return d.result.front();
}

}  // namespace files

}  // namespace ui
