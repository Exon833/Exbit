// The GUI toolkit, headless: widget trees, layouts, events through the windows' dispatch paths,
// painting into images (pixel checks), text editing, spin boxes, shortcuts, menus, dialogs,
// deleteLater safety. Also writes a gallery of the controls as PNGs to build/toolkit-samples/.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "ui/theme.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/frame.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/menu.h"
#include "ui/toolkit/style.h"
#include "ui/toolkit/widget.h"

using namespace ui;

namespace {
double gTime = 100;

plat::Event mouseEv(plat::Event::Type t, int x, int y, int button, int buttons, int mods = 0) {
    plat::Event e;
    e.type = t;
    e.x = x;
    e.y = y;
    e.button = button;
    e.buttons = buttons;
    e.mods = mods;
    e.time = gTime;
    return e;
}

void press(Window& w, int x, int y, int button = plat::LeftButton, int mods = 0) {
    gTime += 1.0;
    w.onEvent(nullptr, mouseEv(plat::Event::MouseMove, x, y, 0, 0, mods));
    w.onEvent(nullptr, mouseEv(plat::Event::MouseDown, x, y, button, 0, mods));
}
void release(Window& w, int x, int y, int button = plat::LeftButton, int mods = 0) {
    w.onEvent(nullptr, mouseEv(plat::Event::MouseUp, x, y, button, button, mods));
}
void click(Window& w, int x, int y, int button = plat::LeftButton, int mods = 0) {
    press(w, x, y, button, mods);
    release(w, x, y, button, mods);
}
void moveTo(Window& w, int x, int y, int buttons = 0) {
    w.onEvent(nullptr, mouseEv(plat::Event::MouseMove, x, y, 0, buttons));
}
void doubleClick(Window& w, int x, int y) {
    click(w, x, y);
    gTime += 0.1;
    w.onEvent(nullptr, mouseEv(plat::Event::MouseDown, x, y, plat::LeftButton, 0));
    release(w, x, y);
}
void key(Window& w, int k, int mods = 0, const std::string& text = "") {
    plat::Event e;
    e.type = plat::Event::KeyDown;
    e.key = k;
    e.mods = mods;
    e.text = text;
    e.time = gTime;
    w.onEvent(nullptr, e);
    e.type = plat::Event::KeyUp;
    w.onEvent(nullptr, e);
}
void type(Window& w, const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        size_t j = utf8NextBoundary(s, i);
        std::string ch = s.substr(i, j - i);
        int k = (unsigned char)ch[0];
        if (k >= 'a' && k <= 'z') k = k - 'a' + 'A';
        key(w, ch.size() == 1 ? k : 0x2000, 0, ch);
        i = j;
    }
}

bool near(Color a, Color b, int tol = 6) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

std::string outDir() {
    std::string d = "build/toolkit-samples";
    util::makeDirs(d);
    return d;
}

void savePicture(Window& w, const std::string& name) {
    w.update();
    App::get().tick();
    savePng(outDir() + "/" + name + ".png", w.framebuffer());
}
}  // namespace

// ------------------------------------------------------------------------------------------
static void testSignalsAndKeys() {
    Signal<int> s;
    int sum = 0;
    int a = s.connect([&](int v) { sum += v; });
    s.connect([&](int v) { sum += 10 * v; });
    s.emit(1);
    CHECK(sum == 11);
    s.disconnect(a);
    s.emit(1);
    CHECK(sum == 21);
    CHECK(!s.empty());
    s.disconnectAll();
    CHECK(s.empty());

    KeySequence k = KeySequence::parse("Ctrl+Shift+S");
    CHECK(k.key == 'S' && k.mods == (plat::ModCtrl | plat::ModShift));
    CHECK(k.text() == "Ctrl+Shift+S");
    CHECK(KeySequence::parse("Space").key == ' ');
    CHECK(KeySequence::parse("Space").text() == "Space");
    CHECK(KeySequence::parse("Del").key == plat::Key_Delete);
    CHECK(KeySequence::parse("Delete").text() == "Del");
    CHECK(KeySequence::parse("F1").key == plat::Key_F1);
    CHECK(KeySequence::parse("ctrl+z").text() == "Ctrl+Z");
    CHECK(KeySequence::parse("Ctrl++").key == '+');
    CHECK(KeySequence::parse("Ctrl++").mods == plat::ModCtrl);
    CHECK(KeySequence::parse("Home").text() == "Home");
    CHECK(KeySequence::parse("Bogus+X").empty());
    KeyEvent e;
    e.key = 'S';
    e.mods = plat::ModCtrl | plat::ModShift;
    CHECK(k.matches(e));
    e.mods = plat::ModCtrl;
    CHECK(!k.matches(e));
    KeyEvent plus;
    plus.key = '+';
    plus.mods = plat::ModCtrl | plat::ModShift;
    CHECK(KeySequence::parse("Ctrl++").matches(plus));
}

// a wrapped text laid out wider than the wrap width: as tall as its lines there, no empty line under it (a dialog's
// paragraphs had one between them)
static void testWrappedLabelHeight() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(700, 300);
    auto* v = vbox(&w, 0, 0);
    auto* l = w.add<Label>("Tiny DAW didn't close normally last time. It kept your changes to Song.json.");
    l->setWordWrap(true);
    v->addWidget(l);
    auto* one = w.add<Label>("One line");  // (not wrapped: one line's height)
    v->addWidget(one);
    v->addStretch(1);
    w.relayout();
    App::get().doLayouts();  // (laid out again once it knows its width, as before every frame)
    CHECK(Font(l->font()).width(l->text()) > theme::metric("label-wrap-width", 360));  // (two lines at the wrap width)
    CHECK(Font(l->font()).width(l->text()) < l->width());                              // (one at its own)
    CHECK(l->height() == one->height());
}

static void testLayouts() {
    testWrappedLabelHeight();
    Window w(plat::WindowOptions{});
    w.resizeWindow(400, 100);
    auto* h = hbox(&w, 10, 5);
    auto* a = w.add<Button>("A");
    auto* b = w.add<Widget>();
    b->hPolicy = SizePolicy::Expanding;
    auto* c = w.add<Widget>();
    c->setFixedSize(50, 20);
    h->addWidget(a);
    h->addWidget(b);
    h->addWidget(c);
    w.relayout();
    // A at its hint, C fixed, B takes the rest
    CHECK(a->x() == 10);
    CHECK(a->width() == a->sizeHint().x);
    CHECK(c->width() == 50 && c->height() == 20);
    CHECK(c->x() + c->width() == 390);
    CHECK(b->x() == a->x() + a->width() + 5);
    CHECK(b->x() + b->width() + 5 == c->x());
    // fixed-height button centred vertically
    CHECK(a->height() == a->sizeHint().y);
    CHECK(std::abs(a->y() + a->height() / 2 - 50) <= 1);
    // stretch factors
    Window w2(plat::WindowOptions{});
    w2.resizeWindow(300, 50);
    auto* v = hbox(&w2, 0, 0);
    auto* x1 = w2.add<Widget>();
    auto* x2 = w2.add<Widget>();
    v->addWidget(x1, 1);
    v->addWidget(x2, 2);
    w2.relayout();
    CHECK(x1->width() == 100 && x2->width() == 200);
    CHECK(x1->height() == 50);
    // hidden widgets take no room
    x1->hide();
    App::get().doLayouts();
    CHECK(x2->width() == 300);
    // addStretch pushes to the end
    Window w3(plat::WindowOptions{});
    w3.resizeWindow(300, 40);
    auto* l3 = hbox(&w3, 0, 4);
    l3->addStretch();
    auto* ok = w3.add<Button>("OK");
    l3->addWidget(ok);
    w3.relayout();
    CHECK(ok->x() + ok->width() == 300);
    // nested layouts and form
    Window w4(plat::WindowOptions{});
    w4.resizeWindow(300, 200);
    auto* outer = vbox(&w4, 8, 6);
    auto* f = new FormLayout();
    outer->addLayout(std::unique_ptr<Layout>(f));
    auto* le = w4.add<LineEdit>();
    f->addRow("Name:", le);
    auto* cb = w4.add<CheckBox>("On");
    f->addRow(cb);
    w4.relayout();
    CHECK(le->x() > 8 + 20);
    CHECK(le->x() + le->width() == 292);
    CHECK(cb->x() == 8);
    CHECK(cb->y() > le->y());
    // sizeHint of a layout-ed widget includes margins
    Point sh = w4.sizeHint();
    CHECK(sh.y >= le->height() + cb->height() + 16);
    // grid
    Window w5(plat::WindowOptions{});
    w5.resizeWindow(200, 100);
    auto* g = grid(&w5, 0, 0);
    auto* g1 = w5.add<Widget>();
    auto* g2 = w5.add<Widget>();
    auto* g3 = w5.add<Widget>();
    g->addWidget(g1, 0, 0);
    g->addWidget(g2, 0, 1);
    g->addWidget(g3, 1, 0, 1, 2);
    w5.relayout();
    CHECK(g1->width() == 100 && g2->x() == 100);
    CHECK(g3->width() == 200 && g3->y() == 50);

    // deep nesting: a widget is measured about once per ancestor laid out, not once per path through
    // its ancestors' questions (a panel of track rows measured that way took ~40 ms a pixel of a drag)
    struct Counted : Widget {
        int* count;
        explicit Counted(int* c) : count(c) {}
        Point sizeHint() const override {
            ++*count;
            return Point(20, 10);
        }
    };
    int measured = 0;
    Window w6(plat::WindowOptions{});
    w6.resizeWindow(300, 400);
    Widget* box = &w6;
    Counted* leaf = nullptr;
    for (int depth = 0; depth < 8; ++depth) {
        BoxLayout* lay = depth % 2 ? (BoxLayout*)hbox(box, 1, 1) : (BoxLayout*)vbox(box, 1, 1);
        leaf = box->add<Counted>(&measured);
        lay->addWidget(leaf);
        auto* inner = box->add<Widget>();
        lay->addWidget(inner, 1);
        box = inner;
    }
    App::get().doLayouts();
    w6.relayout();
    measured = 0;
    w6.resizeWindow(320, 420);
    w6.relayout();
    CHECK(measured > 0 && measured < 1000);  // (about one per ancestor laid out; it was 2,956,116)
    CHECK(leaf->width() == 20 && leaf->height() > 10);  // (in a row: its hint across, the row's height down)
}

static void testPaintingColours() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(300, 60);
    auto* b = w.add<Button>("Lit");
    b->setStyle(Button::Style::Tool);
    b->setGeometry(10, 10, 40, 22);
    b->setProperty("lit", "true");
    auto* n = w.add<Button>("Plain");
    n->setGeometry(60, 10, 60, 26);
    auto* m = w.add<Button>("M");
    m->setStyle(Button::Style::Tool);
    m->setGeometry(130, 10, 26, 22);
    m->setCheckable(true);
    m->setProperty("toggle", "mute");
    m->setChecked(true);
    auto* cb = w.add<CheckBox>("x");
    cb->setGeometry(170, 10, 60, 20);
    cb->setChecked(true);
    w.showWindow();
    App::get().tick();
    const Image& fb = w.framebuffer();
    CHECK(near(fb.colorAt(13, 16), theme::c("accent")));          // lit: accent fill
    CHECK(near(fb.colorAt(62, 12), theme::c("button")));          // plain button fill
    CHECK(near(fb.colorAt(135, 14), theme::c("mute")));           // checked mute toggle
    CHECK(near(fb.colorAt(181, 24), theme::c("accent")));         // checked check box
    CHECK(near(fb.colorAt(250, 50), theme::c("window")));         // background
    // hover lightens
    moveTo(w, 90, 20);
    CHECK(n->underMouse());
    App::get().tick();
    CHECK(near(w.framebuffer().colorAt(64, 14), theme::shades().hover));
    // pressing darkens to "pressed"
    press(w, 90, 20);
    App::get().tick();
    CHECK(n->isPressed());
    CHECK(near(w.framebuffer().colorAt(64, 14), theme::shades().pressed));
    release(w, 90, 20);
    w.hideWindow();
}

static void testButtonsAndClicks() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(300, 100);
    w.showWindow();
    int clicks = 0;
    auto* b = pushButton(&w, "Go", [&] { ++clicks; });
    b->setGeometry(10, 10, 80, 26);
    click(w, 20, 20);
    CHECK(clicks == 1);
    // release outside: no click
    press(w, 20, 20);
    moveTo(w, 200, 80, plat::LeftButton);
    release(w, 200, 80);
    CHECK(clicks == 1);
    // checkable toggles
    bool last = false;
    auto* t = toolButton(&w, "T");
    t->setGeometry(100, 10, 30, 22);
    t->setCheckable(true);
    t->toggled.connect([&](bool on) { last = on; });
    click(w, 110, 20);
    CHECK(t->isChecked() && last);
    click(w, 110, 20);
    CHECK(!t->isChecked() && !last);
    // disabled: nothing
    b->setEnabled(false);
    click(w, 20, 20);
    CHECK(clicks == 1);
    b->setEnabled(true);
    // the implicit grab: moves go to the pressed widget even outside it
    int moves = 0;
    auto* canvas = w.add<Canvas>();
    canvas->setGeometry(150, 10, 50, 50);
    canvas->onPress = [&](MouseEvent&) { return true; };
    canvas->onMove = [&](MouseEvent& e) {
        if (e.pos.x > 50) ++moves;
        return true;
    };
    press(w, 160, 20);
    moveTo(w, 280, 20, plat::LeftButton);
    CHECK(moves == 1);
    CHECK(w.grabWidget() == canvas);
    release(w, 280, 20);
    CHECK(w.grabWidget() == nullptr);
    // double click
    int dbl = 0, presses = 0;
    canvas->onPress = [&](MouseEvent&) {
        ++presses;
        return true;
    };
    canvas->onDoubleClick = [&](MouseEvent&) {
        ++dbl;
        return true;
    };
    doubleClick(w, 170, 30);
    CHECK(presses == 1 && dbl == 1);
    // right click: press then context menu, propagating to the parent
    int ctx = 0;
    auto* parentBox = w.add<Canvas>();
    parentBox->setGeometry(0, 60, 300, 40);
    auto* child = parentBox->add<Label>("x");
    child->setGeometry(5, 5, 40, 20);
    parentBox->onContextMenu = [&](MouseEvent& e) {
        ++ctx;
        CHECK(e.pos.x == 10);
        return true;
    };
    click(w, 10, 70, plat::RightButton);
    CHECK(ctx == 1);
    // wheel goes up to the parent that takes it
    int wheels = 0;
    parentBox->onWheel = [&](WheelEvent& e) {
        wheels += (int)e.dy;
        return true;
    };
    plat::Event we;
    we.type = plat::Event::Wheel;
    we.x = 10;
    we.y = 70;
    we.dy = 1;
    w.onEvent(nullptr, we);
    CHECK(wheels == 1);
    w.hideWindow();
}

static void testLineEdit() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(300, 60);
    w.showWindow();
    auto* e = w.add<LineEdit>();
    e->setGeometry(10, 10, 200, 22);
    int edits = 0;
    e->textEdited.connect([&](const std::string&) { ++edits; });
    click(w, 20, 20);
    CHECK(e->hasFocus());
    type(w, "héllo");
    CHECK(e->text() == "héllo");
    CHECK(edits == 5);
    key(w, plat::Key_Backspace);
    CHECK(e->text() == "héll");
    key(w, plat::Key_Home);
    key(w, plat::Key_Right);
    key(w, plat::Key_Delete);  // the é (two bytes)
    CHECK(e->text() == "hll");
    key(w, plat::Key_End);
    key(w, plat::Key_Left, plat::ModShift);
    key(w, plat::Key_Left, plat::ModShift);
    CHECK(e->selectedText() == "ll");
    key(w, 'C', plat::ModCtrl);
    CHECK(clipboard() == "ll");
    key(w, 'X', plat::ModCtrl);
    CHECK(e->text() == "h");
    key(w, 'V', plat::ModCtrl);
    key(w, 'V', plat::ModCtrl);
    CHECK(e->text() == "hllll");
    key(w, 'A', plat::ModCtrl);
    type(w, "new");
    CHECK(e->text() == "new");
    key(w, 'Z', plat::ModCtrl);
    CHECK(e->text() == "ne");
    // validator
    e->setText("");
    e->validator = [](const std::string& c) { return c.find_first_not_of("0123456789") == std::string::npos; };
    type(w, "1a2");
    CHECK(e->text() == "12");
    e->validator = nullptr;
    // max length
    e->setText("");
    e->setMaxLength(3);
    type(w, "abcdef");
    CHECK(e->text() == "abc");
    e->setMaxLength(32767);
    // a long text scrolls; Home goes back
    e->setText("");
    type(w, "the quick brown fox jumps over the lazy dog and keeps running far away");
    CHECK(e->cursorPosition() == e->text().size());
    // shortcut override: Space typed in the field is text, Ctrl+S is the window's shortcut
    int plays = 0, saves = 0;
    Shortcut sp(&w, KeySequence::parse("Space"), [&] { ++plays; });
    Shortcut sv(&w, KeySequence::parse("Ctrl+S"), [&] { ++saves; });
    e->setText("a");
    key(w, ' ', 0, " ");
    CHECK(plays == 0);
    CHECK(e->text() == "a ");
    key(w, 'S', plat::ModCtrl, "\x13");
    CHECK(saves == 1);
    // without focus in a field, Space plays
    e->clearFocus();
    key(w, ' ', 0, " ");
    CHECK(plays == 1);
    // Return: returnPressed, then the key goes on
    int returns = 0;
    e->returnPressed.connect([&] { ++returns; });
    e->setFocus();
    key(w, plat::Key_Return);
    CHECK(returns == 1);
    // select a word with a double click
    e->setText("alpha beta");
    Font f = e->font();
    int bx = 10 + 6 + (int)f.width("alpha b");
    doubleClick(w, bx, 20);
    CHECK(e->selectedText() == "beta");
    w.hideWindow();
}

static void testSpinBox() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(300, 60);
    w.showWindow();
    auto* s = w.add<SpinBox>();
    s->setGeometry(10, 10, 100, 22);
    s->setRange(0, 200);
    s->setSuffix(" ms");
    double last = -1;
    int changes = 0;
    s->valueChanged.connect([&](double v) {
        last = v;
        ++changes;
    });
    s->setValue(20);
    CHECK(s->text() == "20 ms");
    CHECK(last == 20);
    s->stepBy(3);
    CHECK(s->value() == 23);
    s->setValue(500);
    CHECK(s->value() == 200);
    // typing: tracked while valid and in range; Return commits and reformats
    s->editor()->setFocus();
    key(w, 'A', plat::ModCtrl);
    type(w, "45");
    CHECK(s->value() == 45);
    key(w, plat::Key_Return);
    CHECK(s->text() == "45 ms");
    // arrow keys
    key(w, plat::Key_Up);
    CHECK(s->value() == 46);
    key(w, plat::Key_Down);
    key(w, plat::Key_Down);
    CHECK(s->value() == 44);
    // wheel
    plat::Event we;
    we.type = plat::Event::Wheel;
    we.x = 30;
    we.y = 20;
    we.dy = 2;
    w.onEvent(nullptr, we);
    CHECK(s->value() == 46);
    // the up button (right side, top half)
    click(w, 10 + 100 - 6, 13);
    CHECK(s->value() == 47);
    click(w, 10 + 100 - 6, 29);
    CHECK(s->value() == 46);
    // decimals
    auto* d = w.add<SpinBox>();
    d->setGeometry(120, 10, 100, 22);
    d->setRange(-10, 10);
    d->setDecimals(2);
    d->setSingleStep(0.25);
    d->setValue(1.234);
    CHECK_NEAR(d->value(), 1.23, 1e-9);
    CHECK(d->text() == "1.23");
    d->stepBy(1);
    CHECK_NEAR(d->value(), 1.48, 1e-9);
    // garbage typed: back to the value on focus out
    d->editor()->setFocus();
    key(w, 'A', plat::ModCtrl);
    type(w, "x");
    s->editor()->setFocus();
    CHECK(d->text() == "1.48");
    // keyboard tracking off: only on commit
    d->setKeyboardTracking(false);
    int dch = 0;
    d->valueChanged.connect([&](double) { ++dch; });
    d->editor()->setFocus();
    key(w, 'A', plat::ModCtrl);
    type(w, "5");
    CHECK(dch == 0);
    key(w, plat::Key_Return);
    CHECK(dch == 1 && d->value() == 5);
    w.hideWindow();
}

static void testComboAndMenus() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(400, 300);
    w.moveWindow(100, 100);
    w.showWindow();
    auto* c = w.add<ComboBox>();
    c->setGeometry(10, 10, 150, 22);
    c->addItem("One", 1);
    c->addItem("Two", 2);
    c->insertSeparator();
    c->addItem("Three", 3);
    CHECK(c->currentIndex() == 0);
    CHECK(c->currentDataInt() == 1);
    int act = -1;
    c->activated.connect([&](int i) { act = i; });
    // the list opens under it; a click on "Two" chooses it
    click(w, 50, 20);
    CHECK(c->isPopupOpen());
    CHECK(App::get().popups().size() == 1);
    Window* pop = App::get().popups().back();
    Rect pg = pop->screenGeometry();
    CHECK(pg.y == 100 + 32);
    // second row's middle (border 1 + padding 2, rows ~20 px)
    int y2 = 3 + 20 + 10;
    plat::Event m = mouseEv(plat::Event::MouseMove, 30, y2, 0, 0);
    pop->onEvent(nullptr, m);
    pop->onEvent(nullptr, mouseEv(plat::Event::MouseDown, 30, y2, plat::LeftButton, 0));
    pop->onEvent(nullptr, mouseEv(plat::Event::MouseUp, 30, y2, plat::LeftButton, plat::LeftButton));
    CHECK(!c->isPopupOpen());
    CHECK(c->currentIndex() == 1 && act == 1);
    // keyboard: Down skips the separator
    c->setFocus();
    key(w, plat::Key_Down);
    CHECK(c->currentIndex() == 3);
    // wheel off
    c->setWheelEnabled(false);
    plat::Event we;
    we.type = plat::Event::Wheel;
    we.x = 50;
    we.y = 20;
    we.dy = 1;
    w.onEvent(nullptr, we);
    CHECK(c->currentIndex() == 3);
    CHECK(c->findData(2) == 1 && c->findText("Three") == 3);

    // menus: popup, choose, triggered after closing
    Menu menu;
    int hits = 0;
    bool toggledOn = false;
    menu.addAction("First", [&] { ++hits; }, "Ctrl+F");
    menu.addSeparator();
    menu.addCheckable("Flag", false, [&](bool on) { toggledOn = on; });
    Menu* sub = menu.addMenu("More");
    int subHits = 0;
    sub->addAction("Deep", [&] { ++subHits; });
    Action* dis = menu.addAction("Disabled", [&] { hits += 100; });
    dis->enabled = false;
    int shown = 0;
    menu.aboutToShow = [&](Menu*) { ++shown; };
    menu.popup(Point(150, 150), &w);
    CHECK(menu.isOpen());
    CHECK(shown == 1);
    Window* mp = App::get().popups().back();
    // first item: y = 1 + 4 + itemH/2
    mp->onEvent(nullptr, mouseEv(plat::Event::MouseMove, 40, 14, 0, 0));
    mp->onEvent(nullptr, mouseEv(plat::Event::MouseDown, 40, 14, plat::LeftButton, 0));
    mp->onEvent(nullptr, mouseEv(plat::Event::MouseUp, 40, 14, plat::LeftButton, plat::LeftButton));
    CHECK(!menu.isOpen());
    CHECK(hits == 0);  // (after the event)
    App::get().advanceTimers(0.001);
    CHECK(hits == 1);
    // keyboard: Down, Down (skips separator) → Flag, Return toggles
    menu.popup(Point(150, 150), &w);
    key(w, plat::Key_Down);
    key(w, plat::Key_Down);
    key(w, plat::Key_Return);
    App::get().advanceTimers(0.001);
    CHECK(toggledOn);
    CHECK(menu.actions()[2]->checked);
    // a press outside closes it (and nothing runs)
    menu.popup(Point(150, 150), &w);
    plat::Event out = mouseEv(plat::Event::MouseDown, 5, 290, plat::LeftButton, 0);
    w.onEvent(nullptr, out);
    w.onEvent(nullptr, mouseEv(plat::Event::MouseUp, 5, 290, plat::LeftButton, plat::LeftButton));
    CHECK(!menu.isOpen());
    CHECK(App::get().popups().empty());
    // submenu opens with Right, Return triggers it
    menu.popup(Point(150, 150), &w);
    key(w, plat::Key_Up);   // (wraps to the last selectable: More, Disabled is skipped)
    key(w, plat::Key_Right);
    CHECK(App::get().popups().size() == 2);
    key(w, plat::Key_Return);
    App::get().advanceTimers(0.001);
    CHECK(subHits == 1);
    CHECK(App::get().popups().empty());
    // exec with a timer choosing
    App::get().singleShot(50, [&] { key(w, plat::Key_Down); key(w, plat::Key_Return); });
    Action* got = menu.exec(Point(150, 150), &w);
    CHECK(got == menu.actions()[0].get());
    CHECK(hits == 2);
    // exclusive groups
    Menu g;
    Action* g1 = g.addCheckable("A", true);
    Action* g2 = g.addCheckable("B", false);
    g1->group = g2->group = 1;
    Menu::activate(g2);
    CHECK(g2->checked && !g1->checked);
    Menu::activate(g2);
    CHECK(g2->checked);
    // shortcuts registered from the menu
    menu.registerShortcuts(&w);
    key(w, 'F', plat::ModCtrl);
    CHECK(hits == 3);
    menu.actions()[0]->enabled = false;
    key(w, 'F', plat::ModCtrl);
    CHECK(hits == 3);
    w.hideWindow();
}

