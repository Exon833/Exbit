// The GUI toolkit, headless: widget trees, layouts, events through the windows' dispatch paths,
// painting into images (pixel checks), text editing, spin boxes, shortcuts, menus, dialogs,
// deleteLater safety. Also writes a gallery of the controls as PNGs to build/toolkit-samples/.
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
    gallery();
    testCssPainting();
    TEST_MAIN_END();
}
