// A toolkit window as text (see window_view.h)
#include "tui/window_view.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "platform/platform.h"
#include "ui/computer_keyboard.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/widget.h"

namespace tui {

namespace {

// a control to draw: where it is in the window (px), and how many rows of text it takes
struct Item {
    ui::Widget* w;
    Rect r;      // in the window's coordinates, cut to what its scroll areas show
    int rows = 1;
    int row = 0;  // its row (after placing)
};

bool hasFocusIn(const ui::Widget* w, const ui::Widget* focus) {
    for (const ui::Widget* f = focus; f; f = f->parent())
        if (f == w) return true;
    return false;
}

std::vector<std::string> wrapLines(const std::string& text, int width) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        // (words to the width)
        std::string line;
        size_t i = 0;
        while (i < para.size()) {
            size_t sp = para.find(' ', i);
            std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
            // (a word wider than a line, CJK written without spaces: broken where the line ends)
            if (width > 1 && columns(word) > width && !line.empty()) {
                out.push_back(line);
                line.clear();
            }
            while (width > 1 && columns(word) > width) {
                std::string head = fit(word, width, false);
                if (head.empty()) break;
                out.push_back(head);
                word = word.substr(head.size());
            }
            std::string next = line.empty() ? word : line + " " + word;
            if (columns(next) > width && !line.empty()) {
                out.push_back(line);
                line = word;
            } else {
                line = next;
            }
            if (sp == std::string::npos) break;
            i = sp + 1;
        }
        out.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

// what is drawn of a control: the leaves, and the containers drawn as a row of their own (a tab bar, a
// group's title) before their children
bool ownList(ui::Widget* w) {
    std::vector<std::string> rows;
    int cur = -1;
    return w->listRows(rows, cur);
}

bool ownCells(ui::Widget* w) {
    std::vector<ui::TextCell> cells;
    return w->textCells(1, 1, cells);
}

bool isLeaf(ui::Widget* w) {
    return ownList(w) || ownCells(w) || dynamic_cast<ui::Label*>(w) || dynamic_cast<ui::Button*>(w) || dynamic_cast<ui::CheckBox*>(w) ||
           dynamic_cast<ui::LineEdit*>(w) || dynamic_cast<ui::SpinBox*>(w) || dynamic_cast<ui::ComboBox*>(w) ||
           dynamic_cast<ui::Slider*>(w) || dynamic_cast<ui::ListWidget*>(w) || dynamic_cast<ui::TreeWidget*>(w) ||
           dynamic_cast<ui::StatusBar*>(w);
}

void collect(ui::Widget* w, ui::Window& win, const Rect& clip, std::vector<Item>& out) {
    for (auto& c : w->children()) {
        ui::Widget* child = c.get();
        if (!child->isVisible() || dynamic_cast<ui::Window*>(child) || dynamic_cast<ui::ScrollBar*>(child)) continue;
        Point at = child->mapTo(&win, Point(0, 0));
        Rect r(at.x, at.y, child->geometry().w, child->geometry().h);
        Rect shown = r.intersected(clip);
        if (shown.empty()) continue;
        bool leaf = isLeaf(child);
        bool header = dynamic_cast<ui::TabWidget*>(child) || (dynamic_cast<ui::GroupBox*>(child) &&
                                                               !static_cast<ui::GroupBox*>(child)->title_.empty());
        if (leaf || header) {
            Item it{child, header ? Rect(shown.x, shown.y, shown.w, 1) : shown};
            if (dynamic_cast<ui::ListWidget*>(child) || dynamic_cast<ui::TreeWidget*>(child) || ownList(child))
                it.rows = std::max(2, shown.h / ROW_PX);
            if (ownCells(child)) it.rows = std::max(1, (shown.h + ROW_PX / 2) / ROW_PX);
            out.push_back(it);
        }
        if (!leaf) collect(child, win, shown, out);
    }
}

std::string comboText(ui::ComboBox* c) { return c->currentText(); }

// the cells a control's text needs at least (a button's, a check box's), whatever its pixels
int minWidth(ui::Widget* w) {
    if (auto* l = dynamic_cast<ui::Label*>(w)) {  // (a short label whole; a long one wraps)
        int n = columns(l->text());
        return n <= 24 && l->text().find('\n') == std::string::npos ? n : 1;
    }
    if (auto* b = dynamic_cast<ui::Button*>(w)) return columns(b->text().empty() ? b->toolTip() : b->text()) + 4;
    if (auto* c = dynamic_cast<ui::CheckBox*>(w)) return columns(c->text()) + 4;
    if (auto* sp = dynamic_cast<ui::SpinBox*>(w)) return columns(sp->text()) + 5;
    return 1;
}

}  // namespace

void drawWindow(Screen& s, ui::Window& win, int ax, int ay, int aw, int ah) {
    std::vector<Item> items;
    collect(&win, win, Rect(0, 0, win.width(), win.height()), items);
    // a label's lines at the width it has
    const int inner = std::max(10, aw - 4);
    double scale = std::min(1.0, (double)inner * COL_PX / std::max(1, win.width()));  // (narrower than the window)
    auto colOf = [&](int px) { return (int)std::lround(px * scale / COL_PX); };
    auto widthOf = [&](const Item& it) { return std::max(1, colOf(it.r.w)); };
    for (Item& it : items)
        if (auto* l = dynamic_cast<ui::Label*>(it.w))
            it.rows = std::max(1, (int)wrapLines(l->text(), std::max(1, std::min(std::max(widthOf(it), minWidth(it.w)),
                                                                                    inner - colOf(it.r.x)))).size());
    // rows. Controls side by side (across the window, not overlapping, their middles within the first one's
    // height) make a line; each line goes on the first row free under everything above it in its columns, so
    // columns of controls stack each on its own (a grid of tall pads beside a column of settings)
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.r.y < b.r.y; });
    auto spanOf = [&](const Item& it) {  // (by its pixels: wider text is pushed right below)
        int a = colOf(it.r.x);
        return std::make_pair(a, a + widthOf(it));
    };
    std::vector<std::vector<size_t>> lines;
    std::vector<int> lineTop, lineBottom;
    for (size_t i = 0; i < items.size(); i++) {
        const Item& it = items[i];
        int mid = it.r.y + std::min(it.r.h, ROW_PX) / 2;
        auto [a, b] = spanOf(it);
        bool placed = false;
        for (size_t l = 0; l < lines.size() && !placed; l++) {
            if (mid < lineTop[l] || mid >= lineBottom[l]) continue;
            bool clash = false;
            for (size_t j : lines[l]) {
                auto [c, d] = spanOf(items[j]);
                if (a < d && c < b) clash = true;
            }
            if (clash) continue;
            lines[l].push_back(i);
            placed = true;
        }
        if (!placed) {
            lines.push_back({i});
            lineTop.push_back(it.r.y);
            lineBottom.push_back(it.r.y + std::max(1, std::min(it.r.h, ROW_PX * it.rows)));
        }
    }
    std::vector<int> skyline;  // per column: the first row free
    int rows = 0;
    for (const auto& line : lines) {
        int row = 0;
        for (size_t i : line) {
            auto [a, b] = spanOf(items[i]);
            if ((int)skyline.size() < b) skyline.resize((size_t)b, 0);
            for (int c = std::max(0, a); c < b; c++) row = std::max(row, skyline[(size_t)c]);
        }
        for (size_t i : line) {
            items[i].row = row;
            auto [a, b] = spanOf(items[i]);
            for (int c = std::max(0, a); c < b; c++) skyline[(size_t)c] = row + items[i].rows;
            rows = std::max(rows, row + items[i].rows);
        }
    }
    // (controls side by side whose text is wider than their pixels: pushed right, so they don't overlap)
    std::vector<int> placedAt(items.size());
    {
        std::vector<size_t> order(items.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return items[a].row != items[b].row ? items[a].row < items[b].row : items[a].r.x < items[b].r.x;
        });
        int lastRow = -1, end = 0;
        for (size_t i : order) {
            Item& it = items[i];
            int x = colOf(it.r.x);
            if (it.row != lastRow) end = 0;
            x = std::max(x, end);
            placedAt[i] = x;
            lastRow = it.row;
            end = x + std::max(widthOf(it), minWidth(it.w)) + 1;
        }
    }
    if (auto* pd = dynamic_cast<ui::ProgressDialog*>(&win)) rows += 1, (void)pd;

    // the box
    int need = 0;
    for (size_t i = 0; i < items.size(); i++) need = std::max(need, placedAt[i] + std::max(widthOf(items[i]), minWidth(items[i].w)));
    int bw = std::min(aw, std::max(need, colOf(win.width())) + 4);
    bw = std::max(bw, std::min(aw, columns(win.title()) + 8));
    int bh = std::min(ah, rows + 2);
    int bx = ax + (aw - bw) / 2, by = ay + (ah - bh) / 2;
    Style frame = style(White, Blue);
    s.fill(bx, by, bw, bh, U' ', frame);
    s.box(bx, by, bw, bh, frame, win.title());
    int cx = bx + 2, cy = by + 1, cw = bw - 4, ch = bh - 2;
    if (cw <= 0 || ch <= 0) return;

    ui::Widget* focus = win.focusWidget();
    // (taller than the box: its rows scrolled so the control with the keyboard is shown; ▲ ▼ on the frame: more)
    int top = 0;
    if (rows > ch) {
        for (const Item& it : items) {
            if (!hasFocusIn(it.w, focus)) continue;
            // (a list: its row chosen, as placed when drawn below; else the control's first row)
            int line = 0, n = 0, cur = -1;
            std::vector<std::string> list;
            if (it.w->listRows(list, cur)) n = (int)list.size();
            else if (auto* lw = dynamic_cast<ui::ListWidget*>(it.w)) n = lw->count(), cur = lw->currentRow();
            else if (auto* tw = dynamic_cast<ui::TreeWidget*>(it.w))
                for (int r = 0; r < (n = tw->rowCount()); r++)
                    if (tw->nodeAtRow(r) == tw->current()) cur = r;
            if (cur >= 0) line = cur - std::max(0, std::min(cur - it.rows / 2, n - it.rows));
            top = std::clamp(it.row + std::min(it.rows, ch) - ch, 0, it.row);
            if (it.row + line >= top + ch) top = it.row + line - ch + 1;
        }
        top = std::min(top, rows - ch);
        if (top > 0) s.set(bx + bw - 2, by, U'▲', frame);
        if (top + ch < rows) s.set(bx + bw - 2, by + bh - 1, U'▼', frame);
    }
    auto shows = [&](int y) { return y >= 0 && y < ch; };
    auto put = [&](int x, int y, const std::string& text, Style st, int maxW) {
        if (!shows(y) || x >= cw) return;
        s.text(cx + x, cy + y, text, st, std::min(maxW, cw - x));
    };
    auto band = [&](int x, int y, int w, Style st) {  // a row's ground
        if (shows(y)) s.fill(cx + x, cy + y, std::min(w, cw - x), 1, U' ', st);
    };
    // (a row wider than the box, a toolbar: shifted left so the control with the keyboard is shown; ◀ ▶ beside it: more)
    int wideRow = -1, left = 0;
    for (size_t idx = 0; idx < items.size(); idx++) {
        int end = placedAt[idx] + std::max(widthOf(items[idx]), minWidth(items[idx].w));
        if (hasFocusIn(items[idx].w, focus) && end > cw) wideRow = items[idx].row, left = std::min(end - cw, placedAt[idx]);
    }
    for (size_t idx = 0; idx < items.size(); idx++) {
        int x = placedAt[idx] - (items[idx].row == wideRow ? left : 0), y = items[idx].row - top;
        if (!shows(y)) continue;
        if (x < 0) s.set(cx - 1, cy + y, U'◀', frame);
        if (x >= cw) s.set(cx + cw, cy + y, U'▶', frame);  // (one not drawn at all: those cut short end in "…")
    }
    const Style selFocus = style(Cyan, Black, Reverse | Bold);   // a list's row chosen, the list with the keyboard
    const Style selIdle = style(White, Blue, Bold | Underline);  // …without it
    for (size_t idx = 0; idx < items.size(); idx++) {
        const Item& it = items[idx];
        int x = placedAt[idx] - (it.row == wideRow ? left : 0), y = it.row - top;
        int w = std::min(std::max(widthOf(it), minWidth(it.w)), cw - x);
        if (x < 0 || w <= 0 || y >= ch || y + it.rows <= 0) continue;
        bool focused = hasFocusIn(it.w, focus);
        bool enabled = it.w->isEnabled();
        Style st = frame;
        if (focused) st.attr |= Reverse;
        if (auto* l = dynamic_cast<ui::Label*>(it.w)) {
            auto lines = wrapLines(l->text(), w);
            for (int i = 0; i < (int)lines.size(); i++) put(x, y + i, lines[i], frame, w);
        } else if (auto* b = dynamic_cast<ui::Button*>(it.w)) {
            std::string text = b->text().empty() ? b->toolTip() : b->text();
            bool on = b->isCheckable() && b->isChecked();
            std::string shown = (enabled ? "[" : "(") + std::string(on ? "*" : " ") + text + " " + (enabled ? "]" : ")");
            Style bs = st;
            if (b->isDefault()) bs.attr |= Bold;
            put(x, y, shown, bs, std::max(w, columns(shown)));
        } else if (auto* c = dynamic_cast<ui::CheckBox*>(it.w)) {
            put(x, y, std::string(enabled ? "[" : "(") + (c->isChecked() ? "x" : " ") + (enabled ? "] " : ") ") + c->text(), st,
                std::max(w, 4 + columns(c->text())));
        } else if (auto* e = dynamic_cast<ui::LineEdit*>(it.w)) {
            int fw = std::max(4, w);
            std::string text = e->text().empty() && !focused ? e->placeholder() : e->text();
            std::u32string cps = codePoints(text);
            auto cells = [](char32_t c) { int n = cellWidth(c); return n < 0 ? 1 : n; };
            // (the cursor kept in view: the text shown from as far left as leaves its cell in the field)
            size_t cur = focused ? codePoints(e->text().substr(0, std::min(e->cursorPosition(), e->text().size()))).size() : 0;
            size_t first = 0;
            int before = 0;  // (the cells from the first shown to the cursor)
            for (size_t i = 0; i < cur && i < cps.size(); i++) before += cells(cps[i]);
            while (first < cur && before > fw - 3) before -= cells(cps[first++]);
            std::string shown;
            for (size_t i = first; i < cps.size(); i++) shown += utf8Of(cps[i]);
            Style es = style(Black, White, focused ? Bold : Plain);
            band(x, y, fw, es);
            put(x, y, "[", es, 1);
            put(x + 1, y, shown, es, fw - 2);
            put(x + fw - 1, y, "]", es, 1);
            if (focused && shows(y) && 1 + before < cw - x)  // (the text cursor: a block of the field's colours reversed)
                if (Cell* cell = s.at(cx + x + 1 + before, cy + y)) cell->st = style(Black, White, Reverse | Bold);
        } else if (auto* sp = dynamic_cast<ui::SpinBox*>(it.w)) {
            std::string shown = "[" + sp->text() + " ^v]";
            put(x, y, shown, focused ? style(Black, White, Bold | Reverse) : style(Black, White), std::max(w, columns(shown)));
        } else if (auto* cb = dynamic_cast<ui::ComboBox*>(it.w)) {
            std::string shown = "[" + fit(comboText(cb), std::max(1, w - 4)) + " v]";
            put(x, y, shown, focused ? style(Black, White, Bold | Reverse) : style(Black, White), std::max(w, columns(shown)));
        } else if (auto* sl = dynamic_cast<ui::Slider*>(it.w)) {
            int sw = std::max(5, w - 2);
            double f = sl->maximum() > sl->minimum() ? double(sl->value() - sl->minimum()) / (sl->maximum() - sl->minimum()) : 0;
            int at = (int)std::lround(f * (sw - 1));
            std::string bar = "[";
            for (int i = 0; i < sw; i++) bar += i == at ? "O" : "-";
            put(x, y, bar + "]", st, sw + 2);
        } else if (ownCells(it.w)) {
            int cols = std::min(w, cw - x), rowsN = std::min(it.rows, ch - y);
            std::vector<ui::TextCell> cells;
            if (cols > 0 && rowsN > 0 && it.w->textCells(cols, rowsN, cells) && (int)cells.size() >= cols * rowsN)
                for (int r = std::max(0, -y); r < rowsN; r++)
                    for (int c = 0; c < cols; c++) {
                        const ui::TextCell& t = cells[(size_t)(r * cols + c)];
                        s.set(cx + x + c, cy + y + r, t.ch, Style{t.fg, t.bg, t.attr});
                    }
        } else if (ownList(it.w)) {
            std::vector<std::string> list;
            int cur = -1;
            it.w->listRows(list, cur);
            int n = (int)list.size(), visible = it.rows;
            int first = std::max(0, std::min(cur - visible / 2, n - visible));
            for (int i = 0; i < visible && first + i < n; i++) {
                int r = first + i;
                bool sel = r == cur, ring = focused && cur < 0 && i == 0;  // (none chosen: the keyboard's on the first)
                Style ls = sel ? (focused ? selFocus : selIdle) : ring ? selFocus : frame;
                band(x, y + i, w, ls);
                put(x, y + i, (sel ? "> " : "  ") + list[(size_t)r], ls, w);
            }
            if (n == 0) put(x, y, "(empty)", focused ? selFocus : frame, w);
        } else if (auto* lw = dynamic_cast<ui::ListWidget*>(it.w)) {
            int n = lw->count(), cur = lw->currentRow(), visible = it.rows;
            int first = std::max(0, std::min(cur - visible / 2, n - visible));
            for (int i = 0; i < visible && first + i < n; i++) {
                int r = first + i;
                bool sel = r == cur || lw->isRowSelected(r), ring = focused && cur < 0 && i == 0;
                Style ls = sel ? (focused ? selFocus : selIdle) : ring ? selFocus : frame;
                const ui::ListWidget::Item& item = lw->item(r);
                std::string text = std::string(sel ? "> " : "  ") + (item.checkable ? (item.checked ? "[x] " : "[ ] ") : "") + item.text;
                band(x, y + i, w, ls);
                put(x, y + i, text, ls, w);
            }
            if (n == 0) put(x, y, "(empty)", focused ? selFocus : frame, w);
        } else if (auto* tw = dynamic_cast<ui::TreeWidget*>(it.w)) {
            int n = tw->rowCount(), visible = it.rows, cur = 0;
            for (int r = 0; r < n; r++)
                if (tw->nodeAtRow(r) == tw->current()) cur = r;
            int first = std::max(0, std::min(cur - visible / 2, n - visible));
            for (int i = 0; i < visible && first + i < n; i++) {
                int r = first + i;
                ui::TreeWidget::Node* node = tw->nodeAtRow(r);
                bool sel = node == tw->current();
                std::string mark = node->expandable ? (node->expanded ? "v " : "> ") : "  ";
                std::string text = std::string(tw->depthAtRow(r) * 2, ' ') + mark + node->text;
                Style ls = sel ? (focused ? selFocus : selIdle) : frame;
                band(x, y + i, w, ls);
                put(x, y + i, text, ls, w);
            }
        } else if (auto* tabs = dynamic_cast<ui::TabWidget*>(it.w)) {
            int at = 0;
            for (int i = 0; i < tabs->count(); i++) {
                std::string t = " " + tabs->tabText(i) + " ";
                Style ts = i == tabs->currentIndex() ? style(Black, White, Bold) : frame;
                put(x + at, y, t, ts, cw);
                at += columns(t) + 1;
            }
        } else if (auto* g = dynamic_cast<ui::GroupBox*>(it.w)) {
            put(x, y, "-- " + g->title_ + " --", style(White, Blue, Bold), w);
        } else if (auto* sb = dynamic_cast<ui::StatusBar*>(it.w)) {
            put(x, y, sb->message(), frame, w);
        }
    }
    if (auto* pd = dynamic_cast<ui::ProgressDialog*>(&win)) {
        int pw = cw, lo = pd->minimum(), hi = pd->maximum();
        double f = hi > lo ? double(pd->value() - lo) / (hi - lo) : 0;
        int done = (int)std::lround(std::clamp(f, 0.0, 1.0) * pw), y = rows - 1 - top;
        if (shows(y)) {
            s.fill(cx, cy + y, done, 1, U'█', frame);
            s.fill(cx + done, cy + y, pw - done, 1, U'░', frame);
        }
    }
}