static void testDeleteLaterAndForget() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(200, 100);
    w.showWindow();
    auto* b = w.add<Button>("x");
    b->setGeometry(10, 10, 50, 22);
    bool deleted = false;
    struct Probe : Widget {
        bool* flag;
        explicit Probe(bool* f) : flag(f) {}
        ~Probe() override { *flag = true; }
    };
    auto* probe = b->add<Probe>(&deleted);
    (void)probe;
    // deleting itself from its own click handler
    b->clicked.connect([b] { b->deleteLater(); });
    moveTo(w, 20, 20);
    CHECK(w.hoverWidget() != nullptr);
    click(w, 20, 20);
    App::get().collectGarbage();
    CHECK(deleted);
    CHECK(w.hoverWidget() == &w || w.hoverWidget() == nullptr);
    CHECK(w.grabWidget() == nullptr);
    moveTo(w, 25, 20);  // no crash on the stale place
    // focus pointer forgotten
    auto* e = w.add<LineEdit>();
    e->setGeometry(10, 40, 100, 22);
    e->setFocus();
    CHECK(w.focusWidget() == e);
    delete w.release(e).release();
    CHECK(w.focusWidget() == nullptr);
    key(w, 'A', 0, "a");  // no crash
    // layouts forget deleted widgets
    Window w2(plat::WindowOptions{});
    w2.resizeWindow(200, 40);
    auto* l = hbox(&w2, 0, 0);
    auto* x = w2.add<Widget>();
    auto* y = w2.add<Widget>();
    l->addWidget(x);
    l->addWidget(y);
    x->deleteLater();
    App::get().collectGarbage();
    CHECK(l->count() == 1);
    w2.relayout();
    CHECK(y->width() == 200);
    // shortcut owner deleted: the shortcut is inactive
    auto* owner = w.add<Widget>();
    int fired = 0;
    Shortcut sc(owner, KeySequence::parse("F5"), [&] { ++fired; });
    key(w, plat::Key_F5);
    CHECK(fired == 1);
    owner->deleteLater();
    App::get().collectGarbage();
    CHECK(sc.owner == nullptr);
    key(w, plat::Key_F5);
    CHECK(fired == 1);
    w.hideWindow();
}

static void testScrollAndLists() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(200, 150);
    w.showWindow();
    auto* sa = w.add<ScrollArea>();
    sa->setGeometry(0, 0, 200, 150);
    auto* content = sa->makeContent<Widget>();
    auto* v = vbox(content, 0, 0);
    for (int i = 0; i < 20; ++i) {
        auto* r = content->add<Widget>();
        r->setFixedHeight(30);
        v->addWidget(r);
    }
    App::get().doLayouts();
    CHECK(sa->verticalScrollBar()->isVisible());
    CHECK(sa->verticalScrollBar()->maximum() == 600 - 150);
    CHECK(content->width() == 200 - theme::SCROLLBAR_W);
    plat::Event we;
    we.type = plat::Event::Wheel;
    we.x = 50;
    we.y = 50;
    we.dy = -1;
    w.onEvent(nullptr, we);
    CHECK(sa->verticalScrollBar()->value() == 60);
    CHECK(content->y() == -60);
    sa->ensureVisible(Rect(0, 570, 10, 30));
    CHECK(sa->verticalScrollBar()->value() == 450);
    // list widget
    auto* lw = w.add<ListWidget>();
    lw->setGeometry(0, 0, 200, 150);
    for (int i = 0; i < 30; ++i) lw->addItem("Item " + std::to_string(i));
    int cur = -1, actv = -1;
    lw->currentRowChanged.connect([&](int r) { cur = r; });
    lw->itemActivated.connect([&](int r) { actv = r; });
    click(w, 20, 5);
    CHECK(cur == 0);
    key(w, plat::Key_Down);
    CHECK(cur == 1);
    key(w, plat::Key_End);
    CHECK(cur == 29);
    CHECK(lw->scrollBar()->value() == 30 * 22 - 150);
    key(w, plat::Key_Return);
    CHECK(actv == 29);
    lw->hide();
    // tree with lazy fill
    auto* tw = w.add<TreeWidget>();
    tw->setGeometry(0, 0, 200, 150);
    int fills = 0;
    tw->onFill = [&](TreeWidget::Node* n) {
        ++fills;
        n->add("child a");
        n->add("child b");
    };
    tw->root()->add("Folder", "/f", true);
    tw->root()->add("File", "/x");
    tw->refreshRows();
    CHECK(tw->rowCount() == 2);
    click(w, 5, 10);  // the arrow
    CHECK(fills == 1 && tw->rowCount() == 4);
    click(w, 5, 10);
    CHECK(tw->rowCount() == 2);
    click(w, 5, 10);
    CHECK(fills == 1 && tw->rowCount() == 4);
    TreeWidget::Node* act = nullptr;
    tw->activated.connect([&](TreeWidget::Node* n) { act = n; });
    doubleClick(w, 60, 70);  // row 3: child b? rows: Folder, a, b, File → y 70 → row 3: File
    CHECK(act && act->text == "File");
    // drag from the tree into a drop target
    struct Target : Widget {
        std::vector<std::string> got;
        bool entered = false;
        void dragEnter(DragEvent& e) override {
            entered = true;
            e.accepted = true;
        }
        void drop(DragEvent& e) override { got = e.data->files; }
    };
    tw->setGeometry(0, 0, 100, 150);
    auto* target = w.add<Target>();
    target->setGeometry(100, 0, 100, 150);
    target->setAcceptDrops(true);
    tw->dragDataFor = [](TreeWidget::Node* n) {
        DragData d;
        d.files = {n->data};
        return d;
    };
    press(w, 40, 10);
    moveTo(w, 60, 10, plat::LeftButton);
    CHECK(App::get().drag() != nullptr);
    moveTo(w, 150, 50, plat::LeftButton);
    CHECK(target->entered);
    release(w, 150, 50);
    CHECK(App::get().drag() == nullptr);
    CHECK(target->got.size() == 1 && target->got[0] == "/f");
    w.hideWindow();
}

// a drag that came from outside the program's windows (a Windows plugin's, through its Wine host): it
// follows the pointer, asked every frame, and drops where the left button is let go
static void testPointerDrag() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(300, 200);
    w.showWindow();
    struct Target : Widget {
        std::vector<std::string> got;
        int enters = 0;
        void dragEnter(DragEvent& e) override {
            enters++;
            e.accepted = !e.data->files.empty();
        }
        void drop(DragEvent& e) override { got = e.data->files; }
    };
    auto* target = w.add<Target>();
    target->setGeometry(150, 0, 150, 200);
    target->setAcceptDrops(true);
    Rect g = w.screenGeometry();
    Point at(g.x + 10, g.y + 10);
    int buttons = plat::LeftButton;
    App::get().pointerForDrags = [&] { return std::make_pair(at, buttons); };
    DragData d;
    d.files = {"/tmp/from a plugin.mid"};
    App::get().followPointerDrag(d);
    CHECK(App::get().drag() != nullptr);
    App::get().advanceTimers(0.05);
    CHECK(target->enters == 0 && App::get().drag());  // (not over it yet)
    at = Point(g.x + 200, g.y + 50);
    App::get().advanceTimers(0.05);
    CHECK(target->enters > 0 && App::get().drag());
    buttons = 0;  // let go over it
    App::get().advanceTimers(0.05);
    CHECK(App::get().drag() == nullptr);
    CHECK(target->got.size() == 1 && target->got[0] == "/tmp/from a plugin.mid");
    // let go over nothing that takes it: no drop, the drag ends
    target->got.clear();
    at = Point(g.x + 10, g.y + 10);
    buttons = plat::LeftButton;
    App::get().followPointerDrag(d);
    buttons = 0;
    App::get().advanceTimers(0.05);
    CHECK(App::get().drag() == nullptr && target->got.empty());
    App::get().pointerForDrags = nullptr;
    App::get().followPointerDrag(d);  // (headless without a pointer to ask: nothing starts)
    CHECK(App::get().drag() == nullptr);
    w.hideWindow();
}

static void testDialogs() {
    Window main(plat::WindowOptions{});
    main.resizeWindow(500, 400);
    main.showWindow();
    // exec ends by a timer pressing Escape
    Dialog d(&main, "Test", 300, 200);
    auto* v = vbox(&d, 10, 6);
    auto* bb = d.add<ButtonBox>(ButtonBox::Ok | ButtonBox::Cancel);
    v->addStretch();
    v->addWidget(bb);
    bb->accepted.connect([&] { d.accept(); });
    bb->rejected.connect([&] { d.reject(); });
    CHECK(bb->button(ButtonBox::Ok)->isDefault());
    CHECK(bb->button(ButtonBox::Ok)->text() == "OK");
    App::get().singleShot(30, [&] { key(d, plat::Key_Escape); });
    CHECK(d.exec() == Dialog::Rejected);
    // Return presses the default button
    App::get().singleShot(30, [&] { key(d, plat::Key_Return); });
    CHECK(d.exec() == Dialog::Accepted);
    // OK sits right of… no: Cancel is right of OK
    d.adjustSize();
    d.showWindow();
    App::get().doLayouts();
    CHECK(bb->button(ButtonBox::Ok)->x() < bb->button(ButtonBox::Cancel)->x());
    d.hideWindow();
    // the modal dialog blocks the main window's input
    int mainClicks = 0;
    auto* mb = pushButton(&main, "m", [&] { ++mainClicks; });
    mb->setGeometry(10, 10, 60, 26);
    App::get().singleShot(30, [&] {
        click(main, 20, 20);
        key(d, plat::Key_Escape);
    });
    d.exec();
    CHECK(mainClicks == 0);
    click(main, 20, 20);
    CHECK(mainClicks == 1);
    // message box: Save / Don't Save / Cancel, in that order; Escape gives Cancel
    App::get().singleShot(30, [&] {
        Window* top = App::get().modalWindow();
        if (top) key(*top, plat::Key_Escape);
    });
    int r = msg::question(&main, "Unsaved changes", "Save the changes to the current project?",
                          msg::Save | msg::Discard | msg::Cancel);
    CHECK(r == msg::Cancel);
    App::get().singleShot(30, [&] {
        Window* top = App::get().modalWindow();
        if (top) key(*top, plat::Key_Return);
    });
    r = msg::question(&main, "Unsaved changes", "Save the changes?", msg::Save | msg::Discard | msg::Cancel);
    CHECK(r == msg::Save);
    // getText
    std::string text = "old";
    App::get().singleShot(30, [&] {
        Window* top = App::get().modalWindow();
        if (!top) return;
        type(*top, "new name");
        key(*top, plat::Key_Return);
    });
    CHECK(getText(&main, "Rename", "Name:", text));
    CHECK(text == "new name");
    // a long question: wrapped into lines (a dialog of a sensible width), every line shown
    const std::string longQuestion =
        "Empty: Tiny DAW's own application. Another: make one at discord.com/developers/applications "
        "(New Application), then copy its Application ID here.";
    auto wrappedWhole = [&](const char* what) {
        App::get().singleShot(30, [&, what] {
            Window* top = App::get().modalWindow();
            if (!top) return;
            App::get().doLayouts();
            Label* lab = nullptr;
            for (auto& c : top->children())
                if (auto* l = dynamic_cast<Label*>(c.get())) lab = l;
            int room = theme::metric("label-wrap-width", 360) + 2 * theme::metric("input-dialog-margin", 12) + 40;
            if (top->width() > room) printf("%s with a long question: %d px wide (at most %d)\n", what, top->width(), room);
            CHECK(top->width() <= room);
            CHECK(lab && lab->height() >= lab->minimumSizeHint().y && lab->height() > 2 * lab->font().height());
            key(*top, plat::Key_Escape);
        });
    };
    wrappedWhole("getText");
    CHECK(!getText(&main, "Discord application ID", longQuestion, text));
    int chosenItem = 0;
    wrappedWhole("getItem");
    CHECK(!getItem(&main, "Choose", longQuestion, {"One", "Two"}, chosenItem));
    main.hideWindow();
}

static void testFileDialog() {
    std::string dir = "build/toolkit-samples/files";
    util::makeDirs(dir + "/sub");
    util::writeFile(dir + "/b10.wav", "x");
    util::writeFile(dir + "/b9.wav", "x");
    util::writeFile(dir + "/notes.txt", "x");
    std::string abs = util::absolutePath(dir);
    Window main(plat::WindowOptions{});
    main.resizeWindow(500, 400);
    main.showWindow();
    // open: type a name, Return
    App::get().singleShot(30, [&] {
        Window* top = App::get().modalWindow();
        if (!top) return;
        savePicture(*top, "filedialog");
        type(*top, "b9.wav");
        key(*top, plat::Key_Return);
    });
    std::string got = files::openFile(&main, "Add audio", abs, "Audio (*.wav *.flac);;All files (*)");
    CHECK(got == abs + "/b9.wav");
    // remembered folder for that title
    CHECK(settings::get("file_dialog/Add audio").toString() == abs);
    // save adds the extension
    App::get().singleShot(30, [&] {
        Window* top = App::get().modalWindow();
        if (!top) return;
        key(*top, 'A', plat::ModCtrl);
        type(*top, "song");
        key(*top, plat::Key_Return);
    });
    got = files::saveFile(&main, "Save project", abs + "/Untitled.json", "Tiny DAW project (*.json)");
    CHECK(got == abs + "/song.json");
    main.hideWindow();
}

// the deepest-first walk of a widget's children: the first that `match` takes
static Widget* findWidget(Widget* in, const std::function<bool(Widget*)>& match) {
    for (auto& c : in->children()) {
        if (match(c.get())) return c.get();
        if (Widget* deeper = findWidget(c.get(), match)) return deeper;
    }
    return nullptr;
}

// The file dialog's and a message box's sign's look are the stylesheets': a sheet over default.css changes the
// dialog's size, least size, room, places list and Up / Home buttons, and the sign's size (its drawing scaled);
// theme::setSkin() gives back default.css's
static void testFileDialogAndMessageIconStyles() {
    std::string dir = "build/toolkit-samples/files-styled";
    util::makeDirs(dir);
    std::string abs = util::absolutePath(dir);
    Window main(plat::WindowOptions{});
    main.resizeWindow(500, 400);
    main.showWindow();
    struct Seen {
        int w = 0, h = 0, minW = 0, minH = 0, margin = -1, spacing = -1, placesW = 0;
        Point nav, icon;
        Color iconEdge;
    };
    auto look = [&] {
        Seen seen;
        App::get().singleShot(30, [&] {
            Window* top = App::get().modalWindow();
            if (!top) return;
            seen.w = top->width(), seen.h = top->height();
            seen.minW = top->minimumSize().x, seen.minH = top->minimumSize().y;
            if (auto* l = dynamic_cast<BoxLayout*>(top->layout())) seen.margin = l->margins[0], seen.spacing = l->spacing;
            if (Widget* places = findWidget(top, [](Widget* w) { return dynamic_cast<ListWidget*>(w) != nullptr; }))
                seen.placesW = places->minimumSize().x;
            if (Widget* up = findWidget(top, [](Widget* w) { return w->toolTip() == "Up one folder (Backspace)"; }))
                seen.nav = up->minimumSize();
            key(*top, plat::Key_Escape);
        });
        CHECK(files::openFile(&main, "Styled files", abs, "All files (*)").empty());
        App::get().singleShot(30, [&] {
            Window* top = App::get().modalWindow();
            if (!top) return;
            if (Widget* sign = findWidget(top, [](Widget* w) { return w->isType("MessageIcon"); })) {
                seen.icon = Point(sign->width(), sign->height());
                App::get().tick();
                // (the circle's left edge, a little inside: its background whatever its size)
                Point at = sign->mapToWindow(Point(sign->width() / 8, sign->height() / 2));
                seen.iconEdge = top->framebuffer().colorAt(at.x, at.y);
            }
            key(*top, plat::Key_Escape);
        });
        msg::question(&main, "Styled", "A question?", msg::Yes | msg::No);
        return seen;
    };
    Seen before = look();
    CHECK(before.w == 760 && before.h == 480 && before.minW == 480 && before.minH == 320);
    CHECK(before.margin == 10 && before.spacing == 8 && before.placesW == 150 && before.nav == Point(26, 24));
    CHECK(before.icon == Point(32, 32) && before.iconEdge == theme::c("accent"));
    std::vector<css::Problem> problems;
    theme::setStyles({theme::builtinSheet(),
                      css::parse("FileDialog { width: 640px; height: 420px; min-width: 400px; min-height: 300px;"
                                 " padding: 16px; row-gap: 12px; }\n"
                                 "FileDialog::places { width: 200px; }\n"
                                 "FileDialog::nav-button { width: 30px; height: 28px; }\n"
                                 "MessageIcon { width: 48px; height: 48px; }\n"
                                 "MessageIcon.question { background-color: #20c040; }\n",
                                 "files.css", problems)});
    CHECK(problems.empty());
    Seen styled = look();
    CHECK(styled.w == 640 && styled.h == 420 && styled.minW == 400 && styled.minH == 300);
    CHECK(styled.margin == 16 && styled.spacing == 12 && styled.placesW == 200 && styled.nav == Point(30, 28));
    CHECK(styled.icon == Point(48, 48) && styled.iconEdge == Color::fromHex("#20c040"));
    theme::setSkin();
    Seen after = look();
    CHECK(after.w == before.w && after.h == before.h && after.minW == before.minW && after.minH == before.minH);
    CHECK(after.margin == before.margin && after.spacing == before.spacing && after.placesW == before.placesW);
    CHECK(after.nav == before.nav && after.icon == before.icon && after.iconEdge == before.iconEdge);
    main.hideWindow();
}

static void testFocusAndToolTips() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(300, 100);
    w.showWindow();
    auto* a = w.add<LineEdit>("one");
    a->setGeometry(10, 10, 100, 22);
    auto* b = w.add<Button>("b");
    b->setGeometry(120, 10, 60, 26);
    auto* n = w.add<Button>("nofocus");
    n->setFocusPolicy(FocusPolicy::None);
    n->setGeometry(190, 10, 60, 26);
    auto* c = w.add<LineEdit>("three");
    c->setGeometry(10, 50, 100, 22);
    a->setFocus();
    key(w, plat::Key_Tab);
    CHECK(b->hasFocus());
    key(w, plat::Key_Tab);
    CHECK(c->hasFocus());
    CHECK(c->selectedText() == "three");  // (Tab selects a field's text)
    key(w, plat::Key_Tab);
    CHECK(a->hasFocus());
    key(w, plat::Key_Tab, plat::ModShift);
    CHECK(c->hasFocus());
    // clicking a widget that takes no focus leaves the focus where it is
    click(w, 200, 20);
    CHECK(c->hasFocus());
    // disabling the focus widget drops the focus
    c->setEnabled(false);
    CHECK(w.focusWidget() == nullptr);
    // tooltips: after resting ~700 ms over a widget with one
    b->setToolTip("Line one\nLine two");
    moveTo(w, 130, 20);
    App::get().advanceTimers(0.3);
    CHECK(!App::get().toolTipWindow() || !App::get().toolTipWindow()->isWindowShown());
    App::get().advanceTimers(0.5);
    CHECK(App::get().toolTipWindow() && App::get().toolTipWindow()->isWindowShown());
    Rect tg = App::get().toolTipWindow()->screenGeometry();
    CHECK(tg.h > 2 * (int)theme::uiFont().lineSpacing());  // two lines
    // a press hides it
    press(w, 130, 20);
    CHECK(!App::get().toolTipWindow()->isWindowShown());
    release(w, 130, 20);
    // leaving hides it; toolTipAt can depend on the place
    struct Tipper : Widget {
        std::string toolTipAt(Point p) const override { return p.x < 25 ? "left" : "right"; }
    };
    auto* tp = w.add<Tipper>();
    tp->setGeometry(200, 50, 50, 30);
    moveTo(w, 205, 60);
    App::get().advanceTimers(1.0);
    CHECK(App::get().toolTipWindow()->isWindowShown());
    CHECK(static_cast<Widget*>(App::get().toolTipWindow())->children().empty());
    moveTo(w, 240, 60);
    CHECK(App::get().toolTipWindow()->isWindowShown());
    moveTo(w, 5, 95);
    CHECK(!App::get().toolTipWindow()->isWindowShown());
    // show() / hide() on a window map it
    Window w2(plat::WindowOptions{});
    w2.show();
    CHECK(w2.isWindowShown());
    w2.hide();
    CHECK(!w2.isWindowShown());
    w.hideWindow();
}

// a gallery of the controls, for looking at
static void gallery() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(760, 560);
    w.moveWindow(0, 0);
    auto* root = vbox(&w, 12, 10);
    auto* bar = w.add<MenuBar>();
    bar->addMenu("File")->addAction("Open…", nullptr, "Ctrl+O");
    bar->addMenu("Edit");
    bar->addMenu("View");
    root->addWidget(bar);
    // buttons
    auto row1 = std::make_unique<HBox>();
    HBox* r1 = row1.get();
    r1->spacing = 8;
    root->addLayout(std::move(row1));
    auto* plain = w.add<Button>("Plain");
    auto* hov = w.add<Button>("Hover");
    hov->hovered_ = true;
    auto* def = w.add<Button>("Default");
    def->setDefault(true);
    auto* dis = w.add<Button>("Disabled");
    dis->setEnabled(false);
    auto* focusB = w.add<Button>("Focused");
    auto* pmenu = w.add<Button>("Menu");
    Menu dummy;
    dummy.addAction("x");
    pmenu->setMenu(&dummy);
    for (Button* b : {plain, hov, def, dis, focusB, pmenu}) r1->addWidget(b);
    r1->addStretch();
    auto row2 = std::make_unique<HBox>();
    HBox* r2 = row2.get();
    r2->spacing = 6;
    root->addLayout(std::move(row2));
    auto mk = [&](const std::string& t, const char* prop, const char* val, bool checked) {
        Button* b = toolButton(&w, t);
        b->setCheckable(true);
        if (prop) b->setProperty(prop, val);
        b->setCheckedSilently(checked);
        r2->addWidget(b);
        return b;
    };
    mk("FX", "lit", "true", false)->setFixedSize(28, 22);
    mk("M", "toggle", "mute", true)->setFixedSize(26, 22);
    mk("S", "toggle", "solo", true)->setFixedSize(26, 22);
    mk("● Rec", "toggle", "record", true);
    mk("Tool", nullptr, nullptr, false);
    mk("Checked", nullptr, nullptr, true);
    mk("Raised", "autoRaise", "true", false);
    mk("Raised on", "autoRaise", "true", true);
    auto* split = toolButton(&w, "🎹");
    split->setMenu(&dummy, true);
    split->setFixedSize(40, 22);
    r2->addWidget(split);
    r2->addStretch();
    // fields
    auto row3 = std::make_unique<HBox>();
    HBox* r3 = row3.get();
    r3->spacing = 8;
    root->addLayout(std::move(row3));
    auto* le = w.add<LineEdit>();
    le->setPlaceholder("Search…");
    auto* le2 = w.add<LineEdit>("Some text selected");
    auto* sp = w.add<SpinBox>();
    sp->setRange(0, 999);
    sp->setValue(120);
    sp->setSuffix(" BPM");
    auto* cb = w.add<ComboBox>();
    cb->addItem("44100 Hz");
    cb->addItem("48000 Hz");
    r3->addWidget(le);
    r3->addWidget(le2);
    r3->addWidget(sp);
    r3->addWidget(cb);
    // checks and sliders
    auto row4 = std::make_unique<HBox>();
    HBox* r4 = row4.get();
    r4->spacing = 12;
    root->addLayout(std::move(row4));
    auto* c1 = w.add<CheckBox>("Metronome");
    c1->setChecked(true);
    auto* c2 = w.add<CheckBox>("Count in");
    auto* c3 = w.add<CheckBox>("Disabled");
    c3->setEnabled(false);
    c3->setCheckedSilently(true);
    auto* sl = w.add<Slider>();
    sl->setRange(0, 100);
    sl->setValue(65);
    auto* sl2 = w.add<Slider>();
    sl2->setRange(0, 100);
    sl2->setValue(30);
    sl2->setFixedHeight(14);
    for (Widget* x : std::initializer_list<Widget*>{c1, c2, c3, sl, sl2}) r4->addWidget(x);
    // labels
    auto row5 = std::make_unique<HBox>();
    HBox* r5 = row5.get();
    r5->spacing = 14;
    root->addLayout(std::move(row5));
    auto* l1 = w.add<Label>("A label");
    auto* l2 = w.add<Label>("hint text");
    l2->setProperty("hint", "true");
    auto* l3 = w.add<Label>("dimmed 8 pt");
    l3->setProperty("dimmed", "true");
    auto* l4 = w.add<Label>("001:02:480");
    l4->objectName = "time";
    auto* l5 = w.add<Label>("-14.2 LUFS");
    l5->objectName = "lufs";
    auto* l6 = w.add<Label>("a link");
    l6->setLink("https://example.org");
    for (Widget* x : std::initializer_list<Widget*>{l1, l2, l3, l4, l5, l6}) r5->addWidget(x);
    r5->addStretch();
    // group box, tabs, list, tree
    auto row6 = std::make_unique<HBox>();
    HBox* r6 = row6.get();
    r6->spacing = 10;
    root->addLayout(std::move(row6), 1);
    auto* gb = w.add<GroupBox>("Sounds");
    auto* gl = vbox(gb, 0, 6);
    gl->addWidget(gb->add<CheckBox>("Accent first beat"));
    gl->addWidget(gb->add<Label>("Volume"));
    auto* gsl = gb->add<Slider>();
    gsl->setRange(0, 10);
    gsl->setValue(7);
    gl->addWidget(gsl);
    gl->addStretch();
    auto* tabs = w.add<TabWidget>();
    auto* t1 = tabs->makeTab<Widget>("General");
    tabs->makeTab<Widget>("Audio");
    tabs->makeTab<Widget>("MIDI");
    auto* tl = vbox(t1, 8, 6);
    auto* lw = t1->add<ListWidget>();
    for (const char* s : {"Kick.wav", "Snare.wav", "Hat closed.wav", "Hat open.wav", "Clap.wav", "Ride.wav", "Crash.wav",
                          "Tom 1.wav", "Tom 2.wav"})
        lw->addItem(s);
    lw->setCurrentRow(1);
    tl->addWidget(lw);
    auto* tw = w.add<TreeWidget>();
    auto* n1 = tw->root()->add("Samples", "", true);
    n1->expanded = n1->filled = true;
    n1->add("Drums", "", true);
    n1->add("Bass.wav");
    tw->root()->add("Projects", "", true);
    tw->refreshRows();
    tw->setCurrent(n1->children[1].get());
    r6->addWidget(gb);
    r6->addWidget(tabs, 1);
    r6->addWidget(tw);
    auto* sb = w.add<StatusBar>();
    sb->showMessage("Ready — 48000 Hz, 256 samples");
    root->addWidget(sb);
    w.showWindow();
    App::get().doLayouts();
    w.setFocusWidget(le2);
    le2->setCursorPosition(10);
    le2->setCursorPosition(18, true);
    savePicture(w, "gallery");
    focusB->setFocus();
    savePicture(w, "gallery-focus");

    // a menu, the combo list and a tooltip
    Menu m;
    m.addAction("New project", nullptr, "Ctrl+N");
    m.addAction("Open…", nullptr, "Ctrl+O");
    m.addSeparator();
    m.addSection("Recently tweaked");
    m.addCheckable("Snap to grid", true, nullptr, "Ctrl+G");
    m.addMenu("Export")->addAction("WAV");
    Action* da = m.addAction("Disabled item");
    da->enabled = false;
    m.addAction("Quit", nullptr, "Ctrl+Q");
    m.popup(Point(40, 40), &w);
    Window* mp = App::get().popups().back();
    mp->onEvent(nullptr, mouseEv(plat::Event::MouseMove, 40, 40, 0, 0));
    savePicture(*mp, "menu");
    m.close();
    cb->showPopup();
    Window* cp = App::get().popups().back();
    savePicture(*cp, "combo-list");
    App::get().closeAllPopups();
    App::get().showToolTip(Point(100, 100), "Volume -6.0 dB\n(double-click: 0 dB)", &w);
    savePicture(*App::get().toolTipWindow(), "tooltip");
    App::get().hideToolTip();
    // a message box and a progress window
    App::get().singleShot(30, [&] {
        Window* top = App::get().modalWindow();
        if (!top) return;
        savePicture(*top, "msgbox");
        key(*top, plat::Key_Escape);
    });
    msg::question(&w, "Unsaved changes", "Save the changes to the current project?", msg::Save | msg::Discard | msg::Cancel);
    App::get().singleShot(30, [&] {
        Window* top = App::get().modalWindow();
        if (!top) return;
        savePicture(*top, "msgbox-warning");
        key(*top, plat::Key_Escape);
    });
    msg::warning(&w, "Cannot load plugin", "Some Plugin\n/usr/lib/vst3/Some.vst3\n\nThe plugin didn't load: no factory.");
    ProgressDialog pd(&w, "Rendering", "Rendering Track 1…", true);
    pd.setRange(0, 100);
    pd.setValue(42);
    savePicture(pd, "progress");
    pd.cancel();
    w.hideWindow();
}

