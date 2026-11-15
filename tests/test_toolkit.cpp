// The GUI toolkit, headless: widget trees, layouts, events through the windows' dispatch paths,
// painting into images (pixel checks), text editing, spin boxes, shortcuts, menus, dialogs,
// deleteLater safety. Also writes a gallery of the controls as PNGs to build/toolkit-samples/.
#include <cstdio>
#include <string>

#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "ui/theme.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/menu.h"
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

static void testLayouts() {
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

int main() {
    App::get().initHeadless();
    util::makeDirs("build/toolkit-samples");
    settings::setPath("build/toolkit-samples/settings.json");
    testSignalsAndKeys();
    testLayouts();
    testPaintingColours();
    testButtonsAndClicks();
    testLineEdit();
    testSpinBox();
    testComboAndMenus();
    testDeleteLaterAndForget();
    testFocusAndToolTips();
    testScrollAndLists();
    testDialogs();
    testFileDialog();
    gallery();
    TEST_MAIN_END();
}