namespace {
// the computer keyboard's notes held: a terminal tells a key pressed (and repeated while held), never let
// go; a note key is let go NOTE_HOLD_S after it was last pressed or repeated (longer than the first repeat
// takes to come: a held key's note goes on)
constexpr int NOTE_HOLD_MS = 700;
struct Held {
    ui::Window* window;
    int timer;
};
std::map<int, Held>& held() {
    static std::map<int, Held> m;
    return m;
}
void letGo(int key) {
    auto it = held().find(key);
    if (it == held().end()) return;
    Held h = it->second;
    held().erase(it);
    if (!ui::Widget::alive(h.window)) return;  // (closed meanwhile)
    plat::Event e;
    e.type = plat::Event::KeyUp;
    e.key = key;
    h.window->onEvent(nullptr, e);
}
}  // namespace

void sendKey(ui::Window& w, const Key& k) {
    plat::Event e;
    e.type = plat::Event::KeyDown;
    e.key = k.key;
    e.mods = k.mods;
    e.text = (k.mods & (plat::ModCtrl | plat::ModAlt)) ? "" : k.text;
    computer_keyboard::ComputerKeyboard* kb = computer_keyboard::active();
    if (kb && kb->isOn() && !k.mods && computer_keyboard::stepOf(k.key) >= 0) {
        ui::App& app = ui::App::get();
        auto it = held().find(k.key);
        if (it != held().end()) {  // (a repeat: held on)
            app.cancelTimer(it->second.timer);
        } else {
            w.onEvent(nullptr, e);
        }
        int key = k.key;
        held()[key] = Held{&w, app.singleShot(NOTE_HOLD_MS, [key] { letGo(key); })};
        return;
    }
    w.onEvent(nullptr, e);  // (as from the window system: event filters, the computer keyboard, popups)
    if (!ui::Widget::alive(&w)) return;  // (the key closed it, and it is gone)
    e.type = plat::Event::KeyUp;
    e.text.clear();
    w.onEvent(nullptr, e);
}

}  // namespace tui