// Stylesheets style the controls: a skin's rules over default.css change sizes, colours and corners; widgets
// are matched by type (and the types they count as), #objectName, classes from properties, and states
static void testStylesheets() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(300, 60);
    auto* push = w.add<Button>("Push me");
    push->objectName = "big";
    auto* m = w.add<Button>("M");
    m->setStyle(Button::Style::Tool);
    m->setCheckable(true);
    m->setProperty("toggle", "mute");
    m->setChecked(true);
    auto* lit = w.add<Button>("Lit");
    lit->setStyle(Button::Style::Tool);
    lit->setProperty("lit", "true");
    // what the stylesheets see
    CHECK(push->type() == "PushButton" && push->isType("Button") && push->isType("Widget") && !push->isType("ToolButton"));
    CHECK(m->type() == "ToolButton" && !m->isType("PushButton"));
    CHECK(m->hasClass("toggle") && m->hasClass("toggle-mute") && !m->hasClass("toggle-solo") && !m->hasClass("lit"));
    CHECK(lit->hasClass("lit") && m->hasState("checked") && m->hasState("toggled") && m->hasState("lit"));
    CHECK(push->id() == "big" && push->parent() == &w && !push->hasState("checked"));
    push->setProperty("lit", "false");  // ("false": no class)
    CHECK(!push->hasClass("lit"));
    push->addStyleClass("extra");
    CHECK(push->hasClass("extra"));
    push->removeStyleClass("extra");
    CHECK(!push->hasClass("extra"));

    Point before = push->sizeHint();
    std::vector<css::Problem> problems;
    css::SheetPtr extra = css::parse(
        ":root { --button: #203040; }\n"
        "PushButton { padding: 6px 20px; }\n"
        "#big { border-radius: 0; }\n"
        ".toggle-mute { --toggle-color: #00ff00; }\n"
        "Button.lit { background-color: rgb(10 20 30); color: white; }\n",
        "test.css", problems);
    CHECK(problems.empty());
    theme::setStyles({theme::builtinSheet(), extra});
    Point after = push->sizeHint();
    CHECK(after.x == before.x + 16 && after.y == before.y + 4);
    push->setGeometry(10, 10, after.x, after.y);
    m->setGeometry(140, 10, 26, 22);
    lit->setGeometry(180, 10, 40, 22);
    w.showWindow();
    App::get().tick();
    const Image& fb = w.framebuffer();
    CHECK(fb.colorAt(10, 10) == Color::fromHex("#203040"));  // (square: the corner pixel is the fill)
    CHECK(fb.colorAt(145, 14) == Color::fromHex("#00ff00"));
    CHECK(fb.colorAt(183, 13) == Color(10, 20, 30));
    CHECK(lit->textColor() == Color(255, 255, 255));
    // font-family falls back along its list to a family this computer has; generic families always count;
    // none there: the first name (the font code finds a look-alike)
    {
        std::vector<std::string> have = Font::families();
        CHECK(!have.empty());
        if (!have.empty()) {
            CHECK(theme::pickFamily({"No Such Font", have[0], "sans-serif"}) == have[0]);
            CHECK(theme::pickFamily({"No Such Font", "sans-serif", have[0]}).empty());
        }
        CHECK(theme::pickFamily({"No Such Font", "monospace"}) == "monospace");
        CHECK(theme::pickFamily({"Consolas Not Here"}) == "Consolas Not Here");
        CHECK(theme::pickFamily({}).empty() && theme::pickFamily({"system-ui"}).empty());
    }
    // sizes of the layout: :root's variables, as lengths
    {
        std::vector<css::Problem> pr;
        theme::setStyles({theme::builtinSheet(), css::parse(":root { --test-size: 30px; --test-pt: 9pt; --test-calc: calc(10px * 2 + 1em); --test-bad: red; }", "m.css", pr)});
        CHECK(theme::metric("test-size", 5) == 30 && theme::metric("--test-size", 5) == 30);
        CHECK(theme::metric("test-pt", 5) == 12 && theme::metric("test-calc", 5) == 32);
        CHECK(theme::metric("test-bad", 5) == 5 && theme::metric("no-such-size", 7) == 7);
        CHECK(std::fabs(theme::metricF("test-pt", 0) - 12.0) < 1e-9);
    }
    // a widget's place among its siblings (:first-child, :nth-child(), + and ~): its cached style follows it
    {
        std::vector<css::Problem> pr;
        theme::setStyles({theme::builtinSheet(), css::parse(
            "Widget.box > Widget:first-child { --pos: first; }\n"
            "Widget.box > Widget:nth-child(2n) { --even: yes; }\n"
            "Widget.box > Widget:last-child { --last: yes; }\n"
            ".marked + Widget { --after: marked; }\n"
            ".marked ~ Widget { --later: yes; }\n"
            ":root { --test-rem: 2rem; --test-vw: 1vw; }\n", "s.css", pr)});
        CHECK(pr.empty());
        CHECK(theme::styles().uses() == (css::Engine::UsesPosition | css::Engine::UsesSiblings));
        Widget box;
        box.addStyleClass("box");
        Widget* a = box.add<Widget>();
        Widget* b = box.add<Widget>();
        Widget* c = box.add<Widget>();
        auto var = [](Widget* x, const char* name) {
            std::optional<std::string> v = x->computedStyle().variable(name);
            return v ? *v : std::string();
        };
        CHECK(a->childIndex() == 0 && b->childIndex() == 1 && c->childIndex() == 2 && a->siblingCount() == 3);
        CHECK(box.childIndex() == -1 && box.siblingCount() == 0 && a->sibling(2) == c && !a->sibling(3) && !a->sibling(-1));
        CHECK(var(a, "--pos") == "first" && var(b, "--pos").empty());
        CHECK(var(b, "--even") == "yes" && var(c, "--last") == "yes" && var(b, "--last").empty());
        a->raise();  // (b c a)
        CHECK(a->childIndex() == 2 && var(a, "--last") == "yes" && var(a, "--pos").empty() && var(b, "--pos") == "first");
        CHECK(var(c, "--even") == "yes" && var(b, "--even").empty() && var(c, "--last").empty());
        CHECK(var(c, "--after").empty());
        b->addStyleClass("marked");  // (a sibling before them gets a class)
        CHECK(var(c, "--after") == "marked" && var(a, "--after").empty() && var(a, "--later") == "yes");
        b->removeStyleClass("marked");
        CHECK(var(c, "--after").empty() && var(a, "--later").empty());
        Widget* d = box.add<Widget>();  // (one more: the count changes)
        CHECK(var(d, "--last") == "yes" && var(a, "--last").empty() && var(d, "--even") == "yes");
        a->lower();  // (a b c d)
        CHECK(var(a, "--pos") == "first" && var(b, "--even") == "yes" && var(d, "--even") == "yes" && var(c, "--even").empty());
        // rem (of :root's font size) and vw (of the media) in variables, read as lengths
        CHECK(std::fabs(theme::metricF("test-rem", 0) - 2 * theme::styles().root().emPx_) < 1e-9);
        CHECK(std::fabs(theme::metricF("test-vw", 0) - theme::styles().media().width / 100) < 1e-9);
    }
    // back to the default: the button's colour and size are default.css's again (its style isn't kept)
    theme::setSkin();
    CHECK(push->sizeHint().x == before.x && push->sizeHint().y == before.y);
    CHECK(push->fillColor() == theme::c("button"));
    w.hideWindow();
}

// the fields take their look from the stylesheets: LineEdit (padding, ::selection, ::clear-button), SpinBox
// (:up-hover::up-button, the buttons' width, :focus from its editor), ComboBox (::arrow, :open) and its list
// (ComboPopup::item:selected / :disabled, ::separator)
static void testFieldStylesheets() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(400, 200);
    w.moveWindow(100, 100);
    auto* le = w.add<LineEdit>("Hello world");
    le->setClearButton(true);
    auto* sp = w.add<SpinBox>();
    sp->setRange(0, 10);
    sp->setValue(5);
    auto* cb = w.add<ComboBox>();
    cb->addItem("One");
    cb->addItem("Two");
    cb->insertSeparator();
    cb->addItem("Three");
    cb->setItemEnabled(3, false);
    // what the stylesheets see
    CHECK(le->type() == "LineEdit" && sp->type() == "SpinBox" && cb->type() == "ComboBox");
    CHECK(sp->editor()->hasState("frameless") && !le->hasState("frameless"));
    CHECK(!sp->hasState("at-maximum") && !sp->hasState("at-minimum"));
    sp->setValue(10);
    CHECK(sp->hasState("at-maximum"));
    sp->setValue(5);
    Point leBefore = le->sizeHint(), spBefore = sp->sizeHint(), cbBefore = cb->sizeHint();

    std::vector<css::Problem> problems;
    css::SheetPtr extra = css::parse(
        "LineEdit { padding: 3px 9px; background-color: #102030; }\n"
        "LineEdit::selection { background-color: #ff0000; }\n"
        "LineEdit::clear-button { width: 20px; }\n"
        "SpinBox { border-color: #00ff00; border-radius: 0; }\n"
        "SpinBox::up-button, SpinBox::down-button { width: 20px; }\n"
        "SpinBox:up-hover::up-button { background-color: #0000ff; border-radius: 0; }\n"
        "SpinBox:focus { border-color: #ffffff; }\n"
        "ComboBox::arrow { width: 30px; }\n"
        "ComboBox:open { border-color: #ff00ff; }\n"
        "ComboPopup { padding: 4px; }\n"
        "ComboPopup::item:selected { background-color: #00ffff; }\n"
        "ComboPopup::item:disabled { color: #ffff00; }\n"
        "ComboPopup::separator { background-color: #ff8000; stroke-width: 3px; }\n",
        "fields.css", problems);
    CHECK(problems.empty());
    theme::setStyles({theme::builtinSheet(), extra});
    // sizes: the padding (+8), the clear button (+4), the buttons' column (+4), the arrow's box (+12)
    CHECK(le->sizeHint().x == leBefore.x + 8 + 4 && le->sizeHint().y == leBefore.y);
    CHECK(sp->sizeHint().x == spBefore.x + 4 && cb->sizeHint().x == cbBefore.x + 12);
    le->setGeometry(10, 10, 200, 24);
    sp->setGeometry(10, 50, 80, 24);
    cb->setGeometry(10, 90, 150, 24);
    w.showWindow();
    App::get().doLayouts();
    // the editor sits inside the border, short of the wider buttons
    CHECK(sp->editor()->geometry().x == 1 && sp->editor()->geometry().w == 80 - 2 - 20);
    w.setFocusWidget(le);
    le->selectAll();
    App::get().tick();
    {
        const Image& fb = w.framebuffer();
        CHECK(fb.colorAt(10 + 5, 10 + 12) == Color::fromHex("#102030"));        // the field's own fill
        CHECK(fb.colorAt(10 + 9, 10 + 12) == Color::fromHex("#ff0000") ||
              fb.colorAt(10 + 10, 10 + 12) == Color::fromHex("#ff0000"));      // the selection, from the padding
        CHECK(fb.colorAt(10, 50 + 12) == Color::fromHex("#00ff00"));            // the spin box's own border
    }
    // the up button under the mouse: its hover box (over the inside of the border, clipped to the button)
    moveTo(w, 10 + 80 - 3, 50 + 4);
    CHECK(sp->hasState("up-hover") && !sp->hasState("down-hover"));
    App::get().tick();
    CHECK(w.framebuffer().colorAt(10 + 80 - 19, 50 + 2) == Color::fromHex("#0000ff"));
    CHECK(w.framebuffer().colorAt(10 + 80 - 19, 50 + 20) != Color::fromHex("#0000ff"));
    // the spin box's focus is its editor's
    sp->editor()->setFocus();
    CHECK(sp->hasState("focus"));
    App::get().tick();
    CHECK(w.framebuffer().colorAt(10, 50 + 12) == Color::fromHex("#ffffff"));
    // the combo's list: open, the highlighted row, the disabled one, the separator
    cb->showPopup();
    CHECK(cb->isPopupOpen() && cb->hasState("open"));
    App::get().tick();
    CHECK(w.framebuffer().colorAt(10, 90 + 12) == Color::fromHex("#ff00ff"));
    Window* pop = App::get().popups().back();
    CHECK(pop->type() == "ComboPopup" && !pop->hasState("selected"));
    pop->update();
    App::get().tick();
    {
        const Image& fb = pop->framebuffer();
        Font uf = theme::uiFont();
        int rowH = (int)std::ceil(uf.ascent() + uf.descent()) + 6;  // (its font's height and the item's 3px 6px)
        CHECK(fb.colorAt(1 + 4 + 2, 1 + 4 + rowH / 2) == Color::fromHex("#00ffff"));  // "One": the current one
        CHECK(fb.colorAt(1 + 4 + 2, 1 + 4 + rowH + rowH / 2) != Color::fromHex("#00ffff"));
        int sepY = 1 + 4 + 2 * rowH + (7 - 3) / 2;                                     // a 3 px line across 7 px
        CHECK(fb.colorAt(1 + 4 + 4, sepY) == Color::fromHex("#ff8000") && fb.colorAt(1 + 4 + 4, sepY + 2) == Color::fromHex("#ff8000"));
        CHECK(fb.colorAt(1 + 4 + 3, sepY) != Color::fromHex("#ff8000"));            // (its margin)
        bool yellow = false;  // "Three" (disabled) is drawn in its colour (anti-aliased: yellow-ish pixels)
        for (int y = 1 + 4 + 2 * rowH + 7; y < 1 + 4 + 3 * rowH + 7 && !yellow; ++y)
            for (int x = 1 + 4; x < 60 && !yellow; ++x) {
                Color c = fb.colorAt(x, y);
                yellow = c.r > 150 && c.g > 150 && c.b < 90;
            }
        CHECK(yellow);
    }
    cb->hidePopup();
    CHECK(!cb->hasState("open"));

    // back to the default: sizes as before
    theme::setSkin();
    CHECK(le->sizeHint().x == leBefore.x && sp->sizeHint().x == spBefore.x && cb->sizeHint().x == cbBefore.x);
    w.hideWindow();
}

// the basic controls (Label, CheckBox, GroupBox, Separator, SplitterHandle, StatusBar, TabWidget, ButtonBox), windows
// and tooltips take their sizes and colours from the stylesheets: default.css's are today's, a sheet over it changes them
static void testBasicControlStyles() {
    theme::setSkin();
    Window w(plat::WindowOptions{});
    w.resizeWindow(400, 300);
    auto* lab = w.add<Label>("Hint");
    lab->setProperty("hint", "true");
    auto* link = w.add<Label>("A link");
    link->setLink("https://example.org");
    auto* cb = w.add<CheckBox>("Check");
    cb->setChecked(true);
    auto* gb = w.add<GroupBox>("Group");
    gb->setGeometry(200, 10, 150, 80);
    auto* sb = w.add<StatusBar>();
    auto* tabs = w.add<TabWidget>();
    tabs->addTab(std::make_unique<Widget>(), "One");
    tabs->addTab(std::make_unique<Widget>(), "Two");
    auto* sep = w.add<Separator>(Orientation::Vertical);
    auto* split = w.add<SplitterHandle>(Orientation::Horizontal);
    Canvas canvas;
    ButtonBox bb(ButtonBox::Ok | ButtonBox::Cancel);
    // what the stylesheets see
    CHECK(w.type() == "Window" && canvas.type() == "Canvas" && lab->type() == "Label" && cb->type() == "CheckBox");
    CHECK(gb->type() == "GroupBox" && sb->type() == "StatusBar" && tabs->type() == "TabWidget" && bb.type() == "ButtonBox");
    CHECK(sep->type() == "Separator" && sep->hasState("vertical") && !sep->hasState("horizontal"));
    CHECK(split->type() == "SplitterHandle" && split->hasState("horizontal"));
    CHECK(lab->hasClass("hint") && link->hasState("link") && !lab->hasState("link") && cb->hasState("checked"));
    CHECK(!tabs->hasState("selected"));  // (a tab's states only while that tab is styled)
    // default.css: today's sizes and colours
    int th = (int)std::ceil(Font::ui().ascent() + Font::ui().descent());
    CHECK(lab->color() == theme::c("hint_text") && link->color() == theme::c("accent"));
    Point cbBefore = cb->sizeHint();
    CHECK(cbBefore.y == std::max(th, 14) + 4);
    CHECK(gb->contentsRect().x == 6 && gb->contentsRect().y == 28 && gb->contentsRect().w == 138 && gb->contentsRect().h == 46);
    CHECK(sep->sizeHint().x == 7 && sep->sizeHint().y == 13 && split->sizeHint().x == 5);
    CHECK(tabs->barHeight() == th + 14 && sb->sizeHint().y == th + 9);
    Point bbBefore = bb.sizeHint();
    App::get().showToolTip(Point(100, 100), "Tip", lab);
    int tipBefore = App::get().toolTipWindow()->width();
    App::get().hideToolTip();

    std::vector<css::Problem> problems;
    css::SheetPtr extra = css::parse(
        "Window { background-color: #123456; }\n"
        "Label.hint { color: #102030; }\n"
        "CheckBox { padding: 5px 0; }\n"
        "CheckBox:checked::indicator { background-color: #00ff00; border-color: #00ff00; }\n"
        "GroupBox { margin-top: 30px; padding: 10px; }\n"
        "Separator:vertical { margin: 2px 1px; line-width: 3px; }\n"
        "SplitterHandle:horizontal { width: 9px; }\n"
        "TabWidget::tab { padding: 10px 20px 12px; }\n"
        "TabWidget::tab-line:selected { background-color: #ff0000; height: 4px; }\n"
        "StatusBar::separator { height: 3px; background-color: #0000ff; }\n"
        "ToolTip { padding: 10px; }\n"
        "ButtonBox { column-gap: 20px; }\n",
        "basic.css", problems);
    CHECK(problems.empty());
    theme::setStyles({theme::builtinSheet(), extra});
    CHECK(lab->color() == Color::fromHex("#102030") && link->color() == theme::c("accent"));
    CHECK(cb->sizeHint().y == cbBefore.y + 6 && cb->sizeHint().x == cbBefore.x);
    CHECK(gb->contentsRect().x == 10 && gb->contentsRect().y == 40 && gb->contentsRect().w == 130 && gb->contentsRect().h == 30);
    CHECK(sep->sizeHint().x == 5 && sep->sizeHint().y == 5 && split->sizeHint().x == 9);
    CHECK(tabs->barHeight() == th + 22 && sb->sizeHint().y == th + 11);
    CHECK(bb.sizeHint().x == bbBefore.x + 14);
    App::get().showToolTip(Point(100, 100), "Tip", lab);
    CHECK(App::get().toolTipWindow()->width() == tipBefore + 12);
    App::get().hideToolTip();
    // drawn so
    cb->setGeometry(10, 10, 100, cb->sizeHint().y);
    tabs->setGeometry(10, 100, 180, 80);
    sb->setGeometry(0, 270, 400, sb->sizeHint().y);
    lab->setGeometry(10, 200, 60, 20);
    link->setGeometry(80, 200, 60, 20);
    sep->setGeometry(150, 200, 5, 20);
    split->setGeometry(160, 200, 9, 20);
    w.showWindow();
    App::get().tick();
    const Image& fb = w.framebuffer();
    int indY = 10 + (cb->height() - 14) / 2;
    CHECK(fb.colorAt(10, indY + 7) == Color::fromHex("#00ff00"));                    // the box's edge (checked)
    CHECK(fb.colorAt(15, 100 + tabs->barHeight() - 2) == Color::fromHex("#ff0000"));  // the selected tab's line
    CHECK(fb.colorAt(380, 150) == Color::fromHex("#123456"));                         // the window's background
    CHECK(fb.colorAt(50, 271) == Color::fromHex("#0000ff"));                          // the status bar's line
    CHECK(fb.colorAt(150 + 1, 210) == theme::c("line"));                              // the 3 px separator line
    CHECK(fb.colorAt(160 + 4, 210) == theme::c("line"));                              // the handle's line
    // back to the default
    theme::setSkin();
    CHECK(lab->color() == theme::c("hint_text") && cb->sizeHint().y == cbBefore.y && tabs->barHeight() == th + 14);
    CHECK(gb->contentsRect().y == 28 && sep->sizeHint().x == 7 && bb.sizeHint().x == bbBefore.x);
    w.update();
    App::get().tick();
    CHECK(fb.colorAt(380, 150) == theme::c("window"));
    w.hideWindow();
    App::get().advanceTimers(1.5);  // (a tooltip hidden just now would make the next one quick)
}

// a skin change re-places what controls place themselves, even when their own size stays: a SpinBox whose
// buttons get wider keeps its size (fixed) but its editor narrows at once
static void testRestyleRelayout() {
    theme::setSkin();
    Window w(plat::WindowOptions{});
    w.resizeWindow(200, 60);
    auto* sp = w.add<SpinBox>();
    sp->setGeometry(10, 10, 120, 26);
    w.showWindow();
    App::get().tick();
    int before = sp->editor()->width();
    std::vector<css::Problem> problems;
    theme::setStyles({theme::builtinSheet(),
                      css::parse("SpinBox::up-button, SpinBox::down-button { width: 40px; }", "wide.css", problems)});
    App::get().repaintAll();
    App::get().tick();
    CHECK(sp->width() == 120 && sp->editor()->width() < before - 15);
    theme::setSkin();
    App::get().repaintAll();
    App::get().tick();
    CHECK(sp->editor()->width() == before);
    w.hideWindow();
}

// lists, scroll bars, sliders, menus and the menu bar take their sizes and colours (and their parts', in the
// parts' own states) from the stylesheets
static void testListAndMenuStylesheets() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(320, 200);
    auto* list = w.add<ListWidget>();
    for (const char* t : {"one", "two", "three", "four"}) list->addItem(t);
    list->setAlternatingColors(true);
    list->setGeometry(0, 0, 120, 150);
    list->setCurrentRow(1);
    auto* bar = w.add<ScrollBar>(Orientation::Vertical);
    bar->setRange(0, 100);
    bar->setPageStep(50);
    auto* slider = w.add<Slider>();
    slider->setRange(0, 100);
    slider->setValue(50);
    auto* mb = w.add<MenuBar>();
    mb->addMenu("File");
    // the defaults: default.css's
    CHECK(list->type() == "ListWidget" && bar->hasState("vertical") && slider->hasState("horizontal"));
    CHECK(bar->sizeHint().x == 10 && slider->sizeHint() == Point(84, 16) && list->sizeHint() == Point(256, 192));
    Point barBefore = mb->sizeHint();
    w.showWindow();
    App::get().tick();
    CHECK(list->rowHeight == 22);
    CHECK(w.framebuffer().colorAt(60, 33) == theme::c("highlight"));       // row 1: selected
    CHECK(w.framebuffer().colorAt(60, 77) == theme::c("alternate_base"));  // row 3: every other

    std::vector<css::Problem> problems;
    css::SheetPtr extra = css::parse(
        ":root { --scrollbar-width: 14px; }\n"
        "ListWidget::item { height: 30px; }\n"
        "ListWidget::item:alternate { background-color: #0000ff; }\n"
        "ListWidget::item:hover { background-color: #00ffff; }\n"
        "ListWidget::item:selected { background-color: #ff0000; }\n"
        "ScrollBar::thumb { background-color: #00ff00; margin: 0; border-radius: 0; }\n"
        "ScrollBar::thumb:hover { background-color: #008000; }\n"
        "Slider { height: 24px; }\n"
        "Slider::handle { width: 16px; height: 16px; background-color: #ffff00; border-radius: 0; }\n"
        "MenuBar::item { padding: 10px 8px; }\n"
        "Menu { border-width: 0; }\n"
        "Menu::item { padding: 8px 24px; }\n"
        "Menu::item:hover { background-color: #ff00ff; }\n",
        "lists.css", problems);
    CHECK(problems.empty());
    theme::setStyles({theme::builtinSheet(), extra});
    App::get().repaintAll();  // (as a skin change does)
    // sizes
    CHECK(bar->sizeHint().x == 14 && slider->sizeHint() == Point(84, 24));
    CHECK(mb->sizeHint().y == barBefore.y + 12);
    bar->setGeometry(200, 0, 14, 100);
    slider->setGeometry(220, 120, 60, 24);
    mb->setGeometry(0, 160, 100, mb->sizeHint().y);
    App::get().tick();  // (the list reads its rows' height again when painted)
    CHECK(list->rowHeight == 30);
    const Image& fb = w.framebuffer();
    CHECK(fb.colorAt(60, 45) == Color(255, 0, 0));   // row 1 (30..60): selected (and every other: selected wins)
    CHECK(fb.colorAt(60, 105) == Color(0, 0, 255));  // row 3 (90..120): every other
    CHECK(fb.colorAt(60, 75) == theme::c("base"));   // row 2: plain
    CHECK(fb.colorAt(203, 10) == Color(0, 255, 0));  // the thumb: no margin, green
    CHECK(fb.colorAt(220 + 30, 120 + 12) == Color(255, 255, 0));  // the handle, 16 x 16 in the middle
    CHECK(fb.colorAt(220 + 30 - 7, 120 + 5) == Color(255, 255, 0) && fb.colorAt(220 + 30 - 9, 120 + 5) != Color(255, 255, 0));
    // the parts' own :hover: the row and the thumb under the mouse (not the whole list, or bar)
    w.onEvent(nullptr, mouseEv(plat::Event::MouseMove, 60, 75, 0, 0));
    App::get().tick();
    CHECK(fb.colorAt(60, 75) == Color(0, 255, 255) && fb.colorAt(60, 105) == Color(0, 0, 255));
    w.onEvent(nullptr, mouseEv(plat::Event::MouseMove, 205, 10, 0, 0));
    App::get().tick();
    CHECK(fb.colorAt(203, 10) == Color(0, 128, 0));
    w.onEvent(nullptr, mouseEv(plat::Event::MouseMove, 205, 90, 0, 0));  // (on the bar, below the thumb)
    App::get().tick();
    CHECK(fb.colorAt(203, 10) == Color(0, 255, 0));
    // a menu: its items 8 px above and below, no edge; the item under the mouse magenta
    Menu menu;
    menu.addAction("First");
    menu.addAction("Second");
    menu.popup(Point(100, 100), &w);
    CHECK(menu.isOpen() && !App::get().popups().empty());
    Window* mp = App::get().popups().back();
    int lineH = (barBefore.y - 8);  // (the font's line: the bar was it and 4 px above and below)
    CHECK(mp->height() == 4 + 2 * (lineH + 16) + 4);
    mp->onEvent(nullptr, mouseEv(plat::Event::MouseMove, 40, 4 + lineH + 16 + 5, 0, 0));
    App::get().tick();
    CHECK(mp->framebuffer().colorAt(40, 4 + lineH + 16 + 5) == Color(255, 0, 255));
    CHECK(mp->framebuffer().colorAt(40, 4 + 5) == theme::c("base"));
    CHECK(mp->framebuffer().colorAt(0, 0) == theme::c("base"));  // (no edge)
    menu.close();
    // back to the default skin: default.css's sizes again
    theme::setSkin();
    App::get().repaintAll();
    App::get().tick();
    CHECK(list->rowHeight == 22 && bar->sizeHint().x == 10 && slider->sizeHint() == Point(84, 16));
    CHECK(mb->sizeHint() == barBefore);
    w.hideWindow();
}

// `A:hover + B`: a sibling's state changing restyles (and repaints) the ones after it
static void testSiblingStates() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(200, 40);
    auto* a = w.add<Button>("A");
    auto* b = w.add<Button>("B");
    a->setGeometry(5, 5, 60, 26);
    b->setGeometry(80, 5, 60, 26);
    std::vector<css::Problem> problems;
    theme::setStyles({theme::builtinSheet(),
                      css::parse("PushButton:hover + PushButton { background-color: #00ff00; }", "sib.css", problems)});
    CHECK(problems.empty());
    w.showWindow();
    App::get().tick();
    CHECK(b->fillColor() != Color(0, 255, 0));
    moveTo(w, 20, 15);  // over A
    CHECK(a->underMouse());
    App::get().tick();
    CHECK(b->fillColor() == Color(0, 255, 0));
    CHECK(w.framebuffer().colorAt(100, 18) == Color(0, 255, 0));  // (B was repainted)
    moveTo(w, 190, 35);  // off both
    App::get().tick();
    CHECK(b->fillColor() != Color(0, 255, 0));
    CHECK(w.framebuffer().colorAt(100, 18) != Color(0, 255, 0));
    theme::setSkin();
    w.hideWindow();
}

// ------------------------------------------------------------------------------------------
// CSS painting (style::box beyond plain boxes): every border style, per-side colours and widths, four radii,
// linear / radial / repeating gradients, outer / inset / blurred shadows, text-shadow on a Label, opacity on a
// widget with a child. Pictures: build/toolkit-samples/css-*.png; timings printed.
namespace {

struct BoxSheet {
    css::Engine engine;
    std::vector<css::Problem> problems;
    std::vector<std::unique_ptr<css::SimpleNode>> nodes;
    std::vector<std::unique_ptr<css::Style>> styles;
    explicit BoxSheet(const std::string& text) { engine.setSheets({css::parse(text, "boxes.css", problems)}); }
    const css::Style& style(const std::string& cls) {
        auto n = std::make_unique<css::SimpleNode>("Box");
        n->classes.push_back(cls);
        styles.push_back(std::make_unique<css::Style>(engine.compute(*n)));
        nodes.push_back(std::move(n));
        return *styles.back();
    }
};

int grey(const Image& img, int x, int y) { return img.colorAt(x, y).r; }
bool rgbIs(const Image& img, int x, int y, int r, int g, int b, int tol = 0) {
    Color c = img.colorAt(x, y);
    return std::abs(c.r - r) <= tol && std::abs(c.g - g) <= tol && std::abs(c.b - b) <= tol;
}
int darkness(const Image& img, int x, int y) {
    Color c = img.colorAt(x, y);
    return 765 - c.r - c.g - c.b;
}

}  // namespace

static void testCssPainting() {
    const Color canvas(0xd4, 0xd0, 0xc8);
    // --- border styles, per-side colours and widths, four radii ---
    BoxSheet bs(
        ".outset2 { background-color: #c0c0c0; border: 2px outset #c0c0c0; }\n"
        ".inset2 { background-color: #ffffff; border: 2px inset #c0c0c0; }\n"
        ".outset1 { background-color: #c0c0c0; border: 1px outset #c0c0c0; }\n"
        ".inset1 { background-color: #c0c0c0; border: 1px inset #c0c0c0; }\n"
        ".groove { border: 2px groove #c0c0c0; }\n"
        ".ridge { border: 4px ridge #c0c0c0; background-color: #c0c0c0; }\n"
        ".double { border: 6px double #204080; background-color: #ffffff; }\n"
        ".dashed { border: 2px dashed #a00000; background-color: #ffffff; }\n"
        ".dotted { border: 2px dotted #006000; background-color: #ffffff; }\n"
        ".dotted4 { border: 4px dotted #006000; border-radius: 14px; background-color: #ffffff; }\n"
        ".dashedr { border: 3px dashed #303030; border-radius: 12px; background-color: #ffffe0; }\n"
        ".solid { border: 2px solid #000080; background-color: #ffffff; }\n"
        ".sides { border-style: solid; border-width: 1px 4px 8px 2px; border-color: red lime blue orange; background-color: white; }\n"
        ".radii { background-color: #3070c0; border: 3px solid #102040; border-radius: 0 10px 20px 30px; }\n"
        ".sidesr { border: 6px solid; border-color: red lime blue orange; border-radius: 16px; background-color: white; }\n"
        ".none { border: 3px none red; background-color: #eeeeee; }\n"
        ".insetr { border: 3px inset #6080c0; border-radius: 8px; background-color: #6080c0; }\n"
        ".hidden { border: 2px solid red; border-left-style: hidden; border-bottom-style: dotted; background-color: white; }\n");
    CHECK(bs.problems.empty());
    for (const css::Problem& p : bs.problems) printf("%s\n", p.text().c_str());
    const char* order[] = {"outset2", "inset2", "outset1", "inset1", "groove",  "ridge",  "double", "dashed", "dotted",
                           "dotted4", "dashedr", "solid", "sides",  "radii",  "sidesr", "none",   "insetr", "hidden"};
    const int cols = 6, bw = 100, bh = 60, gap = 16;
    Image img(cols * (bw + gap) + gap, 3 * (bh + gap) + gap, canvas);
    Painter p(img);
    auto at = [&](int i) { return Rect(gap + (i % cols) * (bw + gap), gap + (i / cols) * (bh + gap), bw, bh); };
    for (int i = 0; i < 18; i++) style::box(p, at(i), bs.style(order[i]));
    savePng(outDir() + "/css-borders.png", img);
    {
        Rect r = at(0);  // outset 2px on silver: the four-tone bevel (lighter, the colour | darker, darker still)
        CHECK(grey(img, r.x, r.y) == 255 && grey(img, r.x + 1, r.y + 1) == 192);
        CHECK(grey(img, r.right() - 2, r.bottom() - 2) == 108 && grey(img, r.right() - 1, r.bottom() - 1) == 24);
        CHECK(grey(img, r.right() - 1, r.y) == 24 && grey(img, r.x, r.bottom() - 1) == 24);  // (corners: to the dark sides)
        CHECK(grey(img, r.right() - 2, r.y + 1) == 108 && grey(img, r.x + 1, r.bottom() - 2) == 108);
        CHECK(grey(img, r.x + 50, r.y) == 255 && grey(img, r.x, r.y + 30) == 255 && grey(img, r.x + 50, r.y + 30) == 192);
        r = at(1);  // inset 2px: darker, darker still | the colour, lighter
        CHECK(grey(img, r.x, r.y) == 108 && grey(img, r.x + 1, r.y + 1) == 24);
        CHECK(grey(img, r.right() - 2, r.bottom() - 2) == 192 && grey(img, r.right() - 1, r.bottom() - 1) == 255);
        CHECK(grey(img, r.x + 50, r.y + 30) == 255);
        r = at(2);  // outset 1px: the classic two tones
        CHECK(grey(img, r.x + 50, r.y) == 255 && grey(img, r.x, r.y + 30) == 255);
        CHECK(grey(img, r.x + 50, r.bottom() - 1) == 108 && grey(img, r.right() - 1, r.y + 30) == 108);
        r = at(3);
        CHECK(grey(img, r.x + 50, r.y) == 108 && grey(img, r.x + 50, r.bottom() - 1) == 255);
        r = at(4);  // groove: the outer half dark at the top, light at the bottom
        CHECK(grey(img, r.x + 50, r.y) == 108 && grey(img, r.x + 50, r.y + 1) == 255);
        CHECK(grey(img, r.x + 50, r.bottom() - 1) == 255 && grey(img, r.x + 50, r.bottom() - 2) == 108);
        CHECK(rgbIs(img, r.x + 50, r.y + 30, canvas.r, canvas.g, canvas.b));  // (no background)
        r = at(5);  // ridge 4px: light then dark at the top
        CHECK(grey(img, r.x + 50, r.y) == 255 && grey(img, r.x + 50, r.y + 1) == 255 && grey(img, r.x + 50, r.y + 2) == 108);
        r = at(6);  // double 6px: thirds
        CHECK(rgbIs(img, r.x + 50, r.y + 1, 0x20, 0x40, 0x80) && rgbIs(img, r.x + 50, r.y + 2, 255, 255, 255) &&
              rgbIs(img, r.x + 50, r.y + 3, 255, 255, 255) && rgbIs(img, r.x + 50, r.y + 4, 0x20, 0x40, 0x80));
        r = at(7);  // dashed: a dash at the corner, gaps along the side
        CHECK(rgbIs(img, r.x, r.y, 0xa0, 0, 0) && rgbIs(img, r.right() - 1, r.y, 0xa0, 0, 0));
        int on = 0, off = 0;
        for (int x = r.x; x < r.right(); x++) (rgbIs(img, x, r.y, 0xa0, 0, 0) ? on : off)++;
        CHECK(on > 40 && off > 30 && on + off == 100);
        r = at(8);  // dotted 2px: square dots, as far apart as they are wide
        on = off = 0;
        for (int x = r.x; x < r.right(); x++) (rgbIs(img, x, r.y, 0, 0x60, 0) ? on : off)++;
        CHECK(on >= 48 && on <= 52 && on + off == 100 && rgbIs(img, r.x, r.y + 1, 0, 0x60, 0));
        r = at(12);  // per-side colours and widths
        CHECK(rgbIs(img, r.x + 50, r.y, 255, 0, 0) && rgbIs(img, r.x + 50, r.y + 1, 255, 255, 255));
        CHECK(rgbIs(img, r.right() - 1, r.y + 30, 0, 255, 0) && rgbIs(img, r.right() - 4, r.y + 30, 0, 255, 0) &&
              rgbIs(img, r.right() - 5, r.y + 30, 255, 255, 255));
        CHECK(rgbIs(img, r.x + 50, r.bottom() - 8, 0, 0, 255) && rgbIs(img, r.x + 50, r.bottom() - 9, 255, 255, 255));
        CHECK(rgbIs(img, r.x + 1, r.y + 30, 255, 165, 0) && rgbIs(img, r.x + 2, r.y + 30, 255, 255, 255));
        CHECK(rgbIs(img, r.x, r.bottom() - 1, 0, 0, 255) && rgbIs(img, r.right() - 1, r.bottom() - 1, 0, 0, 255));  // (corners)
        r = at(13);  // four radii: square top-left, rounder and rounder clockwise
        CHECK(rgbIs(img, r.x, r.y, 0x10, 0x20, 0x40));
        CHECK(rgbIs(img, r.right() - 1, r.y, canvas.r, canvas.g, canvas.b) &&
              rgbIs(img, r.right() - 3, r.bottom() - 3, canvas.r, canvas.g, canvas.b) &&
              rgbIs(img, r.x + 4, r.bottom() - 4, canvas.r, canvas.g, canvas.b));
        CHECK(rgbIs(img, r.x + 50, r.y + 30, 0x30, 0x70, 0xc0));
        int mixed = 0;  // (anti-aliased pixels along the big corner: neither the canvas, the border nor the inside)
        for (int y = r.bottom() - 30; y < r.bottom(); y++)
            for (int x = r.x; x < r.x + 30; x++) {
                Color c = img.colorAt(x, y);
                mixed += c != canvas && c != Color(0x10, 0x20, 0x40) && c != Color(0x30, 0x70, 0xc0);
            }
        CHECK(mixed > 40);
        r = at(14);  // rounded per-side colours: each side its own, mixed only along the corner's line
        CHECK(rgbIs(img, r.x + 50, r.y + 2, 255, 0, 0) && rgbIs(img, r.right() - 3, r.y + 30, 0, 255, 0) &&
              rgbIs(img, r.x + 50, r.bottom() - 3, 0, 0, 255) && rgbIs(img, r.x + 2, r.y + 30, 255, 165, 0));
        r = at(15);  // none: no border, no width
        CHECK(rgbIs(img, r.x, r.y, 0xee, 0xee, 0xee));
        r = at(17);  // a hidden left side, a dotted bottom
        CHECK(rgbIs(img, r.x, r.y + 30, 255, 255, 255) && rgbIs(img, r.x + 50, r.y, 255, 0, 0));
    }

    // --- gradients ---
    BoxSheet gs(
        ".v { background: linear-gradient(#ffffff, #000000); }\n"
        ".h { background-image: linear-gradient(90deg, #ff0000, #0000ff); }\n"
        ".corner { background-image: linear-gradient(to bottom right, #ffffff, #000000); }\n"
        ".hard { background-image: linear-gradient(to right, #ff0000 0 50%, #0000ff 50% 100%); }\n"
        ".xp { background: linear-gradient(#ffffff 0%, #ece9d8 85%, #d6d0c5 100%); border: 1px solid #003c74; border-radius: 3px; }\n"
        ".glass { background: linear-gradient(rgba(255,255,255,0.6) 0 50%, rgba(255,255,255,0) 50% 100%), linear-gradient(#a8d0f0, #2070b0);"
        " border: 1px solid rgba(0,0,0,0.5); border-radius: 6px; }\n"
        ".rad { background-image: radial-gradient(circle, #ffffff, #0000ff); }\n"
        ".radc { background-image: radial-gradient(circle closest-side at 30% 40%, yellow, red 60%, transparent); }\n"
        ".rep { background-image: repeating-linear-gradient(45deg, #ffcc00 0 8px, #222222 8px 16px); }\n"
        ".repr { background-image: repeating-radial-gradient(circle, #ff0000 0 5px, #ffffff 5px 10px); }\n"
        ".ell { background-image: radial-gradient(ellipse farthest-corner at 20px 20px, white, green); border-radius: 12px; }\n"
        ".fade { background-image: linear-gradient(to right, #ff0000, transparent); }\n");
    CHECK(gs.problems.empty());
    const char* gorder[] = {"v", "h", "corner", "hard", "xp", "glass", "rad", "radc", "rep", "repr", "ell", "fade"};
    Image gimg(cols * (bw + gap) + gap, 2 * (100 + gap) + gap, canvas);
    Painter gp(gimg);
    auto gat = [&](int i) { return Rect(gap + (i % cols) * (bw + gap), gap + (i / cols) * (100 + gap), bw, 100); };
    for (int i = 0; i < 12; i++) style::box(gp, gat(i), gs.style(gorder[i]));
    savePng(outDir() + "/css-gradients.png", gimg);
    {
        Rect r = gat(0);  // white at the top, black at the bottom, grey between; smooth: steps of at most 3 per row
        CHECK(grey(gimg, r.x + 50, r.y) >= 252 && grey(gimg, r.x + 50, r.bottom() - 1) <= 3);
        CHECK(std::abs(grey(gimg, r.x + 50, r.y + 50) - 127) <= 2);
        bool smooth = true;
        for (int y = r.y + 1; y < r.bottom(); y++) {
            int d = grey(gimg, r.x + 50, y - 1) - grey(gimg, r.x + 50, y);
            smooth = smooth && d >= 0 && d <= 3;
        }
        CHECK(smooth);
        CHECK(gimg.colorAt(r.x, r.y + 40) == gimg.colorAt(r.right() - 1, r.y + 40));  // (one colour along a row)
        r = gat(1);
        CHECK(rgbIs(gimg, r.x, r.y + 50, 254, 0, 1, 2) && rgbIs(gimg, r.right() - 1, r.y + 50, 1, 0, 254, 2) &&
              rgbIs(gimg, r.x + 50, r.y + 50, 127, 0, 128, 2));
        r = gat(2);  // to the corner: the corners black and white, the other diagonal mid-grey
        CHECK(grey(gimg, r.x, r.y) >= 250 && grey(gimg, r.right() - 1, r.bottom() - 1) <= 5);
        CHECK(std::abs(grey(gimg, r.right() - 1, r.y) - grey(gimg, r.x, r.bottom() - 1)) <= 2 &&
              std::abs(grey(gimg, r.right() - 1, r.y) - 128) <= 3);
        r = gat(3);  // a hard stop
        CHECK(rgbIs(gimg, r.x + 48, r.y + 50, 255, 0, 0) && rgbIs(gimg, r.x + 51, r.y + 50, 0, 0, 255));
        r = gat(5);  // glass: the gloss's hard line half way, the colour under it
        Color top = gimg.colorAt(r.x + 50, r.y + 49), below = gimg.colorAt(r.x + 50, r.y + 51);
        CHECK(top.r > below.r + 60);
        r = gat(6);  // radial: white in the middle, blue at the corners
        CHECK(rgbIs(gimg, r.x + 50, r.y + 50, 255, 255, 255, 4) && rgbIs(gimg, r.x, r.y, 0, 0, 255, 6));
        CHECK(gimg.colorAt(r.x + 30, r.y + 50) == gimg.colorAt(r.x + 69, r.y + 50));  // (symmetric)
        r = gat(8);  // repeating: one period (16 px along 45°: 16√2 across) apart, the same
        CHECK(gimg.colorAt(r.x + 30, r.y + 60) == gimg.colorAt(r.x + 40, r.y + 70));  // (along a stripe)
        {
            int yellow = 0, dark = 0;
            for (int x = r.x; x < r.right(); x++) {
                yellow += rgbIs(gimg, x, r.y + 50, 0xff, 0xcc, 0x00, 1);
                dark += rgbIs(gimg, x, r.y + 50, 0x22, 0x22, 0x22, 1);
            }
            CHECK(yellow > 30 && dark > 30);
        }
        CHECK(rgbIs(gimg, r.x + 1, r.bottom() - 2, 0xff, 0xcc, 0x00, 1));
        r = gat(9);  // repeating radial: rings of 5 px
        CHECK(rgbIs(gimg, r.x + 50, r.y + 50, 255, 0, 0, 1) && rgbIs(gimg, r.x + 57, r.y + 50, 255, 255, 255, 1) &&
              rgbIs(gimg, r.x + 62, r.y + 50, 255, 0, 0, 1));
        r = gat(11);  // to transparent: red fading, never grey
        Color mid = gimg.colorAt(r.x + 50, r.y + 50);
        Color expect = blendOver(Color(255, 0, 0, 128), canvas);
        CHECK(std::abs(mid.r - expect.r) <= 3 && std::abs(mid.g - expect.g) <= 3 && std::abs(mid.b - expect.b) <= 3);
    }

    // --- shadows ---
    BoxSheet ss(
        ".hard { background-color: white; box-shadow: 4px 4px 0 #000000; }\n"
        ".soft { background-color: white; border-radius: 8px; box-shadow: 0 4px 12px rgba(0,0,0,0.6); }\n"
        ".inner { background-color: #dddddd; box-shadow: inset 0 2px 6px rgba(0,0,0,0.7); }\n"
        ".ring { background-color: white; border-radius: 4px; box-shadow: 0 0 0 3px #3070c0, 0 0 10px 3px rgba(48,112,192,0.6); }\n"
        ".aero { background: linear-gradient(#e8f2fc, #b0d0f0); border: 1px solid #5080b0; border-radius: 6px;"
        " box-shadow: inset 0 1px 0 rgba(255,255,255,0.9), 0 2px 5px rgba(0,0,0,0.5); }\n"
        ".clear { background-color: rgba(255,255,255,0.3); box-shadow: 6px 6px 8px #000000; }\n");
    CHECK(ss.problems.empty());
    const char* sorder[] = {"hard", "soft", "inner", "ring", "aero", "clear"};
    Image simg(cols * (bw + 2 * gap) + gap, bh + 4 * gap, canvas);
    Painter sp(simg);
    auto sat = [&](int i) { return Rect(gap + i * (bw + 2 * gap), 2 * gap, bw, bh); };
    style::clearBoxCaches();
    int made0 = style::shadowMasksMade();
    for (int i = 0; i < 6; i++) style::box(sp, sat(i), ss.style(sorder[i]));
    int made1 = style::shadowMasksMade();
    CHECK(made1 - made0 == 8);  // (one mask per shadow)
    Image simg2(simg.width(), simg.height(), canvas);  // (a repaint: the kept masks, the same pixels)
    Painter sp2(simg2);
    for (int i = 0; i < 6; i++) style::box(sp2, sat(i), ss.style(sorder[i]));
    CHECK(style::shadowMasksMade() == made1);
    savePng(outDir() + "/css-shadows.png", simg2);
    {
        Rect r = sat(0);
        CHECK(rgbIs(simg2, r.right() + 2, r.bottom() + 2, 0, 0, 0) && rgbIs(simg2, r.right() + 5, r.bottom() + 5, canvas.r, canvas.g, canvas.b));
        CHECK(rgbIs(simg2, r.right() + 2, r.y + 2, canvas.r, canvas.g, canvas.b));  // (offset: none at the top right)
        r = sat(1);  // a soft shadow falling off below the box
        int d1 = darkness(simg2, r.x + 50, r.bottom() + 1), d2 = darkness(simg2, r.x + 50, r.bottom() + 5),
            d3 = darkness(simg2, r.x + 50, r.bottom() + 9), d0 = darkness(simg2, r.x + 50, r.bottom() + 20);
        int base = darkness(simg2, 2, 2);
        CHECK(d1 > d2 && d2 > d3 && d3 > base && d0 - base <= 3);
        CHECK(darkness(simg2, r.x + 50, r.y - 3) < darkness(simg2, r.x + 50, r.bottom() + 3));  // (offset down)
        CHECK(rgbIs(simg2, r.x + 50, r.y + 30, 255, 255, 255));
        r = sat(2);  // inset: darker near the top edge than in the middle
        CHECK(darkness(simg2, r.x + 50, r.y + 1) > darkness(simg2, r.x + 50, r.y + 8) + 30);
        CHECK(rgbIs(simg2, r.x + 50, r.y + 30, 0xdd, 0xdd, 0xdd, 1));
        r = sat(3);  // spread 3px, the first shadow on top
        CHECK(rgbIs(simg2, r.x + 50, r.y - 2, 0x30, 0x70, 0xc0) && rgbIs(simg2, r.x - 2, r.y + 30, 0x30, 0x70, 0xc0));
        CHECK(darkness(simg2, r.x + 50, r.y - 6) > base);
        r = sat(5);  // a see-through box: its shadow is cut out under it
        Color in = simg2.colorAt(r.x + 50, r.y + 30), expect = blendOver(Color(255, 255, 255, 77), canvas);
        CHECK(std::abs(in.r - expect.r) <= 1 && std::abs(in.g - expect.g) <= 1);
        CHECK(darkness(simg2, r.right() + 4, r.bottom() + 4) > base + 100);
        bool same = true;  // (kept masks draw the same pixels)
        for (int y = 0; y < simg.height(); y++)
            for (int x = 0; x < simg.width(); x++) same = same && simg.pixel(x, y) == simg2.pixel(x, y);
        CHECK(same);
    }

    // --- fractional rectangles and a scaled painter ---
    {
        Image fimg(220, 120, canvas);
        Painter fp(fimg);
        style::box(fp, RectF(10.5f, 10.25f, 80.5f, 40.5f), bs.style("outset2"));
        style::box(fp, RectF(110.3f, 10.6f, 90.2f, 40.1f), gs.style("glass"));
        Painter big(fimg, 1.5f);
        style::box(big, Rect(8, 40, 50, 30), bs.style("outset2"));
        style::box(big, Rect(70, 40, 60, 30), ss.style("soft"));
        savePng(outDir() + "/css-fractional.png", fimg);
        CHECK(grey(fimg, 50, 30) == 192);
        Color edge = fimg.colorAt(10, 30);  // (half covered: the light edge over the canvas)
        CHECK(edge.r > canvas.r && edge.r < 255);
        CHECK(grey(fimg, 12, 60) == 255 && grey(fimg, 13, 61) == 192);  // (scaled 1.5: 2 px are 3 device px)
        CHECK(grey(fimg, 14, 62) == 192 && grey(fimg, 86, 104) == 24);
    }

    // --- text-shadow on a Label, Button and list rows; opacity on a widget with a child ---
    std::vector<css::Problem> problems;
    css::SheetPtr extra = css::parse(
        "#shadowed { color: white; text-shadow: 1px 1px 0 #000000, 0 0 4px #ff0000; font-size: 18px; background-color: #406080; }\n"
        "#sharp { color: #ffffff; text-shadow: 2px 2px 0 #000000; background-color: #808080; font-size: 16px; }\n"
        "#outer { opacity: 0.5; background-color: #ff0000; }\n"
        "#inner { background-color: #0000ff; }\n"
        "#lit { color: #ffffff; text-shadow: 0 2px 0 #ff00ff; }\n"
        "#rows::item { color: #ffffff; text-shadow: 1px 1px 0 #00ffff; }\n",
        "paint.css", problems);
    CHECK(problems.empty());
    theme::setStyles({theme::builtinSheet(), extra});
    {
        Window w(plat::WindowOptions{});
        w.resizeWindow(360, 200);
        auto* shadowed = w.add<Label>("Shadowed text");
        shadowed->objectName = "shadowed";
        shadowed->setGeometry(10, 10, 200, 40);
        auto* sharp = w.add<Label>("Sharp");
        sharp->objectName = "sharp";
        sharp->setGeometry(220, 10, 120, 40);
        auto* outer = w.add<Label>("");
        outer->objectName = "outer";
        outer->setGeometry(10, 60, 160, 70);
        auto* inner = outer->add<Label>("");
        inner->objectName = "inner";
        inner->setGeometry(20, 15, 60, 40);
        auto* lit = w.add<Button>("Lit button");
        lit->objectName = "lit";
        lit->setGeometry(190, 70, 140, 30);
        auto* rows = w.add<ListWidget>();
        rows->objectName = "rows";
        rows->addItem("A list row");
        rows->addItem("Another row");
        rows->setGeometry(10, 140, 200, 56);
        w.showWindow();
        App::get().tick();
        savePicture(w, "css-text");
        const Image& fb = w.framebuffer();
        Color bgc = fb.colorAt(200, 120);  // (the window's own background)
        // the opacity: the parent's red at half over the window, the child's blue at half over the window
        // (not over the red: the two are put on together, as one layer)
        Color red = fb.colorAt(15, 65), blue = fb.colorAt(50, 100);
        Color expRed = blendOver(Color(255, 0, 0, 128), bgc), expBlue = blendOver(Color(0, 0, 255, 128), bgc);
        CHECK(std::abs(red.r - expRed.r) <= 1 && std::abs(red.g - expRed.g) <= 1 && std::abs(red.b - expRed.b) <= 1);
        CHECK(std::abs(blue.r - expBlue.r) <= 1 && std::abs(blue.g - expBlue.g) <= 1 && std::abs(blue.b - expBlue.b) <= 1);
        // the sharp shadow: black pixels 2 px right and down of white ones
        int shadowPx = 0, whitePx = 0;
        for (int y = 10; y < 50; y++)
            for (int x = 220; x < 340; x++) {
                Color c = fb.colorAt(x, y);
                if (c == Color(255, 255, 255)) {
                    whitePx++;
                    Color d = fb.colorAt(x + 2, y + 2);
                    if (d == Color(0, 0, 0)) shadowPx++;
                }
            }
        CHECK(whitePx > 20 && shadowPx > whitePx / 4);
        // the blurred red glow: reddish pixels around the text, beyond the sharp shadow
        int glow = 0;
        for (int y = 10; y < 50; y++)
            for (int x = 10; x < 210; x++) {
                Color c = fb.colorAt(x, y);
                if (c.r > 0x40 + 20 && c.g < 0x60 && c.b < 0x80) glow++;
            }
        CHECK(glow > 50);
        // the button's and the list rows' shadows
        auto count = [&](Rect area, bool magenta) {  // (pixels mostly of the shadow's colour)
            int n = 0;
            for (int y = area.y; y < area.bottom(); y++)
                for (int x = area.x; x < area.right(); x++) {
                    Color c = fb.colorAt(x, y);
                    n += magenta ? c.r > c.g + 80 && c.b > c.g + 80 : c.g > c.r + 80 && c.b > c.r + 80;
                }
            return n;
        };
        CHECK(count(Rect(190, 70, 140, 30), true) > 15);
        CHECK(count(Rect(10, 140, 200, 56), false) > 15);
        // with no opacity anywhere the layer isn't made: the same picture as painting directly (default.css)
    }
    theme::setSkin();
    CHECK(!css::declaresOnNodes(theme::styles(), "opacity"));

    // --- timings: a plain box, a gradient box, a blurred shadow (kept and made) ---
    {
        BoxSheet ts(
            ".plain { background-color: #333333; border: 1px solid #555555; border-radius: 3px; }\n"
            ".flat { background-color: #333333; }\n"
            ".grad { background: linear-gradient(#fafafa, #d0d0d0); border: 1px solid #555555; border-radius: 3px; }\n"
            ".shadow { background-color: white; border-radius: 3px; box-shadow: 0 2px 6px rgba(0,0,0,0.5); }\n");
        Image timg(300, 100, canvas);
        Painter tp(timg);
        Rect r(20, 20, 100, 24);
        auto time = [&](const css::Style& st, int n, bool clear) {
            style::box(tp, r, st);
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < n; i++) {
                if (clear) style::clearBoxCaches();
                style::box(tp, r, st);
            }
            return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
        };
        const css::Style& plain = ts.style("plain");
        const css::Style& flat = ts.style("flat");
        const css::Style& grad = ts.style("grad");
        const css::Style& shadow = ts.style("shadow");
        CHECK(!style::boxLook(plain).more && !style::boxLook(flat).more && style::boxLook(grad).more && style::boxLook(shadow).more);
        // the box code as it was before gradients and shadows (the plain path must cost what it did)
        auto before = [&](const css::Style& st, int n) {
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < n; i++) {
                const style::BoxLook& b = style::boxLook(st);
                float rad = theme::rounded ? b.radius : 0.f;
                if (b.fillOnly) {
                    style::fill(tp, r, b.bg, rad);
                    continue;
                }
                style::fill(tp, r, b.edge, rad);
                int most = std::max(std::max(b.border.left, b.border.right), std::max(b.border.top, b.border.bottom));
                style::fill(tp, style::inside(r, b.border), b.bg, std::max(0.f, rad - (float)most));
            }
            return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
        };
        auto fills = [&](int n) {
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < n; i++) tp.fillRect(r, Color(0x33, 0x33, 0x33));
            return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / n;
        };
        double tFill = fills(400000), tFlat = time(flat, 400000, false), tFlatBefore = before(flat, 400000);
        double tPlain = time(plain, 50000, false), tPlainBefore = before(plain, 50000);
        double tGrad = time(grad, 20000, false), tShadow = time(shadow, 20000, false), tShadowNew = time(shadow, 2000, true);
        printf("style::box 100x24: flat %.0f ns (code before: %.0f, fillRect: %.0f); rounded with a border %.0f ns (code "
               "before: %.0f); gradient %.0f ns; blurred shadow %.0f ns kept / %.0f ns made\n",
               tFlat, tFlatBefore, tFill, tPlain, tPlainBefore, tGrad, tShadow, tShadowNew);
    }
}

// outer box-shadows reach beyond the widget (a glow around a focused button), and are repainted away
static void testShadowOverflow() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(200, 60);
    auto* b = w.add<Button>("Glow");
    b->setGeometry(40, 15, 80, 26);
    std::vector<css::Problem> problems;
    theme::setStyles({theme::builtinSheet(),
                      css::parse("PushButton:focus { box-shadow: 0 0 6px 3px #ff0000; }", "glow.css", problems)});
    CHECK(problems.empty());
    w.showWindow();
    App::get().tick();
    Color before = w.framebuffer().colorAt(36, 28);  // (4 px left of the button)
    b->setFocus();
    App::get().tick();
    Color glow = w.framebuffer().colorAt(36, 28);
    CHECK(glow.r > before.r + 40 && glow.g < glow.r);
    CHECK(b->paintRect().x < 0 && b->paintRect().right() > b->width());
    b->clearFocus();
    App::get().tick();
    CHECK(w.framebuffer().colorAt(36, 28) == before);  // (repainted away)
    theme::setSkin();
    CHECK(b->paintRect() == b->rect());
    w.hideWindow();
}

// a border around a see-through background: the border is a ring, the inside shows what is under it
static void testSeeThroughBorder() {
    Window w(plat::WindowOptions{});
    w.resizeWindow(120, 60);
    auto* g = w.add<GroupBox>("");
    g->setGeometry(10, 10, 100, 40);
    std::vector<css::Problem> problems;
    theme::setStyles({theme::builtinSheet(),
                      css::parse("GroupBox { margin-top: 0; background-color: transparent; border: 2px solid #ff0000; "
                                 "border-radius: 0; }",
                                 "ring.css", problems)});
    CHECK(problems.empty());
    w.showWindow();
    App::get().tick();
    const Image& fb = w.framebuffer();
    CHECK(fb.colorAt(10, 30) == Color(255, 0, 0) && fb.colorAt(11, 30) == Color(255, 0, 0));
    CHECK(fb.colorAt(60, 30) == theme::c("window"));  // (the inside: the window, not the border's red)
    // half see-through: tinted, not red
    theme::setStyles({theme::builtinSheet(),
                      css::parse("GroupBox { margin-top: 0; background-color: rgb(0 0 255 / 50%); border: 2px solid #ff0000; "
                                 "border-radius: 0; }",
                                 "ring2.css", problems)});
    App::get().repaintAll();
    App::get().tick();
    Color mid = w.framebuffer().colorAt(60, 30);
    CHECK(mid.r < 100 && mid.b > 100);
    theme::setSkin();
    w.hideWindow();
}

// what skins of classic desktops draw controls with (over default.css, whose look they leave alone): a group box's
// title as a box across its frame's line, an etched separator, a scroll bar's arrow buttons (a click steps it, held
// they repeat), the file list's header inside its border, the tab widget's pane; setSkin() brings all back
static void testClassicControlParts() {
    theme::setSkin();
    Window w(plat::WindowOptions{});
    w.resizeWindow(400, 300);
    auto* gb = w.add<GroupBox>("Group");
    gb->setGeometry(10, 10, 180, 100);
    auto* sep = w.add<Separator>(Orientation::Vertical);
    sep->setGeometry(200, 10, 8, 60);
    auto* hsep = w.add<Separator>(Orientation::Horizontal);
    hsep->setGeometry(220, 10, 60, 8);
    auto* bar = w.add<ScrollBar>(Orientation::Vertical);
    bar->setGeometry(300, 10, 16, 150);
    bar->setRange(0, 100);
    bar->setPageStep(10);
    bar->setSingleStep(7);
    auto* hbar = w.add<ScrollBar>(Orientation::Horizontal);
    hbar->setGeometry(10, 270, 200, 16);
    hbar->setRange(0, 100);
    hbar->setPageStep(10);
    hbar->setSingleStep(5);
    auto* tabs = w.add<TabWidget>();
    Widget* page = tabs->addTab(std::make_unique<Widget>(), "One");
    tabs->setGeometry(10, 130, 180, 120);
    int bh = tabs->barHeight();
    w.showWindow();
    App::get().tick();
    const Image& fb = w.framebuffer();
    // default.css: as before
    CHECK(gb->contentsRect() == Rect(6, 28, 168, 66));
    CHECK(fb.colorAt(10 + 2, 10 + 10) == theme::c("window"));  // (the title is no box: the window beside its text)
    CHECK(sep->sizeHint().x == 7 && hsep->sizeHint().y == 1);
    CHECK(bar->lineRect(-1).empty() && bar->lineRect(1).empty() && bar->trackRect() == bar->rect());
    CHECK(hbar->lineRect(-1).empty() && hbar->trackRect() == hbar->rect());
    CHECK(page->geometry() == Rect(0, bh, 180, 120 - bh));
    Point tabsHint = tabs->sizeHint(), gbMin = gb->minimumSizeHint();

    std::vector<css::Problem> problems;
    css::SheetPtr extra = css::parse(
        "GroupBox { margin-top: 7px; background-color: transparent; border: 2px solid #0000ff; border-radius: 0; }\n"
        "GroupBox::title { margin-top: -7px; margin-left: 8px; background-color: #00ff00; padding: 0 3px; }\n"
        "Separator:vertical { border-left: 1px solid #808080; border-right: 1px solid #ffffff; width: 2px; }\n"
        "Separator:horizontal { border-top: 1px solid #808080; border-bottom: 1px solid #ffffff; height: 2px; }\n"
        "ScrollBar::sub-line, ScrollBar::add-line { width: 16px; height: 16px; background-color: #c0c0c0; "
        "color: #000000; }\n"
        "ScrollBar::sub-line:pressed { background-color: #ff00ff; }\n"
        "TabWidget::pane { border: 2px solid #ff0000; padding: 3px; background-color: #ffff00; }\n",
        "classic.css", problems);
    CHECK(problems.empty());
    theme::setStyles({theme::builtinSheet(), extra});
    App::get().repaintAll();
    App::get().tick();
    // the group box: the frame 7 px down, the title's box (green) across its 2 px line, 8 px in; the contents below
    // the title (it reaches 12 px below the frame's top: 10 below its border), then the padding
    int th = (int)std::ceil(Font::ui().ascent() + Font::ui().descent());
    int titleW = (int)std::ceil(Font::ui().width("Group")) + 6;
    CHECK(fb.colorAt(14, 17) == Color(0, 0, 255) && fb.colorAt(14, 18) == Color(0, 0, 255));      // the frame's line
    CHECK(fb.colorAt(19, 17) == Color(0, 255, 0) && fb.colorAt(19, 18) == Color(0, 255, 0));      // the title over it
    CHECK(fb.colorAt(18 + titleW - 1, 17) == Color(0, 255, 0));
    CHECK(fb.colorAt(18 + titleW + 2, 17) == Color(0, 0, 255));                                   // the line again
    CHECK(fb.colorAt(19, 10 + (18 - th) / 2 - 1) == theme::c("window"));                          // above the title
    CHECK(gb->contentsRect() == Rect(8, 27, 164, 65));
    CHECK(gb->minimumSizeHint().y == 7 + 4 + 14 + 10 && gbMin.y == 20 + 14);
    // the etched separators: two colours across them
    CHECK(sep->sizeHint().x == 2 + 6 && hsep->sizeHint().y == 2);
    CHECK(fb.colorAt(203, 40) == Color::fromHex("#808080") && fb.colorAt(204, 40) == Color(255, 255, 255));
    CHECK(fb.colorAt(202, 40) == theme::c("window") && fb.colorAt(205, 40) == theme::c("window"));
    CHECK(fb.colorAt(250, 13) == Color::fromHex("#808080") && fb.colorAt(250, 14) == Color(255, 255, 255));
    // the scroll bars' arrow buttons: 16 px at each end, an arrow in them, the track between
    CHECK(bar->lineRect(-1) == Rect(0, 0, 16, 16) && bar->lineRect(1) == Rect(0, 134, 16, 16));
    CHECK(bar->trackRect() == Rect(0, 16, 16, 118));
    CHECK(bar->handleRect().y >= 16 && bar->handleRect().bottom() <= 134);
    CHECK(hbar->lineRect(-1) == Rect(0, 0, 16, 16) && hbar->lineRect(1) == Rect(184, 0, 16, 16));
    CHECK(hbar->trackRect() == Rect(16, 0, 168, 16));
    auto dark = [&](int x0, int y0) {
        int n = 0;
        for (int y = y0; y < y0 + 16; ++y)
            for (int x = x0; x < x0 + 16; ++x)
                if (fb.colorAt(x, y).r < 100) ++n;
        return n;
    };
    CHECK(fb.colorAt(301, 11) == Color::fromHex("#c0c0c0") && dark(300, 10) > 2 && dark(300, 144) > 2);
    CHECK(dark(10, 270) > 2 && dark(194, 270) > 2);
    // a click steps it by the single step; held, it repeats (after 300 ms, every 100 ms)
    click(w, 308, 152);
    CHECK(bar->value() == 7);
    click(w, 308, 18);
    CHECK(bar->value() == 0);
    click(w, 308, 18);
    CHECK(bar->value() == 0);  // (at its least)
    press(w, 308, 152);
    CHECK(bar->value() == 7);
    App::get().advanceTimers(0.35);
    CHECK(bar->value() == 14);
    App::get().advanceTimers(0.11);
    CHECK(bar->value() == 21);
    moveTo(w, 308, 80, plat::LeftButton);  // (off the button: it waits)
    App::get().advanceTimers(0.11);
    CHECK(bar->value() == 21);
    release(w, 308, 80);
    App::get().advanceTimers(0.5);
    CHECK(bar->value() == 21);
    press(w, 308, 18);  // held: :pressed
    App::get().tick();
    CHECK(fb.colorAt(301, 11) == Color(255, 0, 255));
    release(w, 308, 18);
    App::get().tick();
    CHECK(bar->value() == 14 && fb.colorAt(301, 11) == Color::fromHex("#c0c0c0"));
    click(w, 10 + 192, 278);
    CHECK(hbar->value() == 5);
    // the thumb moves in the track: dragged to its end, the bar is at its most
    Rect hr = bar->handleRect();
    press(w, 308, 10 + hr.y + hr.h / 2);
    moveTo(w, 308, 300, plat::LeftButton);
    release(w, 308, 300);
    CHECK(bar->value() == 100 && bar->handleRect().bottom() <= 134);
    // the tab widget's pane: a box under the tabs around the page, the page inside its border and padding
    CHECK(page->geometry() == Rect(5, bh + 5, 170, 120 - bh - 10));
    CHECK(tabs->sizeHint() == Point(std::max(tabsHint.x, 10), tabsHint.y + 10));
    CHECK(fb.colorAt(10, 130 + bh + 20) == Color(255, 0, 0) && fb.colorAt(11, 130 + bh) == Color(255, 0, 0));
    CHECK(fb.colorAt(13, 130 + bh + 20) == Color(255, 255, 0));
    CHECK(fb.colorAt(10 + 175, 130 + 3) == theme::c("window"));  // (the tab bar: no pane there)
    // the file list's header inside the list's border
    std::string dir = "build/toolkit-samples/files-classic";
    util::makeDirs(dir);
    theme::setStyles({theme::builtinSheet(), extra,
                      css::parse("FileList { border: 3px solid #ff0000; }\n"
                                 "FileList::header { background-color: #00ff00; }\n",
                                 "classic-list.css", problems)});
    CHECK(problems.empty());
    Color edge, header, right;
    bool found = false;
    App::get().singleShot(30, [&] {
        Window* top = App::get().modalWindow();
        if (!top) return;
        if (Widget* list = findWidget(top, [](Widget* x) { return x->isType("FileList"); })) {
            found = true;
            App::get().tick();
            const Image& tfb = top->framebuffer();
            Point at = list->mapToWindow(Point(list->width() / 2, 1));
            edge = tfb.colorAt(at.x, at.y);
            at = list->mapToWindow(Point(list->width() / 2, 3 + 2));
            header = tfb.colorAt(at.x, at.y);
            at = list->mapToWindow(Point(list->width() - 2, list->height() / 2));
            right = tfb.colorAt(at.x, at.y);
        }
        key(*top, plat::Key_Escape);
    });
    CHECK(files::openFile(&w, "Classic files", util::absolutePath(dir), "All files (*)").empty());
    CHECK(found && edge == Color(255, 0, 0) && header == Color(0, 255, 0) && right == Color(255, 0, 0));
    // back to default.css
    theme::setSkin();
    App::get().repaintAll();  // (as the program does on a skin change)
    App::get().tick();
    CHECK(gb->contentsRect() == Rect(6, 28, 168, 66) && gb->minimumSizeHint() == gbMin);
    CHECK(fb.colorAt(10 + 2, 10 + 10) == theme::c("window"));
    CHECK(sep->sizeHint().x == 7 && hsep->sizeHint().y == 1);
    CHECK(bar->lineRect(-1).empty() && bar->lineRect(1).empty() && bar->trackRect() == bar->rect());
    CHECK(page->geometry() == Rect(0, bh, 180, 120 - bh) && tabs->sizeHint() == tabsHint);
    w.hideWindow();
}

// ------------------------------------------------------------------------------------------
// Tiny DAW's own window frames (App::ownFrames), forced on without a display: the title bar above the inside,
// the border around both; what the inside sees doesn't change; the buttons, a double click, the menu, dragging
// the bar and the edges; :active / :inactive; the stylesheets' sizes and colours; the option turned off
namespace {
// a point of the window (the inside's coordinates: the title bar's negative) as the platform gives it: from the
// native window's corner, the frame's insets too
Point native(Window& w, Point p) {
    Window::FrameInsets f = w.frameInsets();
    return Point(p.x + f.left, p.y + f.top);
}
Point partCentre(Window& w, TitleBar::Part part) {
    TitleBar* tb = w.titleBar();
    Rect r = tb->partRect(part);
    return Point(r.x + r.w / 2 + tb->x(), r.y + r.h / 2 + tb->y());
}
void clickAt(Window& w, Point inside, int button = plat::LeftButton) {
    Point n = native(w, inside);
    click(w, n.x, n.y, button);
}
}  // namespace

static void testWindowFrames() {
    App& app = App::get();
    std::vector<std::string> actions;
    int conn = app.frameAction.connect([&](Window*, std::string what) { actions.push_back(what); });
    {
        // headless windows have no frame unless asked for
        Window plain(plat::WindowOptions{});
        CHECK(!plain.isFramed() && !plain.titleBar() && plain.frameInsets() == Window::FrameInsets());
        CHECK(plain.outerRect() == plain.rect() && !plain.hasState("framed"));
    }
    app.setFramesWhenHeadless(true);
    CHECK(app.framesDrawn());

    // --- the layout: the inside is the size asked for; the title bar above it, the border around both ---
    plat::WindowOptions o;
    o.title = "Framed window";
    o.w = 400;
    o.h = 300;
    Window w(o);
    CHECK(w.isFramed() && w.titleBar() && w.titleBar()->parent() == &w && w.titleBar()->window() == &w);
    CHECK(w.width() == 400 && w.height() == 300 && w.contentsRect() == Rect(0, 0, 400, 300));
    int bar = TitleBar::barHeight();
    CHECK(bar == 30);
    Window::FrameInsets f = w.frameInsets();
    CHECK(f.left == 1 && f.right == 1 && f.bottom == 1 && f.top == 1 + bar);
    CHECK(w.outerRect() == Rect(-1, -1 - bar, 402, 302 + bar));
    CHECK(w.titleBar()->geometry() == Rect(0, -bar, 400, bar));
    CHECK(w.hasState("framed") && !w.hasState("maximized"));
    // what is inside is laid out in the inside, as without a frame
    auto* v = vbox(&w, 0, 0);
    auto* top = w.add<Button>("Top");
    v->addWidget(top);
    v->addStretch();
    w.showWindow();
    app.doLayouts();
    CHECK(top->x() == 0 && top->y() == 0 && top->width() == 400);
    CHECK(w.childAt(Point(5, 5)) == top);
    app.tick();
    const Image& fb = w.framebuffer();
    CHECK(fb.width() == 402 && fb.height() == 302 + bar);
    // the border (inactive: --frame-edge), the bar's background, the window's below it
    Color edge = style::color(w.computedStyle(), "border-top-color");
    CHECK(near(fb.colorAt(0, 100), edge, 1) && near(fb.colorAt(401, 100), edge, 1) && near(fb.colorAt(200, 0), edge, 1));
    CHECK(near(fb.colorAt(200, fb.height() - 1), edge, 1));
    Color barBg = style::background(w.titleBar()->computedStyle());
    CHECK(near(fb.colorAt(200, 1 + bar / 2), barBg, 1));
    savePng(outDir() + "/frame-inactive.png", fb);
    // the parts: the title on the left, the buttons on the right (close outermost)
    TitleBar* tb = w.titleBar();
    Rect cl = tb->partRect(TitleBar::Close), mx = tb->partRect(TitleBar::Maximize), mn = tb->partRect(TitleBar::Minimize);
    CHECK(!cl.empty() && !mx.empty() && !mn.empty() && tb->partRect(TitleBar::Icon).empty());
    CHECK(cl.right() == 400 && mx.right() == cl.x && mn.right() == mx.x && cl.w == 46 && cl.h == bar);
    CHECK(tb->partRect(TitleBar::Title).x == 10 && tb->partRect(TitleBar::Title).right() <= mn.x);
    CHECK(tb->partAt(Point(cl.x + 3, 10)) == TitleBar::Close && tb->partAt(Point(50, 10)) == TitleBar::Title);
    CHECK(tb->toolTipAt(Point(cl.x + 3, 10)) == "Close" && tb->toolTipAt(Point(mx.x + 3, 10)) == "Maximize");

    // --- :active / :inactive: the focus in the window or not ---
    CHECK(tb->hasState("inactive") && !tb->hasState("active") && w.hasState("inactive"));
    plat::Event focus;
    focus.type = plat::Event::FocusIn;
    w.onEvent(nullptr, focus);
    CHECK(tb->hasState("active") && !tb->hasState("inactive") && w.hasState("active"));
    app.tick();
    Color activeBg = style::background(tb->computedStyle());
    CHECK(!near(activeBg, barBg, 2));  // (default.css: a darker bar while active)
    CHECK(near(w.framebuffer().colorAt(200, 1 + bar / 2), activeBg, 1));
    Color activeEdge = style::color(w.computedStyle(), "border-top-color");
    CHECK(!near(activeEdge, edge, 2) && near(w.framebuffer().colorAt(0, 100), activeEdge, 1));
    savePng(outDir() + "/frame-active.png", w.framebuffer());
    // a button's :hover
    Point closeAt = partCentre(w, TitleBar::Close);
    Point n = native(w, closeAt);
    moveTo(w, n.x, n.y);
    CHECK(tb->hoverPart() == TitleBar::Close && w.hoverWidget() == tb);
    app.tick();
    CHECK(near(w.framebuffer().colorAt(n.x - 15, n.y - 8), Color(0xc4, 0x2b, 0x1c), 1));  // (default.css: red)
    savePng(outDir() + "/frame-close-hover.png", w.framebuffer());
    moveTo(w, native(w, Point(200, 150)).x, native(w, Point(200, 150)).y);
    CHECK(tb->hoverPart() == TitleBar::NoPart);

    // --- the buttons ---
    actions.clear();
    clickAt(w, partCentre(w, TitleBar::Minimize));
    CHECK(actions.size() == 1 && actions[0] == "minimize" && w.isWindowMinimized());
    actions.clear();
    clickAt(w, partCentre(w, TitleBar::Maximize));
    CHECK(actions.size() == 1 && actions[0] == "maximize" && w.isWindowMaximized());
    CHECK(w.hasState("maximized") && tb->hasState("maximized"));
    // maximized: no border (default.css), the inside takes the room; no resize band; the button restores
    CHECK(w.frameInsets().left == 0 && w.frameInsets().top == bar && w.width() == 402 && w.height() == 302);
    CHECK(tb->toolTipAt(tb->partRect(TitleBar::Maximize).center()) == "Restore");
    CHECK(w.resizeEdgeAt(Point(0, 100)) == 0);
    app.tick();
    savePng(outDir() + "/frame-maximized.png", w.framebuffer());
    clickAt(w, partCentre(w, TitleBar::Maximize));
    CHECK(actions.size() == 2 && actions[1] == "restore" && !w.isWindowMaximized() && w.frameInsets() == f);
    w.resizeWindow(400, 300);
    // a double click on the bar maximizes, another restores
    actions.clear();
    Point t = native(w, Point(150, -bar / 2));
    doubleClick(w, t.x, t.y);
    CHECK(w.isWindowMaximized() && actions.size() == 1 && actions[0] == "maximize");
    gTime += 1;
    doubleClick(w, t.x, t.y);
    CHECK(!w.isWindowMaximized());
    w.resizeWindow(400, 300);
    // close: as the window manager's ✕ (closeRequested asks first)
    int asked = 0;
    bool allow = false;
    w.closeRequested = [&] {
        ++asked;
        return allow;
    };
    clickAt(w, partCentre(w, TitleBar::Close));
    CHECK(asked == 1 && w.isWindowShown());
    allow = true;
    clickAt(w, partCentre(w, TitleBar::Close));
    CHECK(asked == 2 && !w.isWindowShown());
    w.showWindow();
    // a press on a button let go elsewhere does nothing
    Point mxAt = native(w, partCentre(w, TitleBar::Maximize));
    press(w, mxAt.x, mxAt.y);
    CHECK(tb->pressedPart() == TitleBar::Maximize);
    release(w, mxAt.x - 200, mxAt.y);
    CHECK(!w.isWindowMaximized() && tb->pressedPart() == TitleBar::NoPart);

    // --- the window's menu: right click on the bar ---
    clickAt(w, Point(120, -bar / 2), plat::RightButton);
    CHECK(tb->menu() && tb->menu()->isOpen() && tb->menu()->actions().size() == 4);
    if (tb->menu() && tb->menu()->actions().size() == 4) {
        CHECK(tb->menu()->actions()[0]->text == "Minimize" && tb->menu()->actions()[1]->text == "Maximize" &&
              tb->menu()->actions()[3]->text == "Close");
        Menu::activate(tb->menu()->actions()[1].get());
        tb->menu()->close();
        app.advanceTimers(0.01);
        CHECK(w.isWindowMaximized());
        w.setWindowMaximized(false);
    }
    app.closeAllPopups();
    w.resizeWindow(400, 300);

    // --- dragging the bar moves the window (without a window manager: by the program) ---
    w.moveWindow(300, 200);
    actions.clear();
    Point g0 = native(w, Point(150, -bar / 2));
    press(w, g0.x, g0.y);
    w.onEvent(nullptr, mouseEv(plat::Event::MouseMove, g0.x + 2, g0.y + 1, 0, plat::LeftButton));
    CHECK(w.screenGeometry().x == 300 && actions.empty());  // (not yet: a click may still be meant)
    w.onEvent(nullptr, mouseEv(plat::Event::MouseMove, g0.x + 30, g0.y + 20, 0, plat::LeftButton));
    // (the pointer's place is the window's: the window moved under it, so the next one is from the new place)
    CHECK(actions.size() == 1 && actions[0] == "move" && w.screenGeometry().x == 330 && w.screenGeometry().y == 220);
    release(w, g0.x, g0.y);

    // --- the resize band: around the edges, over what is inside; corners resize both ways ---
    Rect out = w.outerRect();
    CHECK(w.resizeEdgeAt(Point(out.x, 150)) == plat::EdgeLeft);
    CHECK(w.resizeEdgeAt(Point(2, 150)) == plat::EdgeLeft);           // (over the inside's edge)
    CHECK(w.resizeEdgeAt(Point(3, 150)) == 0);
    CHECK(w.resizeEdgeAt(Point(out.right() - 1, 150)) == plat::EdgeRight);
    CHECK(w.resizeEdgeAt(Point(200, out.bottom() - 1)) == plat::EdgeBottom);
    CHECK(w.resizeEdgeAt(Point(200, out.y)) == plat::EdgeTop);         // (the top of the title bar)
    CHECK(w.resizeEdgeAt(Point(200, -bar / 2)) == 0);                  // (the title bar itself: a move)
    CHECK(w.resizeEdgeAt(Point(200, 150)) == 0);
    CHECK(w.resizeEdgeAt(Point(out.x, out.y)) == (plat::EdgeLeft | plat::EdgeTop));
    CHECK(w.resizeEdgeAt(Point(out.x + 5, out.y)) == (plat::EdgeLeft | plat::EdgeTop));  // (along the corner)
    CHECK(w.resizeEdgeAt(Point(out.right() - 1, out.bottom() - 10)) == (plat::EdgeRight | plat::EdgeBottom));
    CHECK(w.resizeEdgeAt(Point(out.right() - 1, -bar)) == (plat::EdgeRight | plat::EdgeTop));
    CHECK(w.resizeEdgeAt(Point(out.x - 1, 150)) == 0);                 // (outside)
    // its cursor, and a drag of the right edge (by the program, without a window manager)
    Point e0 = native(w, Point(out.right() - 1, 150));
    moveTo(w, e0.x, e0.y);
    CHECK(w.hoverWidget() == nullptr);
    actions.clear();
    press(w, e0.x, e0.y);
    w.onEvent(nullptr, mouseEv(plat::Event::MouseMove, e0.x + 50, e0.y + 40, 0, plat::LeftButton));
    CHECK(actions.size() == 1 && actions[0] == "resize" && w.width() == 450 && w.height() == 300);
    // (the pointer's place counts from the window, which kept its place: the right edge only widens it)
    release(w, e0.x + 50, e0.y + 40);
    CHECK(w.width() == 450 && top->width() == 450);
    // the left edge: wider, and moved left as much
    Rect before = w.screenGeometry();
    Point l0 = native(w, Point(out.x, 150));
    press(w, l0.x, l0.y);
    w.onEvent(nullptr, mouseEv(plat::Event::MouseMove, l0.x - 20, l0.y, 0, plat::LeftButton));
    release(w, l0.x - 20, l0.y);
    CHECK(w.width() == 470 && w.screenGeometry().x == before.x - 20 && w.screenGeometry().right() == before.right());
    // a window that can't be resized: no band, no maximize
    w.setResizable(false);
    CHECK(w.resizeEdgeAt(Point(out.x, 150)) == 0 && !tb->hasButton(TitleBar::Maximize));
    CHECK(tb->partRect(TitleBar::Maximize).empty() && tb->partRect(TitleBar::Minimize).right() == tb->partRect(TitleBar::Close).x);
    Point t2 = native(w, Point(150, -bar / 2));
    gTime += 1;
    doubleClick(w, t2.x, t2.y);
    CHECK(!w.isWindowMaximized());
    w.setResizable(true);
    w.resizeWindow(400, 300);

    // --- dialogs: the size asked for is the inside's; a modal one has no minimize ---
    {
        Dialog d(&w, "Framed dialog", 300, 200);
        CHECK(d.isFramed() && d.width() == 300 && d.height() == 200);
        auto* dv = vbox(&d, 10, 6);
        auto* field = d.add<LineEdit>();
        dv->addWidget(field);
        dv->addStretch();
        d.adjustSize();
        Point hint = d.layout()->sizeHint();
        CHECK(d.width() == std::max({hint.x, d.layout()->minimumSize().x, 120}));
        d.resizeWindow(300, 200);
        d.showWindow();
        app.doLayouts();
        CHECK(field->x() == 10 && field->y() == 10 && field->width() == 280);
        app.tick();
        CHECK(d.framebuffer().width() == 302 && d.framebuffer().height() == 202 + bar);
        CHECK(d.titleBar()->hasButton(TitleBar::Minimize));
        d.setModal(true);
        CHECK(!d.titleBar()->hasButton(TitleBar::Minimize) && d.titleBar()->hasButton(TitleBar::Close));
        d.update();
        app.tick();
        savePng(outDir() + "/frame-dialog.png", d.framebuffer());
        d.setModal(false);
        d.hideWindow();
    }
    // popups have none
    {
        PopupWindow pop;
        CHECK(!pop.isFramed() && pop.frameInsets() == Window::FrameInsets());
    }

    // --- the stylesheets: the bar's height and colours, the border, where the buttons are ---
    std::vector<css::Problem> problems;
    theme::setStyles({theme::builtinSheet(),
                      css::parse(":root { --title-bar-height: 40px; --title-buttons: left; }"
                                 "Window:framed { border: 3px solid #0000ff; }"
                                 "TitleBar, TitleBar:inactive { background-color: #ff0000; padding: 0; }"
                                 "TitleBar::title, TitleBar:inactive::title { color: #00ff00; text-align: center; }"
                                 "TitleBar::close { width: 30px; }",
                                 "frames.css", problems)});
    CHECK(problems.empty());
    app.repaintAll();
    app.tick();
    CHECK(TitleBar::barHeight() == 40 && w.frameInsets().top == 43 && w.frameInsets().left == 3);
    CHECK(w.width() == 400 && w.height() == 300 && tb->geometry() == Rect(0, -40, 400, 40));  // (the inside kept)
    CHECK(w.framebuffer().width() == 406 && w.framebuffer().height() == 346);
    CHECK(near(w.framebuffer().colorAt(1, 100), Color(0, 0, 255), 1));
    cl = tb->partRect(TitleBar::Close);
    CHECK(cl.x == 0 && cl.w == 30 && tb->partRect(TitleBar::Maximize).x == 30);  // (left: close outermost)
    CHECK(near(w.framebuffer().colorAt(3 + 200, 3 + 3), Color(255, 0, 0), 1));
    // the title's green, centred
    int greenLeft = 1000, greenRight = -1;
    for (int x = 0; x < w.framebuffer().width(); ++x)
        for (int y = 3; y < 43; ++y) {
            Color c = w.framebuffer().colorAt(x, y);
            if (c.g > 150 && c.r < 100 && c.b < 100) {
                greenLeft = std::min(greenLeft, x);
                greenRight = std::max(greenRight, x);
            }
        }
    CHECK(greenRight > greenLeft && std::abs((greenLeft + greenRight) / 2 - (3 + 200)) <= 3);
    savePng(outDir() + "/frame-styled.png", w.framebuffer());
    theme::setSkin();
    app.repaintAll();
    app.tick();
    CHECK(w.frameInsets() == f && w.width() == 400 && w.height() == 300);

    // --- Options → Window frames → The system's: no frame (the inside keeps its size), and back ---
    app.setOwnFrames(false);
    CHECK(!w.isFramed() && !w.titleBar() && w.frameInsets() == Window::FrameInsets() && !w.hasState("framed"));
    CHECK(w.width() == 400 && w.height() == 300 && top->width() == 400);
    app.tick();
    CHECK(w.framebuffer().width() == 400 && w.framebuffer().height() == 300);
    CHECK(w.resizeEdgeAt(Point(0, 150)) == 0);
    {
        Window later(o);  // (made while they are off: none)
        CHECK(!later.isFramed());
    }
    app.setOwnFrames(true);
    CHECK(w.isFramed() && w.titleBar() && w.frameInsets() == f && w.width() == 400 && w.height() == 300);
    app.tick();
    CHECK(w.framebuffer().width() == 402 && w.framebuffer().height() == 302 + bar);

    w.hideWindow();
    app.frameAction.disconnect(conn);
    app.setFramesWhenHeadless(false);
    CHECK(!w.isFramed() && !app.framesDrawn());
}

// url() images: background-image layers (tiled, sized, at 150 %), -tiny-glyph on a check box and a scroll
// arrow (tinted, moved when pressed), and font-smooth: never (only full-on and full-off pixels); from tiny files
// in a scratch folder
static void testCssImages() {
    const std::string dir = "build/css-images";
    util::makeDirs(dir);
    const Color canvas(0xd4, 0xd0, 0xc8);
    // the files: a 2x2 checkerboard, a 4x2 PNG red on the left and blue on the right, an 8x8 SVG with a green 4x4
    // square in its middle, a 7x7 black SVG check, a 3x2 red PNG arrow
    Image checker(2, 2, Color(255, 255, 255));
    checker.setPixel(0, 0, Color(0, 0, 0).premul());
    checker.setPixel(1, 1, Color(0, 0, 0).premul());
    CHECK(savePng(dir + "/checker.png", checker));
    Image rb(4, 2, Color(255, 0, 0));
    rb.fill(Rect(2, 0, 2, 2), Color(0, 0, 255));
    CHECK(savePng(dir + "/rb.png", rb));
    CHECK(util::writeFile(dir + "/sq.svg", "<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8' viewBox='0 0 8 8'>"
                                           "<rect x='2' y='2' width='4' height='4' fill='#00ff00'/></svg>"));
    CHECK(util::writeFile(dir + "/check.svg", "<svg xmlns='http://www.w3.org/2000/svg' width='7' height='7'>"
                                              "<rect width='7' height='7' fill='#000000'/></svg>"));
    CHECK(savePng(dir + "/arrow.png", Image(3, 2, Color(255, 0, 0))));
    CHECK(util::writeFile(dir + "/broken.png", "\x89PNG\r\n\x1a\n not really"));

    css::ParseOptions o;
    o.baseDir = dir;
    o.checkImage = [](const std::string& path, std::string& why) { return style::imageFileOk(path, why); };
    std::vector<css::Problem> problems;
    css::SheetPtr sheet = css::parse(
        ".checker { background: url(checker.png); image-rendering: pixelated }\n"
        ".smooth { background: url(checker.png) }\n"
        ".svg { background: url(sq.svg) no-repeat }\n"
        ".contain { background: url(rb.png) no-repeat center / contain }\n"
        ".cover { background: url(rb.png) no-repeat center / cover }\n"
        ".pos { background: url(arrow.png) no-repeat right 2px bottom 1px; background-color: #ffffff }\n"
        ".space { background: url(arrow.png) space no-repeat; background-color: #ffffff }\n"
        ".broken { background: url(broken.png), url(missing.png) #123456 }\n",
        "images.css", problems, o);
    CHECK(problems.size() == 2 && problems[0].line == 8 && problems[0].column == 23 && problems[1].column == 40);
    if (problems.size() != 2)
        for (const css::Problem& p : problems) printf("  %s\n", p.text().c_str());
    css::Engine engine;
    engine.setSheets({sheet});
    std::vector<std::unique_ptr<css::Style>> kept;
    auto styleOf = [&](const char* cls) -> const css::Style& {
        css::SimpleNode n("Box");
        n.classes.push_back(cls);
        kept.push_back(std::make_unique<css::Style>(engine.compute(n)));
        return *kept.back();
    };
    auto isOnly = [](const Image& img, const Rect& r, std::initializer_list<Color> colours) {
        for (int y = r.y; y < r.bottom(); y++)
            for (int x = r.x; x < r.right(); x++) {
                bool any = false;
                for (Color c : colours) any = any || img.colorAt(x, y) == c;
                if (!any) return false;
            }
        return true;
    };

    // a 2x2 checkerboard tiled, pixel for pixel; at 150 %, pixelated: only black and white, 3 x 3 device px a tile
    {
        Image img(40, 30, canvas);
        Painter p(img);
        style::box(p, Rect(5, 5, 21, 11), styleOf("checker"));
        bool exact = true;
        for (int y = 0; y < 11; y++)
            for (int x = 0; x < 21; x++) exact = exact && img.colorAt(5 + x, 5 + y) == ((x + y) % 2 ? Color(255, 255, 255) : Color(0, 0, 0));
        CHECK(exact);
        CHECK(img.colorAt(4, 5) == canvas && img.colorAt(26, 15) == canvas && img.colorAt(5, 16) == canvas);
        Image big(60, 45, canvas);
        Painter q(big, 1.5f);
        style::box(q, Rect(4, 4, 20, 10), styleOf("checker"));
        CHECK(isOnly(big, Rect(6, 6, 30, 15), {Color(0, 0, 0), Color(255, 255, 255)}));
        // (each source pixel to the device pixels whose centres fall in it: a tile's columns black, white, white)
        CHECK(big.colorAt(6, 6) == Color(0, 0, 0) && big.colorAt(7, 6) == Color(255, 255, 255) &&
              big.colorAt(8, 6) == Color(255, 255, 255) && big.colorAt(9, 6) == Color(0, 0, 0) && big.colorAt(7, 7) == Color(0, 0, 0));
        Image soft(60, 45, canvas);
        Painter r(soft, 1.5f);
        style::box(r, Rect(4, 4, 20, 10), styleOf("smooth"));  // (smooth: greys where it is scaled)
        CHECK(!isOnly(soft, Rect(6, 6, 30, 15), {Color(0, 0, 0), Color(255, 255, 255)}));
        // drawn again: the image made once, kept
        int made = style::imagesMade();
        style::box(q, Rect(4, 4, 20, 10), styleOf("checker"));
        style::box(p, Rect(5, 5, 21, 11), styleOf("checker"));
        CHECK(style::imagesMade() == made);
    }
    // an SVG layer at 150 %: drawn at device pixels (12 x 12), its square sharp at device 3…9 in the box
    {
        Image img(30, 30, canvas);
        Painter p(img, 1.5f);
        style::box(p, Rect(2, 2, 12, 12), styleOf("svg"));  // (device 3…21)
        CHECK(isOnly(img, Rect(6, 6, 6, 6), {Color(0, 255, 0)}));
        CHECK(img.colorAt(5, 6) == canvas && img.colorAt(6, 5) == canvas && img.colorAt(12, 11) == canvas &&
              img.colorAt(11, 12) == canvas && img.colorAt(18, 18) == canvas);
    }
    // background-size: contain (20 x 10 in the middle of a 20 x 20 box) and cover (40 x 20, its middle shown)
    {
        Image img(60, 30, canvas);
        Painter p(img);
        style::box(p, Rect(0, 0, 20, 20), styleOf("contain"));
        CHECK(rgbIs(img, 1, 6, 255, 0, 0, 8) && rgbIs(img, 18, 13, 0, 0, 255, 8));
        CHECK(img.colorAt(10, 4) == canvas && img.colorAt(10, 15) == canvas && img.colorAt(10, 5) != canvas &&
              img.colorAt(10, 14) != canvas);
        style::box(p, Rect(30, 0, 20, 20), styleOf("cover"));
        CHECK(rgbIs(img, 32, 1, 255, 0, 0, 8) && rgbIs(img, 32, 18, 255, 0, 0, 8) && rgbIs(img, 47, 10, 0, 0, 255, 8));
        CHECK(img.colorAt(30, 0) != canvas && img.colorAt(49, 19) != canvas);
        // right 2px bottom 1px: the 3x2 arrow at x 15…17, y 17…18 of a 20 x 20 box
        style::box(p, Rect(0, 0, 20, 20), styleOf("pos"));
        CHECK(isOnly(img, Rect(15, 17, 3, 2), {Color(255, 0, 0)}) && img.colorAt(14, 17) == Color(255, 255, 255) &&
              img.colorAt(18, 18) == Color(255, 255, 255) && img.colorAt(15, 16) == Color(255, 255, 255));
        // space: as many whole arrows as fit across 20 px (6: 18 px of them), the first and last at the edges, the
        // 2 px left between them (at 0, 3.4, 6.8…: on whole pixels 0, 3, 7, 10, 14, 17)
        style::box(p, Rect(30, 0, 20, 20), styleOf("space"));
        int red = 0;
        for (int x = 30; x < 50; x++) red += img.colorAt(x, 0) == Color(255, 0, 0);
        CHECK(red == 18 && img.colorAt(30, 0) == Color(255, 0, 0) && img.colorAt(49, 0) == Color(255, 0, 0) &&
              img.colorAt(36, 0) == Color(255, 255, 255) && img.colorAt(43, 0) == Color(255, 255, 255) &&
              img.colorAt(30, 2) == Color(255, 255, 255));
        // images that can't be drawn: left out, the colour drawn
        style::box(p, Rect(0, 0, 10, 10), styleOf("broken"));
        CHECK(img.colorAt(5, 5) == Color(0x12, 0x34, 0x56));
    }

    // -tiny-glyph: a check box's mark and a scroll bar's arrows from images; tinted; moved 1 px when pressed
    {
        theme::setSkin();
        Window w(plat::WindowOptions{});
        w.resizeWindow(200, 200);
        auto* cb = w.add<CheckBox>("Glyph");
        cb->setGeometry(10, 10, 100, 20);
        cb->setChecked(true);
        auto* bar = w.add<ScrollBar>(Orientation::Vertical);
        bar->setGeometry(150, 10, 16, 150);
        bar->setRange(0, 100);
        bar->setPageStep(10);
        problems.clear();
        css::SheetPtr glyphs = css::parse(
            "CheckBox::indicator { -tiny-glyph: url(check.svg) }\n"
            "ScrollBar::sub-line, ScrollBar::add-line { width: 16px; height: 16px; padding: 0; border: none;"
            " background-color: #c0c0c0; color: #00ff00; -tiny-glyph: url(arrow.png); -tiny-glyph-tint: color }\n"
            "ScrollBar::sub-line:pressed { -tiny-glyph-offset: 1px 1px }\n",
            "glyphs.css", problems, o);
        CHECK(problems.empty());
        theme::setStyles({theme::builtinSheet(), glyphs});
        w.showWindow();
        App::get().repaintAll();
        App::get().tick();
        const Image& fb = w.framebuffer();
        // the check: 7x7 black, centred in the indicator's inside
        const css::Style& is = cb->computedStyle("indicator");
        int iw = style::px(is, "width", 14), ih = style::px(is, "height", 14);
        Rect ind(10 + style::insets(cb->computedStyle(), "padding").left, 10 + (20 - ih) / 2, iw, ih);
        Rect mark = style::contents(ind, is);
        Rect check(mark.x + (mark.w - 7) / 2, mark.y + (mark.h - 7) / 2, 7, 7);
        CHECK(isOnly(fb, check, {Color(0, 0, 0)}));
        CHECK(fb.colorAt(check.x - 1, check.y + 3) != Color(0, 0, 0) && fb.colorAt(check.right(), check.y + 3) != Color(0, 0, 0));
        // the arrows: 3x2, in the part's colour (green, not the file's red), centred in each 16 x 16 button
        auto greenBox = [&](const Rect& r) {
            Rect found;
            int n = 0, red = 0;
            for (int y = r.y; y < r.bottom(); y++)
                for (int x = r.x; x < r.right(); x++) {
                    Color c = fb.colorAt(x, y);
                    red += c == Color(255, 0, 0);
                    if (c != Color(0, 255, 0)) continue;
                    found = n++ ? found.united(Rect(x, y, 1, 1)) : Rect(x, y, 1, 1);
                }
            return n == 6 && red == 0 ? found : Rect();
        };
        Rect up = bar->lineRect(-1), down = bar->lineRect(1);
        CHECK(up == Rect(0, 0, 16, 16) && down == Rect(0, 134, 16, 16));
        CHECK(greenBox(Rect(150, 10, 16, 16)) == Rect(150 + 6, 10 + 7, 3, 2));
        CHECK(greenBox(Rect(150, 144, 16, 16)) == Rect(150 + 6, 144 + 7, 3, 2));
        press(w, 158, 18);
        App::get().tick();
        CHECK(greenBox(Rect(150, 10, 16, 16)) == Rect(150 + 7, 10 + 8, 3, 2));  // (pressed: moved 1 px right and down)
        release(w, 158, 18);
        App::get().tick();
        CHECK(greenBox(Rect(150, 10, 16, 16)) == Rect(150 + 6, 10 + 7, 3, 2));
        // without -tiny-glyph: the drawn glyphs, as before
        theme::setSkin();
        App::get().repaintAll();
        App::get().tick();
        CHECK(greenBox(Rect(150, 10, 16, 16)).empty());
        w.hideWindow();
    }

    // font-smooth: never: each pixel of the text fully on or off (and at 150 %); without it, anti-aliased
    {
        problems.clear();
        css::SheetPtr text = css::parse(".aliased { font-smooth: never } .webkit { -webkit-font-smoothing: none }",
                                        "text.css", problems);
        engine.setSheets({text});
        const char* words = "Aliased text, 0123 &@%";
        for (const char* cls : {"aliased", "webkit"})
            for (float scale : {1.f, 1.5f}) {
                Font f = style::font(styleOf(cls), Font::ui());
                CHECK(f.aliased());
                Image img((int)(160 * scale), (int)(24 * scale), Color(255, 255, 255));
                Painter p(img, scale);
                p.drawText(RectF(2.3f, 0, 150, 24), AlignLeft | AlignVCenter, words, f, Color(0, 0, 0));
                int black = 0;
                bool twoTone = true;
                for (int y = 0; y < img.height(); y++)
                    for (int x = 0; x < img.width(); x++) {
                        Color c = img.colorAt(x, y);
                        black += c == Color(0, 0, 0);
                        twoTone = twoTone && (c == Color(0, 0, 0) || c == Color(255, 255, 255));
                    }
                CHECK(twoTone && black > 50);
            }
        Font smooth = style::font(styleOf("none"), Font::ui());
        CHECK(!smooth.aliased());
        Image img(160, 24, Color(255, 255, 255));
        Painter p(img);
        p.drawText(RectF(2, 0, 150, 24), AlignLeft | AlignVCenter, words, smooth, Color(0, 0, 0));
        CHECK(!isOnly(img, img.rect(), {Color(0, 0, 0), Color(255, 255, 255)}));
        // a widget's text, the font inherited from its window (Label, Button, the title bar… all draw through
        // style::font)
        theme::setSkin();
        problems.clear();
        css::SheetPtr crisp = css::parse("Window { font-smooth: never; color: #000000; background-color: #ffffff }\n"
                                         "Label { color: #000000; background-color: #ffffff }",
                                         "crisp.css", problems);
        theme::setStyles({theme::builtinSheet(), crisp});
        Window w(plat::WindowOptions{});
        w.resizeWindow(200, 40);
        auto* label = w.add<Label>(words);
        label->setGeometry(0, 0, 200, 40);
        w.showWindow();
        App::get().tick();
        CHECK(label->font().aliased());
        CHECK(isOnly(w.framebuffer(), Rect(0, 0, 200, 40), {Color(0, 0, 0), Color(255, 255, 255)}));
        w.hideWindow();
        theme::setSkin();
        // the interface font's size in px is kept exactly (the 90s skin's 11.5px: not 8.63pt, 11.5067px)
        for (auto& size : std::vector<std::pair<const char*, double>>{{"11.5px", 11.5}, {"11px", 11}, {"10pt", 40.0 / 3},
                                                                      {"9pt", 12}, {"13.3px", 13.3}}) {
            problems.clear();
            css::SheetPtr sized = css::parse(std::string(":root { font-size: ") + size.first + " }", "size.css", problems);
            theme::setStyles({theme::builtinSheet(), sized});
            // (px as given: exactly; pt: as the engine works them out, to its last bits)
            bool givenPx = std::string(size.first).find("px") != std::string::npos;
            CHECK(givenPx ? theme::fonts().uiPx == size.second : std::fabs(theme::fonts().uiPx - size.second) < 1e-12);
            CHECK(Font::ui().pixelSize() == (float)size.second && Font::mono().pixelSize() == (float)size.second);
            CHECK(std::fabs(theme::fonts().uiSize - size.second * 0.75) < 1e-9);
            if (std::fabs(theme::fonts().uiPx - size.second) > 1e-12 || Font::ui().pixelSize() != (float)size.second)
                printf("  font-size %s: %.6f px (theme), %.6f px (Font::ui)\n", size.first, theme::fonts().uiPx,
                       Font::ui().pixelSize());
        }
        CHECK(theme::fonts().uiSize == 9.975);  // (13.3px)
        theme::setSkin();
        CHECK(Font::ui().pixelSize() == 12.f);
    }

    // timings: a 200 x 120 box tiled with the checkerboard; a scroll arrow's image
    {
        Image img(220, 140, canvas);
        Painter p(img);
        engine.setSheets({sheet});
        const css::Style& tiled = styleOf("checker");
        style::box(p, Rect(10, 10, 200, 120), tiled);
        auto t0 = std::chrono::steady_clock::now();
        const int n = 500;
        for (int i = 0; i < n; i++) style::box(p, Rect(10, 10, 200, 120), tiled);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
        Image big(330, 210, canvas);
        Painter q(big, 1.5f);
        style::box(q, Rect(10, 10, 200, 120), tiled);
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; i++) style::box(q, Rect(10, 10, 200, 120), tiled);
        double ms15 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
        printf("css images: a 200x120 tiled box %.3f ms (%.3f ms at 150 %%)\n", ms, ms15);
        CHECK(ms < 5 && ms15 < 10);
    }
    style::clearImageCaches();
}

// ------------------------------------------------------------------------------------------
// transparency (App::setTransparency): see-through windows, headless (setAlphaWhenHeadless: the framebuffer as a
// compositing manager would show it), and native windows' requests through a stub (App::nativeWindowFactory)
namespace {
struct StubNative : plat::Window {
    static std::vector<StubNative*>& all() {
        static std::vector<StubNative*> v;
        return v;
    }
    plat::WindowOptions opts;
    Rect geom;
    bool shown = false, alpha = true, blur = false;
    float opacity = 1;
    Rect input, blurArea;
    int radii[4] = {0, 0, 0, 0};
    int opacityCalls = 0;
    explicit StubNative(const plat::WindowOptions& o) : opts(o), geom(std::max(0, o.x), std::max(0, o.y), o.w, o.h) {
        all().push_back(this);
    }
    ~StubNative() override { all().erase(std::find(all().begin(), all().end(), this)); }
    void show() override { shown = true; }
    void hide() override { shown = false; }
    bool isVisible() const override { return shown; }
    void setTitle(const std::string&) override {}
    void resize(int w, int h) override { geom.w = w, geom.h = h; }
    void move(int x, int y) override { geom.x = x, geom.y = y; }
    void setMinimumSize(int, int) override {}
    void setResizable(bool) override {}
    Rect geometry() const override { return geom; }
    Rect frameGeometry() const override { return geom; }
    void raise() override {}
    void setMaximized(bool) override {}
    bool isMaximized() const override { return false; }
    void setFullScreen(bool) override {}
    bool hasFocus() const override { return false; }
    void setCursor(plat::Cursor) override {}
    void present(const Image&, const Rect&) override {}
    void grabPointer(bool) override {}
    uintptr_t nativeHandle() const override { return 0; }
    void* nativeDisplay() const override { return nullptr; }
    bool capture(Image&) override { return false; }
    // (as the platform: see-through only while transparency is on)
    bool hasAlpha() const override { return alpha && App::get().transparency(); }
    void setInputArea(const Rect& r) override { input = r; }
    void setOpacity(float o) override {
        opacity = o;
        opacityCalls++;
    }
    void setBlurBehind(bool on, const Rect& r) override {
        blur = on;
        blurArea = r;
    }
    void setCornerRadii(int tl, int tr, int br, int bl) override {
        radii[0] = tl, radii[1] = tr, radii[2] = br, radii[3] = bl;
    }
};
StubNative* stubOf(Window& w) { return dynamic_cast<StubNative*>(w.native()); }
// every pixel a valid premultiplied colour (no channel above its alpha): what composites correctly
bool premultiplied(const Image& img) {
    for (int y = 0; y < img.height(); y++)
        for (int x = 0; x < img.width(); x++) {
            uint32_t p = img.pixel(x, y), a = p >> 24;
            if (((p >> 16) & 255) > a || ((p >> 8) & 255) > a || (p & 255) > a) return false;
        }
    return true;
}
}  // namespace

// an SVG background with preserveAspectRatio="none" stretched to background-size: 2px 100% (each tile 2 px across,
// the box's height down); without it, the drawing keeps its proportions (centred in the tile); an SVG gradient in it
static void testCssSvgStretch() {
    const std::string dir = "build/css-svg-stretch";
    util::makeDirs(dir);
    const Color canvas(0xd4, 0xd0, 0xc8);
    // 2 x 10 view units: black on top, white below
    const std::string body = "viewBox='0 0 2 10'><rect width='2' height='5' fill='#000000'/>"
                             "<rect y='5' width='2' height='5' fill='#ffffff'/></svg>";
    CHECK(util::writeFile(dir + "/stretch.svg", "<svg xmlns='http://www.w3.org/2000/svg' preserveAspectRatio='none' " + body));
    CHECK(util::writeFile(dir + "/keep.svg", "<svg xmlns='http://www.w3.org/2000/svg' " + body));
    CHECK(util::writeFile(dir + "/grad.svg",
                          "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 1 1' preserveAspectRatio='none'>"
                          "<linearGradient id='g' x2='0' y2='1'><stop offset='0' stop-color='#ff0000'/>"
                          "<stop offset='1' stop-color='#0000ff'/></linearGradient>"
                          "<rect width='1' height='1' fill='url(#g)'/></svg>"));
    css::ParseOptions o;
    o.baseDir = dir;
    o.checkImage = [](const std::string& path, std::string& why) { return style::imageFileOk(path, why); };
    std::vector<css::Problem> problems;
    css::SheetPtr sheet = css::parse(".stretch { background: url(stretch.svg) repeat-x; background-size: 2px 100% }\n"
                                     ".keep { background: url(keep.svg) repeat-x; background-size: 2px 100% }\n"
                                     ".grad { background: url(grad.svg) no-repeat; background-size: 100% 100% }\n",
                                     "stretch.css", problems, o);
    CHECK(problems.empty());
    css::Engine engine;
    engine.setSheets({sheet});
    auto styleOf = [&](const char* cls) {
        css::SimpleNode n("Box");
        n.classes.push_back(cls);
        return engine.compute(n);
    };
    for (float scale : {1.f, 1.5f}) {
        int W = (int)(10 * scale), H = (int)(40 * scale);
        Image img(80, 80, canvas);
        Painter p(img, scale);
        css::Style s = styleOf("stretch");
        style::box(p, Rect(0, 0, 10, 40), s);
        bool ok = true;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                ok = ok && img.colorAt(x, y) == (y < H / 2 ? Color(0, 0, 0) : Color(255, 255, 255));
        CHECK(ok);
        // kept proportions: 2 x 10 in a 2 x 40 tile, in its middle; the canvas above and below
        Image kept(80, 80, canvas);
        Painter q(kept, scale);
        css::Style k = styleOf("keep");
        style::box(q, Rect(0, 0, 10, 40), k);
        CHECK(kept.colorAt(1, 0) == canvas && kept.colorAt(1, H - 1) == canvas &&
              kept.colorAt(1, H / 2 - 2) == Color(0, 0, 0) && kept.colorAt(1, H / 2 + 2) == Color(255, 255, 255));
    }
    // a gradient stretched to a 30 x 20 box: red at the top, blue at the bottom, the same across
    Image g(40, 30, canvas);
    Painter p(g);
    css::Style gs = styleOf("grad");
    style::box(p, Rect(0, 0, 30, 20), gs);
    CHECK(g.colorAt(0, 0).r > 240 && g.colorAt(0, 0).b < 15 && g.colorAt(29, 19).b > 240 && g.colorAt(29, 19).r < 15 &&
          g.pixel(0, 10) == g.pixel(29, 10) && g.colorAt(30, 10) == canvas);
}

// calc() of a percentage and a length, worked out against the box where it is drawn (and its px scaled with the
// interface): background-size, background-position, gradient colour stops, a radial gradient's centre and size;
// written out and through var()
static void testCssCalcMixed() {
    const std::string dir = "build/css-calc";
    util::makeDirs(dir);
    const Color canvas(0xd4, 0xd0, 0xc8), red(255, 0, 0), white(255, 255, 255), blue(0, 0, 255);
    CHECK(savePng(dir + "/red.png", Image(2, 2, red)));
    css::ParseOptions o;
    o.baseDir = dir;
    o.checkImage = [](const std::string& path, std::string& why) { return style::imageFileOk(path, why); };
    std::vector<css::Problem> problems;
    css::SheetPtr sheet = css::parse(
        ":root { --w: calc(100% - 8px); --at: calc(100% - 3px) calc(100% - 2px); --edge: calc(100% - 1px);"
        " --size: calc(100% - 8px) calc(50% + 1px); }\n"
        "Box { background-color: #ffffff; }\n"
        ".size { background: url(red.png) no-repeat 0 0 / calc(100% - 8px) calc(50% + 1px) #ffffff }\n"
        ".size-var { background-image: url(red.png); background-repeat: no-repeat; background-size: var(--size) }\n"
        ".width-var { background: url(red.png) no-repeat 0 0 / var(--w) 100% #ffffff }\n"
        ".pos { background: url(red.png) no-repeat calc(100% - 3px) calc(100% - 2px) / 2px 2px #ffffff }\n"
        ".pos-var { background: url(red.png) no-repeat var(--at) / 2px 2px #ffffff }\n"
        ".pos-edge { background: url(red.png) no-repeat right calc(10% + 1px) top 0 / 2px 2px #ffffff }\n"
        ".stops { background: linear-gradient(to right, #ff0000 calc(100% - 1px), #0000ff calc(100% - 1px)) }\n"
        ".stops-var { background-image: linear-gradient(to right, #ff0000 var(--edge), #0000ff var(--edge)) }\n"
        ".radial { background: radial-gradient(ellipse calc(50% - 6px) calc(50% + 0px) at calc(100% - 4px) 50%,"
        " #ff0000 99%, #ffffff 99%) }\n",
        "calc.css", problems, o);
    CHECK(problems.empty());
    for (const css::Problem& p : problems) printf("  %s\n", p.text().c_str());
    css::Engine engine;
    engine.setSheets({sheet});
    engine.checkVarUses();
    CHECK(engine.takeProblems().empty());
    auto styleOf = [&](const char* cls) {
        css::SimpleNode n("Box");
        n.classes.push_back(cls);
        return engine.compute(n);
    };
    // where a colour is in the box drawn at (0, 0), 20 x 10 logical px, at `scale`: its device px
    auto draw = [&](const char* cls, float scale) {
        Image img(40, 30, canvas);
        Painter p(img, scale);
        css::Style s = styleOf(cls);
        style::box(p, Rect(0, 0, 20, 10), s);
        return img;
    };
    auto isRect = [](const Image& img, const Rect& r, Color in, Color out, int W, int H) {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (img.colorAt(x, y) != (r.contains(Point(x, y)) ? in : out)) return false;
        return true;
    };
    CHECK(engine.takeProblems().empty());
    for (float scale : {1.f, 1.5f}) {
        int W = (int)(20 * scale), H = (int)(10 * scale);
        int px8 = (int)(8 * scale), px2 = (int)std::lround(2 * scale);
        // calc(100% - 8px) x calc(50% + 1px): 12 x 6 at 100 %
        Rect size(0, 0, W - px8, (int)std::lround(H * 0.5 + scale));
        CHECK(isRect(draw("size", scale), size, red, white, W, H));
        CHECK(isRect(draw("size-var", scale), size, red, white, W, H));
        CHECK(isRect(draw("width-var", scale), Rect(0, 0, W - px8, H), red, white, W, H));
        // at calc(100% - 3px) calc(100% - 2px): of (the box - the image), less 3 and 2 px
        Rect at((int)std::lround(W - px2 - 3 * scale), (int)std::lround(H - px2 - 2 * scale), px2, px2);
        CHECK(isRect(draw("pos", scale), at, red, white, W, H));
        CHECK(isRect(draw("pos-var", scale), at, red, white, W, H));
        // right calc(10% + 1px): 90 % of (the box - the image), less 1 px
        Rect edge((int)std::lround((W - px2) * 0.9 - scale), 0, px2, px2);
        CHECK(isRect(draw("pos-edge", scale), edge, red, white, W, H));
        // a hard stop 1 px before the right edge: red up to there, the last px blue
        for (const char* cls : {"stops", "stops-var"}) {
            Image img = draw(cls, scale);
            int last = W - (int)std::lround(scale);
            CHECK(img.colorAt(0, 1) == red && img.colorAt(last - 1, 1) == red && img.colorAt(W - 1, 1) == blue &&
                  img.colorAt(W, 1) == canvas);
        }
    }
    // the ellipse: centred 4 px from the right edge, 4 px across (50% - 6px of 20) and 5 px down from the middle
    Image r = draw("radial", 1);
    CHECK(r.colorAt(16, 5) == red && r.colorAt(13, 5) == red && r.colorAt(11, 5) == white && r.colorAt(16, 1) == red &&
          r.colorAt(5, 5) == white);
    CHECK(engine.takeProblems().empty());
}

static void testTransparency() {
    App& app = App::get();
    std::vector<css::Problem> problems;
    // backdrop-filter: none or blur(<length>) only (others reported and left out)
    css::parse("A { backdrop-filter: blur(1em); } B { backdrop-filter: none; } C { backdrop-filter: blur(); }",
               "blur.css", problems);
    CHECK(problems.empty());
    css::parse("D { backdrop-filter: brightness(2); } E { backdrop-filter: blur(-3px); } F { backdrop-filter: blur(4px) "
               "blur(2px); }",
               "blur2.css", problems);
    CHECK(problems.size() == 3);
    problems.clear();
    {
        theme::setStyles({theme::builtinSheet(), css::parse("Label#b { backdrop-filter: blur(1.5em); font-size: 10px; }",
                                                            "blur3.css", problems)});
        Window w(plat::WindowOptions{});
        auto* l = w.add<Label>("x");
        l->objectName = "b";
        CHECK(std::abs(l->computedStyle().backdropBlur() - 15) < 1e-9 && w.computedStyle().backdropBlur() == 0);
    }
    // --- headless, see-through: a framed window with rounded corners, a half see-through blue background and a
    // square red title bar (cut round all the same) ---
    theme::setStyles({theme::builtinSheet(),
                      css::parse("Window:framed:transparent { border: 0; border-radius: 10px; "
                                 "background-color: rgb(0 0 255 / 50%); } TitleBar, TitleBar:inactive { background-color: #ff0000; "
                                 "border-radius: 0; } Label { color: #ffffff; }",
                                 "glass.css", problems)});
    CHECK(problems.empty());
    app.setFramesWhenHeadless(true);
    app.setAlphaWhenHeadless(true);
    {
        plat::WindowOptions o;
        o.w = 200;
        o.h = 120;
        Window w(o);
        auto* text = w.add<Label>("Glass text");
        text->setGeometry(10, 40, 120, 20);
        w.showWindow();
        app.tick();
        CHECK(w.isFramed() && w.hasAlpha() && w.hasState("transparent"));
        const Image& fb = w.framebuffer();
        int bar = TitleBar::barHeight();
        CHECK(fb.width() == 200 && fb.height() == 120 + bar);
        // outside the rounded corners: nothing (the red title bar's square corner taken off), every corner
        CHECK(fb.pixel(0, 0) == 0 && fb.pixel(1, 1) == 0 && fb.pixel(199, 0) == 0 && fb.pixel(0, fb.height() - 1) == 0 &&
              fb.pixel(199, fb.height() - 1) == 0);
        // on the arc: anti-aliased (some pixels partly covered, the red kept red)
        int partial = 0;
        for (int y = 0; y < 10; y++)
            for (int x = 0; x < 10; x++) {
                uint32_t a = fb.pixel(x, y) >> 24;
                if (a > 0 && a < 255) {
                    partial++;
                    CHECK(fb.colorAt(x, y).r > 240 && fb.colorAt(x, y).g < 10);
                }
            }
        CHECK(partial >= 5);
        // the title bar opaque red; the inside the half see-through blue (cleared, not over black)
        CHECK((fb.pixel(100, bar / 2) >> 24) == 255 && fb.colorAt(100, bar / 2) == Color(255, 0, 0));
        uint32_t in = fb.pixel(180, bar + 100);
        CHECK(std::abs((int)(in >> 24) - 128) <= 1 && fb.colorAt(180, bar + 100).b > 250 && fb.colorAt(180, bar + 100).r < 5);
        // the text on it: grayscale anti-aliased, composited (premultiplied, some of it more opaque than the glass)
        CHECK(premultiplied(fb));
        int brighter = 0;
        for (int y = bar + 40; y < bar + 60; y++)
            for (int x = 10; x < 130; x++)
                if ((fb.pixel(x, y) >> 24) > 140) brighter++;
        CHECK(brighter > 20);
        savePng(outDir() + "/transparent-window.png", fb);
        // repainted in part: still cleared to see-through there (not stacked up)
        w.update(Rect(150, 80, 40, 30));
        app.tick();
        CHECK(fb.pixel(180, bar + 100) == in);
        // transparency off: opaque as before (over black; the corners are the native shape's)
        app.setTransparency(false);
        app.tick();
        CHECK(!w.hasAlpha() && !w.hasState("transparent"));
        CHECK((w.framebuffer().pixel(180, bar + 100) >> 24) == 255);
        app.setTransparency(true);
        app.tick();
        CHECK(w.hasAlpha() && w.framebuffer().pixel(1, 1) == 0);
        w.hideWindow();
    }
    app.setAlphaWhenHeadless(false);
    app.setFramesWhenHeadless(false);

    // --- native windows (a stub): a popup's room for its shadow and where clicks reach, opacity, blur ---
    theme::setStyles({theme::builtinSheet(),
                      css::parse("Window#pop:transparent { box-shadow: 0 4px 6px #000000; background-color: #ffffff; }"
                                 "Window#faded { opacity: 0.5; } Window#frosted { backdrop-filter: blur(12px); }",
                                 "stub.css", problems)});
    CHECK(problems.empty());
    app.nativeWindowFactory = [](const plat::WindowOptions& o, plat::WindowListener*) {
        return std::unique_ptr<plat::Window>(new StubNative(o));
    };
    {
        Window owner(plat::WindowOptions{});
        owner.showWindow();
        // a popup (as a menu, a drop-down list or a tooltip is), its own style's shadow below it
        plat::WindowOptions po;
        po.popup = true;
        po.takesFocus = false;
        po.w = 120;
        po.h = 40;
        Window pop(po);
        pop.objectName = "pop";
        Window* tip = &pop;
        pop.moveWindow(302, 318);
        app.openPopup(&pop, &owner, [&] { app.closePopup(&pop); });
        CHECK(stubOf(pop) && pop.hasAlpha() && pop.hasState("transparent"));
        app.tick();
        StubNative* sn = stubOf(*tip);
        Window::FrameInsets s = tip->nativeInsets();
        // the shadow reaches 6 px around, 4 px lower: more room below than above, none taken from the inside
        CHECK(s.left > 0 && s.right == s.left && s.bottom > s.top && tip->frameInsets() == Window::FrameInsets());
        Rect g = sn->geometry(), inside = tip->screenGeometry();
        CHECK(g.w == tip->width() + s.left + s.right && g.h == tip->height() + s.top + s.bottom);
        CHECK(inside.x == g.x + s.left && inside.y == g.y + s.top && inside.x == 302 && inside.y == 318);
        // clicks only on the tooltip itself; the shadow drawn in the room around it, nothing in its far corner
        CHECK(sn->input == Rect(s.left, s.top, tip->width(), tip->height()));
        const Image& fb = tip->framebuffer();
        CHECK(fb.width() == g.w && fb.height() == g.h);
        CHECK((fb.pixel(fb.width() / 2, fb.height() - s.bottom + 3) >> 24) > 20 && fb.pixel(0, 0) >> 24 < 8);
        CHECK(premultiplied(fb));
        savePng(outDir() + "/transparent-popup.png", fb);
        // a press in the shadow (the platform's grab sends it to the popup): outside it, so the popup closes
        plat::Event pe = mouseEv(plat::Event::MouseDown, fb.width() / 2, fb.height() - 2, plat::LeftButton, 0);
        pop.onEvent(nullptr, pe);
        CHECK(app.popups().empty() && !pop.isWindowShown());
        app.openPopup(&pop, &owner, [&] { app.closePopup(&pop); });
        app.tick();
        // transparency off: no room, clicks everywhere (the stub opaque); the popup stays where it was put
        app.setTransparency(false);
        app.tick();
        CHECK(tip->nativeInsets() == Window::FrameInsets() && sn->input.empty() && !tip->hasState("transparent"));
        CHECK(sn->geometry() == Rect(302, 318, tip->width(), tip->height()));
        app.setTransparency(true);
        app.tick();
        CHECK(tip->nativeInsets() == s && sn->geometry() == g);
        app.closePopup(&pop);

        // opacity: the native window's, while transparency is on
        Window faded(plat::WindowOptions{});
        faded.objectName = "faded";
        faded.showWindow();
        app.tick();
        CHECK(stubOf(faded)->opacity == 0.5f);
        app.setTransparency(false);
        app.tick();
        CHECK(stubOf(faded)->opacity == 1.f);
        app.setTransparency(true);
        app.tick();
        CHECK(stubOf(faded)->opacity == 0.5f && stubOf(owner)->opacityCalls == 0);
        // backdrop-filter: blur(): asked for the whole window; not for others, nor while opaque
        Window frosted(plat::WindowOptions{});
        frosted.objectName = "frosted";
        frosted.showWindow();
        app.tick();
        CHECK(stubOf(frosted)->blur && stubOf(frosted)->blurArea.empty() && !stubOf(owner)->blur);
        stubOf(frosted)->alpha = false;
        frosted.update();
        app.tick();
        CHECK(!stubOf(frosted)->blur);
    }
    app.nativeWindowFactory = nullptr;
    theme::setSkin();
}

// ------------------------------------------------------------------------------------------
// What classic skins draw with (each only when a stylesheet asks: default.css draws as before): spin buttons as two
// boxes of their own, a combo box's arrow inside its border, tabs with margins (the selected one larger, over its
// neighbours and joined to the page), the title bar's buttons placed by their vertical margins with :first / :last,
// a framed window's ::frame ring, a tree's dotted lines and a selection of its text only, a check box's focus
// rectangle (outline)
namespace {
// a sheet over default.css (none of its rules left out)
void classicSheet(const std::string& text) {
    std::vector<css::Problem> problems;
    css::SheetPtr extra = css::parse(text, "classic-gaps.css", problems);
    CHECK(problems.empty());
    for (auto& pr : problems) printf("  problem: %s\n", pr.message.c_str());
    theme::setStyles({theme::builtinSheet(), extra});
    App::get().repaintAll();
}
int countColour(const Image& fb, const Rect& r, Color c) {
    int n = 0;
    for (int y = r.y; y < r.bottom(); ++y)
        for (int x = r.x; x < r.right(); ++x)
            if (fb.colorAt(x, y) == c) ++n;
    return n;
}
}  // namespace

static void testSpinButtonBoxes() {
    theme::setSkin();
    Window w(plat::WindowOptions{});
    w.resizeWindow(200, 60);
    auto* s = w.add<SpinBox>();
    s->setGeometry(10, 10, 80, 24);
    s->setRange(0, 10);
    s->setValue(5);
    w.showWindow();
    App::get().tick();
    Point hint = s->sizeHint();
    // apart: each button its half of the column (16 px) less its margin, a red border each, a 2 px gap between
    classicSheet("SpinBox { border: 2px solid #000000; border-radius: 0; background-color: #ffffff; }"
                 "SpinBox::up-button, SpinBox::down-button { width: 16px; margin: 0; border: 1px solid #ff0000;"
                 " border-radius: 0; background-color: #c0c0c0; color: #000000; }"
                 "SpinBox::up-button { margin-bottom: 1px; } SpinBox::down-button { margin-top: 1px; }");
    App::get().tick();
    const Image& fb = w.framebuffer();
    // the inside of the border: (2, 2) 76 x 20; the column x 62..77, the up half y 2..11, the down 12..21: the up
    // button y 2..10, the down 13..21 (the spin box at (10, 10))
    Color red(255, 0, 0), grey(192, 192, 192), white(255, 255, 255);
    auto at = [&](int x, int y) { return fb.colorAt(10 + x, 10 + y); };
    CHECK(at(62, 2) == red && at(77, 2) == red && at(62, 10) == red && at(77, 10) == red);  // up: its corners
    CHECK(at(63, 3) == grey && at(76, 9) == grey);
    CHECK(at(70, 11) == white && at(70, 12) == white);  // the gap: the spin box's own background
    CHECK(at(62, 13) == red && at(77, 21) == red && at(63, 14) == grey && at(76, 20) == grey);  // down
    CHECK(at(61, 5) != red && at(70, 22) == Color(0, 0, 0));  // (beside it: the field; below: the border)
    CHECK(s->sizeHint().x == hint.x + 2);  // (the column's width as before; the border 1 px wider each side)
    // the arrows in each box's middle (black pixels in both)
    CHECK(countColour(fb, Rect(10 + 63, 10 + 3, 14, 7), Color(0, 0, 0)) > 0);
    CHECK(countColour(fb, Rect(10 + 63, 10 + 14, 14, 7), Color(0, 0, 0)) > 0);
    // a press on each half still steps it
    click(w, 10 + 70, 10 + 6);
    CHECK(s->value() == 6);
    click(w, 10 + 70, 10 + 17);
    CHECK(s->value() == 5);
    savePng(outDir() + "/gaps-spin.png", fb);
    w.hideWindow();
    theme::setSkin();
}

static void testComboArrowMargin() {
    theme::setSkin();
    Window w(plat::WindowOptions{});
    w.resizeWindow(200, 60);
    auto* c = w.add<ComboBox>();
    c->addItem("One");
    c->setGeometry(10, 10, 120, 24);
    w.showWindow();
    App::get().tick();
    classicSheet("ComboBox { border: 2px solid #000000; border-radius: 0; background-color: #ffffff; }"
                 "ComboBox::arrow { width: 16px; background-color: #c0c0c0; border-radius: 0; }");
    int plain = c->sizeHint().x;
    classicSheet("ComboBox { border: 2px solid #000000; border-radius: 0; background-color: #ffffff; }"
                 "ComboBox::arrow { width: 16px; margin: 2px 2px 2px 0; background-color: #c0c0c0; border-radius: 0;"
                 " color: #000000; }");
    CHECK(c->sizeHint().x == plain + 2);  // (its margin's room too)
    App::get().tick();
    const Image& fb = w.framebuffer();
    // the arrow's box: 16 px wide, 2 px in from the right, top and bottom: inside the border (x 102..117, y 2..21)
    Color grey(192, 192, 192), black(0, 0, 0);
    CHECK(fb.colorAt(10 + 119, 10 + 12) == black && fb.colorAt(10 + 118, 10 + 12) == black);  // the border shows
    CHECK(fb.colorAt(10 + 110, 10 + 1) == black && fb.colorAt(10 + 110, 10 + 22) == black);
    CHECK(fb.colorAt(10 + 102, 10 + 2) == grey && fb.colorAt(10 + 117, 10 + 21) == grey);
    CHECK(fb.colorAt(10 + 101, 10 + 12) == Color(255, 255, 255));
    CHECK(countColour(fb, Rect(10 + 102, 10 + 2, 16, 20), black) > 0);  // the ▾ in it
    savePng(outDir() + "/gaps-combo.png", fb);
    w.hideWindow();
    theme::setSkin();
}

static void testTabMarginsAndOverlap() {
    theme::setSkin();
    Window w(plat::WindowOptions{});
    w.resizeWindow(300, 200);
    auto* tabs = w.add<TabWidget>();
    tabs->addTab(std::make_unique<Widget>(), "One");
    Widget* second = tabs->addTab(std::make_unique<Widget>(), "Two");
    tabs->addTab(std::make_unique<Widget>(), "Three");
    tabs->setGeometry(0, 0, 300, 200);
    w.showWindow();
    App::get().tick();
    // default.css: side by side from the left, all as high as the bar
    int bh = tabs->barHeight();
    CHECK(tabs->tabRect(0).x == 0 && tabs->tabRect(1).x == tabs->tabRect(0).right() && tabs->tabRect(1).h == bh);
    classicSheet("TabWidget::tab { padding: 2px 6px; margin: 2px 0 1px; border: 0; border-radius: 0;"
                 " background-color: #c0c0c0; color: #000000; }"
                 "TabWidget::tab:hover { background-color: #c0c0c0; }"
                 "TabWidget::tab:first { margin-left: 2px; }"
                 "TabWidget::tab:selected { padding: 3px 8px 5px; margin: 0 -2px -1px; background-color: #ffffff; }"
                 "TabWidget::tab-line { height: 0; }"
                 "TabWidget::base { height: 1px; background-color: #000000; }");
    tabs->setCurrentIndex(1);
    App::get().tick();
    int th = style::textHeight(Font::ui());
    int t0 = style::textWidth(Font::ui(), "One"), t1 = style::textWidth(Font::ui(), "Two");
    int bar = th + 7;  // (2 + th + 4 + 1, or th + 8 - 1)
    CHECK(tabs->barHeight() == bar);
    Rect r0 = tabs->tabRect(0), r1 = tabs->tabRect(1), r2 = tabs->tabRect(2);
    CHECK(r0 == Rect(2, 2, t0 + 12, th + 4));
    CHECK(r1 == Rect(r0.right() - 2, 0, t1 + 16, th + 8));  // (over its neighbours by 2 px, 1 px below the bar)
    CHECK(r2.x == r0.right() + t1 + 12 && r2.y == 2);         // (as if it weren't larger)
    CHECK(second->geometry().y == bar);
    const Image& fb = w.framebuffer();
    Color white(255, 255, 255), grey(192, 192, 192), black(0, 0, 0);
    CHECK(fb.colorAt(r0.right() - 1, 3) == white && fb.colorAt(r0.right() - 3, 3) == grey);  // (drawn last)
    CHECK(fb.colorAt(r2.x + 1, 3) == white && fb.colorAt(r2.x + 2, 3) == grey);
    CHECK(fb.colorAt(r1.x + 4, bar - 1) == white && fb.colorAt(r2.x + 4, bar - 1) == black);  // joined to the page
    CHECK(fb.colorAt(1, 3) != grey && fb.colorAt(r1.x + 4, 0) == white && fb.colorAt(r0.x + 4, 0) != grey);
    // a click where the selected tab overlaps the first: the selected one; on the first's own part: the first
    click(w, r0.right() - 1, 5);
    CHECK(tabs->currentIndex() == 1);
    click(w, r0.x + 3, 5);
    CHECK(tabs->currentIndex() == 0);
    App::get().tick();
    // the first selected: the larger one now
    CHECK(tabs->tabRect(0).y == 0 && tabs->tabRect(1).y == 2 && tabs->barHeight() == bar);
    // hovering a tab doesn't move the page
    moveTo(w, tabs->tabRect(2).x + 3, 5);
    CHECK(second->geometry().y == bar);
    savePng(outDir() + "/gaps-tabs.png", fb);
    w.hideWindow();
    theme::setSkin();
}

static void testTitleButtonPlaces() {
    App& app = App::get();
    theme::setSkin();
    app.setFramesWhenHeadless(true);
    plat::WindowOptions o;
    o.title = "Placed";
    o.w = 300;
    o.h = 100;
    Window w(o);
    w.showWindow();
    TitleBar* tb = w.titleBar();
    CHECK(tb != nullptr);
    if (!tb) return;
    // default.css: centred in the bar, no :first / :last look
    Rect plain = tb->partRect(TitleBar::Close);
    CHECK(plain.y == 0 && plain.h == TitleBar::barHeight());
    classicSheet(":root { --title-bar-height: 30px; } TitleBar { padding: 0; }"
                 "TitleBar::minimize, TitleBar::maximize, TitleBar::close { width: 16px; height: 14px;"
                 " margin: 5px 0 9px 0; }"
                 "TitleBar::minimize:first, TitleBar::maximize:first, TitleBar::close:first { width: 20px; }"
                 "TitleBar::minimize:last, TitleBar::maximize:last, TitleBar::close:last { width: 24px; }");
    app.tick();
    // vertically: centred in what the margins leave (30 - 5 - 9 = 16: 1 px above the 14)
    Rect mn = tb->partRect(TitleBar::Minimize), mx = tb->partRect(TitleBar::Maximize), cl = tb->partRect(TitleBar::Close);
    CHECK(mn.y == 6 && mx.y == 6 && cl.y == 6 && cl.h == 14);
    // right: minimize first, close last
    CHECK(mn.w == 20 && mx.w == 16 && cl.w == 24 && cl.right() == 300 && mx.right() == cl.x && mn.right() == mx.x);
    // left: close first, minimize last
    classicSheet(":root { --title-bar-height: 30px; --title-buttons: left; } TitleBar { padding: 0; }"
                 "TitleBar::minimize, TitleBar::maximize, TitleBar::close { width: 16px; height: 14px; margin: 0 0 4px; }"
                 "TitleBar::minimize:first, TitleBar::maximize:first, TitleBar::close:first { width: 20px; }"
                 "TitleBar::minimize:last, TitleBar::maximize:last, TitleBar::close:last { width: 24px; }");
    mn = tb->partRect(TitleBar::Minimize), cl = tb->partRect(TitleBar::Close);
    CHECK(cl.x == 0 && cl.w == 20 && mn.w == 24 && cl.y == 6);  // ((30 - 4 - 14) / 2)
    // a window that can't be resized: no maximize (and so minimize is the last)
    w.setResizable(false);
    CHECK(!tb->hasButton(TitleBar::Maximize) && tb->partRect(TitleBar::Maximize).empty());
    CHECK(tb->partRect(TitleBar::Minimize).w == 24);
    // a modal one that can't be resized: only close, both first and last (the later rule: 24)
    w.setModal(true);
    CHECK(tb->partRect(TitleBar::Close).w == 24 && tb->partRect(TitleBar::Minimize).empty());
    TitleBar::Part shown[3];
    CHECK(tb->shownButtons(shown) == 1 && shown[0] == TitleBar::Close);
    w.setModal(false);
    w.hideWindow();
    // a message box can't be resized, so it has no maximize
    bool sawBox = false;
    app.singleShot(30, [&] {
        Window* top = app.modalWindow();
        if (!top) return;
        sawBox = true;
        CHECK(!top->isResizable() && top->isFramed() && top->titleBar());
        if (top->titleBar()) CHECK(!top->titleBar()->hasButton(TitleBar::Maximize));
        key(*top, plat::Key_Escape);
    });
    msg::information(&w, "Note", "Nothing to resize here.");
    CHECK(sawBox);
    theme::setSkin();
    app.setFramesWhenHeadless(false);
}

static void testWindowFrameRing() {
    App& app = App::get();
    theme::setSkin();
    app.setFramesWhenHeadless(true);
    plat::WindowOptions o;
    o.title = "Ring";
    o.w = 200;
    o.h = 100;
    Window w(o);
    w.showWindow();
    int bar = TitleBar::barHeight();
    CHECK(w.frameInsets().left == 1 && w.frameInsets().top == 1 + bar);
    classicSheet("Window:framed { border: 2px solid #0000ff; background-color: #c0c0c0; }"
                 "Window:framed::frame { border: 1px solid #ff0000; padding: 2px; background-color: #00ff00; }"
                 "Window:framed:maximized::frame { border-width: 0; padding: 0; }");
    app.tick();
    Window::FrameInsets f = w.frameInsets();
    CHECK(f.left == 5 && f.right == 5 && f.bottom == 5 && f.top == 5 + bar);
    CHECK(w.width() == 200 && w.height() == 100 && w.contentsRect() == Rect(0, 0, 200, 100));  // (its padding as it was)
    CHECK(w.titleBar()->geometry() == Rect(0, -bar, 200, bar));
    const Image& fb = w.framebuffer();
    CHECK(fb.width() == 210 && fb.height() == 110 + bar);
    Color blue(0, 0, 255), red(255, 0, 0), green(0, 255, 0), grey(192, 192, 192);
    int midY = 5 + bar + 50;
    CHECK(fb.colorAt(0, midY) == blue && fb.colorAt(1, midY) == blue && fb.colorAt(2, midY) == red);
    CHECK(fb.colorAt(3, midY) == green && fb.colorAt(4, midY) == green && fb.colorAt(5, midY) == grey);  // the inside: the Window's
    CHECK(fb.colorAt(209, midY) == blue && fb.colorAt(207, midY) == red && fb.colorAt(205, midY) == green);
    CHECK(fb.colorAt(100, 2) == red && fb.colorAt(100, 4) == green && fb.colorAt(100, fb.height() - 4) == green);
    CHECK(fb.colorAt(100, fb.height() - 3) == red);
    CHECK(near(fb.colorAt(100, 5 + bar / 2), style::background(w.titleBar()->computedStyle()), 1));  // (the bar over it)
    savePng(outDir() + "/gaps-frame-ring.png", fb);
    // the resize band reaches over the ring
    CHECK(w.resizeEdgeAt(Point(-5, 50)) != 0);
    // maximized: no ring (its own states)
    w.setWindowMaximized(true);
    CHECK(w.frameInsets().left == 0 && w.frameInsets().top == bar);
    w.setWindowMaximized(false);
    CHECK(w.frameInsets().left == 5);
    // :active restyles the window's own background (a synthetic FocusIn repaints it)
    // (the border's colour the same both ways: only the background changes)
    classicSheet("Window:framed { border-color: #000000; background-color: #c0c0c0; }"
                 "Window:framed:active { border-color: #000000; background-color: #ff00ff; }");
    app.tick();
    Window::FrameInsets g = w.frameInsets();
    CHECK(w.framebuffer().colorAt(g.left + 100, g.top + 50) == Color(192, 192, 192));
    plat::Event focus;
    focus.type = plat::Event::FocusIn;
    w.onEvent(nullptr, focus);
    app.tick();
    CHECK(w.framebuffer().colorAt(g.left + 100, g.top + 50) == Color(255, 0, 255));
    focus.type = plat::Event::FocusOut;
    w.onEvent(nullptr, focus);
    app.tick();
    CHECK(w.framebuffer().colorAt(g.left + 100, g.top + 50) == Color(192, 192, 192));
    // a ring on :active only: the insets follow the focus
    classicSheet("Window:framed:active::frame { padding: 3px; }");
    app.tick();
    CHECK(w.frameInsets().left == 1);
    focus.type = plat::Event::FocusIn;
    w.onEvent(nullptr, focus);
    CHECK(w.frameInsets().left == 4 && w.width() == 200 && w.height() == 100);
    w.hideWindow();
    theme::setSkin();
    app.setFramesWhenHeadless(false);
}

static void testTreeLinesAndTextSelection() {
    theme::setSkin();
    Window w(plat::WindowOptions{});
    w.resizeWindow(220, 120);
    auto* t = w.add<TreeWidget>();
    t->setGeometry(0, 0, 200, 100);
    TreeWidget::Node* a = t->root()->add("A");
    TreeWidget::Node* a1 = a->add("A1");
    a->add("A2");
    t->root()->add("B");
    a->filled = true;
    t->setExpanded(a, true);
    w.showWindow();
    App::get().tick();
    CHECK(t->rowCount() == 4);
    // default.css: no lines (the white of a classic sheet's lines' places stays the list's)
    classicSheet("TreeWidget { background-color: #ffffff; border: 0; padding: 0; }"
                 "TreeWidget::item { height: 16px; padding: 0; color: #000000; }"
                 "TreeWidget::item:hover { background-color: transparent; }"
                 "TreeWidget::item:selected { background-color: transparent; color: #000000; }"
                 "TreeWidget::text:selected { background-color: #000080; color: #ffffff; padding: 0 2px; }"
                 "TreeWidget::branch { width: 19px; padding: 0 5px; color: #000000;"
                 " -tiny-tree-lines: dotted; -tiny-tree-line-color: #808080; }");
    t->setCurrent(a1);
    App::get().tick();
    CHECK(t->indent == 19 && t->rowHeight == 16);
    const Image& fb = w.framebuffer();
    Color line(128, 128, 128), white(255, 255, 255), navy(0, 0, 128);
    // (each level's line through x = 9 of its indent; each row's middle at y = 7 of its 16; dots where x + y is even)
    // A1 (row 1, depth 1): its own line down (A2 follows) and across; A's passing by (B follows)
    CHECK(fb.colorAt(28, 18) == line && fb.colorAt(28, 19) == white && fb.colorAt(28, 30) == line);
    CHECK(fb.colorAt(31, 23) == line && fb.colorAt(30, 23) == white && fb.colorAt(37, 23) == line);
    CHECK(fb.colorAt(9, 17) == line && fb.colorAt(9, 18) == white);
    // A2 (row 2): the last child: its line stops at its middle
    CHECK(fb.colorAt(28, 34) == line && fb.colorAt(28, 38) == line && fb.colorAt(28, 44) == white);
    // B (row 3, the last): A's line comes down to its middle only
    CHECK(fb.colorAt(9, 49) == line && fb.colorAt(9, 55) == line && fb.colorAt(9, 61) == white);
    // the selection covers A1's text only (its padding: 2 px each side)
    int tw = style::textWidth(Font::ui(), "A1");
    CHECK(fb.colorAt(38, 20) == navy && fb.colorAt(38 + tw + 3, 20) == navy && fb.colorAt(38 + tw + 4, 20) == white);
    CHECK(fb.colorAt(150, 20) == white && countColour(fb, Rect(40, 16, tw, 16), white) > 0);  // (its text white)
    savePng(outDir() + "/gaps-tree.png", fb);
    // the focus rectangle around the text: its outline
    classicSheet("TreeWidget { background-color: #ffffff; } TreeWidget::item { height: 16px; padding: 0; }"
                 "TreeWidget::item:selected { background-color: transparent; }"
                 "TreeWidget::branch { width: 19px; padding: 0 5px; }"
                 "TreeWidget:focus::text:selected { outline: 1px solid #ff0000; }");
    App::get().tick();
    CHECK(countColour(fb, Rect(0, 0, 200, 100), Color(255, 0, 0)) == 0);
    t->setFocus();
    App::get().tick();
    CHECK(fb.colorAt(37, 15) == Color(255, 0, 0) && fb.colorAt(38 + tw, 32) == Color(255, 0, 0));
    w.hideWindow();
    theme::setSkin();
}

static void testCheckBoxFocusRectangle() {
    theme::setSkin();
    Window w(plat::WindowOptions{});
    w.resizeWindow(200, 60);
    auto* other = w.add<Button>("Other");
    other->setGeometry(10, 36, 60, 20);
    auto* cb = w.add<CheckBox>("Loop");
    cb->setGeometry(10, 10, 150, 22);
    w.showWindow();
    App::get().tick();
    Point plain = cb->sizeHint();
    classicSheet("CheckBox::label { padding: 0 1px; }"
                 "CheckBox::label:focus { outline: 1px dotted #ff0000; }");
    CHECK(cb->computedStyle("label").raw("outline-style").empty());
    CHECK(cb->sizeHint().x == plain.x + 2);
    App::get().tick();
    const Image& fb = w.framebuffer();
    Color red(255, 0, 0);
    CHECK(countColour(fb, Rect(10, 10, 150, 22), red) == 0);  // (not focused: none)
    cb->setFocus();
    App::get().tick();
    const css::Style& ls = cb->computedStyle("label");
    CHECK(ls.keyword("outline-style", "") == "dotted" && style::px(ls, "outline-width") == 1);
    // the outline: 1 px outside the label's box (the text and 1 px each side), every other pixel
    const css::Style& is = cb->computedStyle("indicator");
    int lx = style::px(is, "width", 14) + style::px(is, "margin-right", 6);
    int lw = style::textWidth(style::font(cb->computedStyle(), theme::uiFont()), "Loop") + 2;
    int lh = style::textHeight(style::font(cb->computedStyle(), theme::uiFont()));
    int ly = (22 - lh) / 2;
    int top = countColour(fb, Rect(10 + lx - 1, 10 + ly - 1, lw + 2, 1), red);
    CHECK(top >= (lw + 2) / 3 && top <= (lw + 2) / 2 + 2);  // (dotted: about half of them)
    CHECK(countColour(fb, Rect(10 + lx - 1, 10 + ly + lh, lw + 2, 1), red) >= (lw + 2) / 3);
    CHECK(countColour(fb, Rect(10 + lx - 1, 10 + ly, 1, lh), red) >= lh / 2 - 1);
    CHECK(countColour(fb, Rect(10 + lx, 10 + ly, lw, lh), red) == 0);  // (the inside: the text)
    savePng(outDir() + "/gaps-check-focus.png", fb);
    other->setFocus();
    App::get().tick();
    CHECK(countColour(fb, Rect(10, 10, 150, 22), red) == 0);
    // the outline shorthand: as a border's
    classicSheet("CheckBox::label:focus { outline: thin dashed; outline-offset: 2px; }");
    cb->setFocus();
    const css::Style& l2 = cb->computedStyle("label");
    CHECK(l2.keyword("outline-style", "") == "dashed" && l2.length("outline-width", 0) == 1 &&
          l2.raw("outline-color") == "currentcolor" && style::px(l2, "outline-offset") == 2);
    App::get().tick();
    w.hideWindow();
    theme::setSkin();
}

int main() {
    App::get().initHeadless();
    util::makeDirs("build/toolkit-samples");
    settings::setPath("build/toolkit-samples/settings.json");
    testSignalsAndKeys();
    testLayouts();
    testPaintingColours();
    testStylesheets();
    testFieldStylesheets();
    testBasicControlStyles();
    testRestyleRelayout();
    testSiblingStates();
    testShadowOverflow();
    testSeeThroughBorder();
    testClassicControlParts();
    testListAndMenuStylesheets();
    testButtonsAndClicks();
    testLineEdit();
    testSpinBox();
    testComboAndMenus();
    testDeleteLaterAndForget();
    testFocusAndToolTips();
    testPointerDrag();
    testScrollAndLists();
    testDialogs();
    testFileDialog();
    testFileDialogAndMessageIconStyles();
    testWindowFrames();
    gallery();
    testCssPainting();
    testCssImages();
    testCssSvgStretch();
    testCssCalcMixed();
    testTransparency();
    testSpinButtonBoxes();
    testComboArrowMargin();
    testTabMarginsAndOverlap();
    testTitleButtonPlaces();
    testWindowFrameRing();
    testTreeLinesAndTextSelection();
    testCheckBoxFocusRectangle();
    TEST_MAIN_END();
}
