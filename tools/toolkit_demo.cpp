// A window with every control of the toolkit, a menu bar, dialogs and the file chooser, to try
// them by hand (not built by default). Build (in cpp/):
//   g++ -std=c++17 -O1 -g -Isrc -pthread tools/toolkit_demo.cpp build/src/ui/toolkit/*.o
//       build/src/ui/theme.o build/src/gfx/*.o build/src/platform/*.o build/src/core/*.o
//       -o build/toolkit_demo -lX11 -lXext -lXrandr -lXcursor -lXi -pthread -lm
// Options: --seconds N (quit after N seconds), --shot file.png (a picture of the window before quitting)
#include <cstdio>
#include <cstdlib>
#include <string>

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
// takes files dragged onto it (from the desktop or the tree)
class DropBox : public Label {
public:
    DropBox() : Label("Drop files here") {
        setAcceptDrops(true);
        setAlignment(AlignCenter);
        setMinimumHeight(48);
        setProperty("hint", "true");
    }
    bool hot = false;
    void dragEnter(DragEvent& e) override {
        e.accepted = e.data && !e.data->files.empty();
        hot = e.accepted;
        update();
    }
    void dragLeave() override {
        hot = false;
        update();
    }
    void drop(DragEvent& e) override {
        hot = false;
        std::string t;
        for (auto& f : e.data->files) t += util::baseName(f) + "\n";
        setText(t.empty() ? "(nothing)" : t.substr(0, t.size() - 1));
        printf("dropped: %s\n", text().c_str());
        fflush(stdout);
    }
    void paint(Painter& p) override {
        p.fillRect(rect(), hot ? theme::shades().itemHover : theme::shades().panel);
        p.drawRect(rect(), hot ? theme::c("accent") : theme::shades().line);
        Label::paint(p);
    }
};
}  // namespace

int main(int argc, char** argv) {
    double seconds = 0;
    std::string shot;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
    }
    App& app = App::get();
    app.init(argc, argv);

    plat::WindowOptions o;
    o.title = "Toolkit demo";
    o.w = 900;
    o.h = 640;
    o.acceptsFiles = true;
    Window win(o);
    app.mainWindow = &win;
    win.closeRequested = [&] {
        app.quit();
        return true;
    };
    auto* root = vbox(&win, 0, 0);
    auto* bar = win.add<MenuBar>();
    root->addWidget(bar);
    auto* body = win.add<Widget>();
    root->addWidget(body, 1);
    auto* status = win.add<StatusBar>();
    root->addWidget(status);
    auto say = [status](const std::string& s) {
        status->showMessage(s, 4000);
        printf("%s\n", s.c_str());
        fflush(stdout);
    };

    // --- menus ---
    Menu* file = bar->addMenu("&File");
    file->addAction("Open…", [&] {
        std::string p = files::openFile(&win, "Open project", "", "Exbit project (*.json);;All files (*)");
        say(p.empty() ? "Open: cancelled" : "Open: " + p);
    }, "Ctrl+O");
    file->addAction("Add audio or MIDI…", [&] {
        auto v = files::openFiles(&win, "Add audio or MIDI", "",
                                  "Audio and MIDI (*.wav *.flac *.mp3 *.ogg *.aif *.aiff *.mid *.midi);;All files (*)");
        say(std::to_string(v.size()) + " file(s) chosen");
    });
    file->addAction("Save as…", [&] {
        std::string f;
        std::string p = files::saveFile(&win, "Save project", "Untitled.json", "Exbit project (*.json)", &f);
        say(p.empty() ? "Save: cancelled" : "Save: " + p + "  [" + f + "]");
    }, "Ctrl+Shift+S");
    file->addAction("Choose a folder…", [&] {
        std::string p = files::folder(&win, "Add a folder with samples", "");
        say(p.empty() ? "Folder: cancelled" : "Folder: " + p);
    });
    Menu* recent = file->addMenu("Recent projects");
    recent->aboutToShow = [](Menu* m) {
        m->clear();
        for (int i = 1; i <= 4; ++i) m->addAction("Project " + std::to_string(i) + ".json");
        m->addSeparator();
        m->addAction("Clear the list");
    };
    file->addSeparator();
    file->addAction("Quit", [&] { app.quit(); }, "Ctrl+Q");
    Menu* edit = bar->addMenu("&Edit");
    edit->addAction("Undo", [&] { say("Undo"); }, "Ctrl+Z");
    edit->addAction("Redo", [&] { say("Redo"); }, "Ctrl+Shift+Z");
    edit->addSeparator();
    edit->addCheckable("Snap to grid", true, [&](bool on) { say(on ? "Snap on" : "Snap off"); }, "Ctrl+G");
    Menu* grid = edit->addMenu("Grid");
    for (const char* g : {"1/4", "1/8", "1/16", "1/32"}) {
        Action* a = grid->addCheckable(g, std::string(g) == "1/16", [&, g](bool on) {
            if (on) say(std::string("Grid ") + g);
        });
        a->group = 1;
    }
    edit->addSection("Recently tweaked");
    Action* dis = edit->addAction("Nothing yet");
    dis->enabled = false;
    Menu* view = bar->addMenu("&View");
    Menu* fps = view->addMenu("Screen refresh");
    for (int f : {60, 30, 15}) {
        Action* a = fps->addCheckable(std::to_string(f) + " fps", f == 60, [&, f](bool on) {
            if (on) {
                app.setFrameRate(f);
                say("Frame rate " + std::to_string(f));
            }
        });
        a->group = 2;
    }
    Menu* help = bar->addMenu("&Help");
    help->addAction("About", [&] {
        msg::information(&win, "About", "Exbit's own toolkit.\nNo Qt was harmed.");
    });
    bar->registerShortcuts(ShortcutScope::ApplicationWide);
    Shortcut play(&win, KeySequence::parse("Space"), [&] { say("Space: play / stop"); }, ShortcutScope::ApplicationWide);
    Shortcut save(&win, KeySequence::parse("Ctrl+S"), [&] { say("Ctrl+S: saved"); }, ShortcutScope::ApplicationWide);

    // --- the controls ---
    auto* v = vbox(body, 12, 10);
    auto row1 = std::make_unique<HBox>();
    HBox* r1 = row1.get();
    r1->spacing = 8;
    v->addLayout(std::move(row1));
    r1->addWidget(pushButton(body, "Dialog…", [&] {
        Dialog d(&win, "Track settings", 360, 200);
        auto* dv = vbox(&d, 12, 10);
        auto* dw = d.add<Widget>();
        auto* fl = form(dw, 0, 8);
        auto* name = dw->add<LineEdit>("Track 1");
        fl->addRow("Name:", name);
        auto* vol = dw->add<SpinBox>();
        vol->setRange(-60, 12);
        vol->setDecimals(1);
        vol->setSingleStep(0.5);
        vol->setSuffix(" dB");
        fl->addRow("Volume:", vol);
        auto* out = dw->add<ComboBox>();
        out->addItem("1/2", 0);
        out->addItem("3/4", 2);
        fl->addRow("Output:", out);
        auto* mono = dw->add<CheckBox>("Mono");
        fl->addRow(mono);
        dv->addWidget(dw);
        dv->addStretch();
        auto* bb = d.add<ButtonBox>(ButtonBox::Ok | ButtonBox::Cancel | ButtonBox::Apply);
        bb->accepted.connect([&] { d.accept(); });
        bb->rejected.connect([&] { d.reject(); });
        bb->button(ButtonBox::Apply)->clicked.connect([&] { say("Applied: " + name->text()); });
        dv->addWidget(bb);
        d.adjustSize();
        int r = d.exec();
        say(r ? "Dialog: OK, name " + name->text() + ", " + vol->text() : "Dialog: cancelled");
    }));
    r1->addWidget(pushButton(body, "Question…", [&] {
        int a = msg::question(&win, "Unsaved changes", "Save the changes to the current project?",
                              msg::Save | msg::Discard | msg::Cancel);
        say(a == msg::Save ? "Save" : a == msg::Discard ? "Don't save" : "Cancel");
    }));
    r1->addWidget(pushButton(body, "Warning…", [&] {
        msg::warning(&win, "Cannot load plugin", "Some Plugin\n/usr/lib/vst3/Some.vst3\n\nNo factory in it.");
    }));
    r1->addWidget(pushButton(body, "Rename…", [&] {
        std::string t = "Track 1";
        if (getText(&win, "Rename track", "Name:", t)) say("Renamed to " + t);
    }));
    r1->addWidget(pushButton(body, "Pick…", [&] {
        int i = 0;
        if (getItem(&win, "Driver", "Audio driver:", {"ALSA", "PulseAudio"}, i)) say(i ? "PulseAudio" : "ALSA");
    }));
    r1->addWidget(pushButton(body, "Progress…", [&] {
        ProgressDialog pd(&win, "Rendering", "Rendering Track 1…", true);
        pd.setRange(0, 100);
        for (int i = 0; i <= 100 && !pd.wasCanceled(); ++i) {
            pd.setValue(i);
            double until = plat::now() + 0.03;
            while (plat::now() < until) app.processEvents();
        }
        say(pd.wasCanceled() ? "Rendering cancelled" : "Rendered");
    }));
    r1->addStretch();
    auto row2 = std::make_unique<HBox>();
    HBox* r2 = row2.get();
    r2->spacing = 6;
    v->addLayout(std::move(row2));
    Button* fx = toolButton(body, "FX", [&] { say("FX"); }, "Effects: none yet\nClick to add some");
    fx->setProperty("lit", "true");
    fx->setFixedSize(28, 22);
    r2->addWidget(fx);
    for (auto rs : {std::pair<const char*, const char*>{"M", "mute"}, {"S", "solo"}}) {
        Button* b = toolButton(body, rs.first, nullptr, std::string(rs.second) + " (click to toggle)");
        b->setCheckable(true);
        b->setProperty("toggle", rs.second);
        b->setFixedSize(26, 22);
        b->setFocusPolicy(FocusPolicy::None);
        r2->addWidget(b);
    }
    Menu instruments;
    instruments.aboutToShow = [&](Menu* m) {
        m->clear();
        m->addAction("Built-in synth", [&] { say("Built-in synth"); });
        m->addAction("Some VST3 synth", [&] { say("Some VST3 synth"); });
    };
    Button* inst = toolButton(body, "🎹", [&] { say("instrument window"); }, "Instrument\n▾ to choose another");
    inst->setMenu(&instruments, true);
    inst->setFixedSize(40, 22);
    r2->addWidget(inst);
    Button* rep = toolButton(body, "+1", nullptr, "Auto-repeat: hold it");
    int counter = 0;
    rep->setAutoRepeat(true);
    rep->clicked.connect([&] { say("count " + std::to_string(++counter)); });
    r2->addWidget(rep);
    auto* raised = toolButton(body, "Raised");
    raised->setProperty("autoRaise", "true");
    raised->setCheckable(true);
    r2->addWidget(raised);
    r2->addStretch();
    auto row3 = std::make_unique<HBox>();
    HBox* r3 = row3.get();
    r3->spacing = 8;
    v->addLayout(std::move(row3));
    auto* search = body->add<LineEdit>();
    search->setPlaceholder("Type here (Space types, Ctrl+S saves)");
    search->setClearButton(true);
    r3->addWidget(search, 2);
    auto* sp = body->add<SpinBox>();
    sp->setRange(20, 300);
    sp->setValue(120);
    sp->setSuffix(" BPM");
    sp->valueChanged.connect([&](double x) { say("tempo " + util::fixed(x, 0)); });
    r3->addWidget(sp);
    auto* cb = body->add<ComboBox>();
    for (const char* s : {"44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz"}) cb->addItem(s);
    cb->insertSeparator();
    for (int i = 0; i < 14; ++i) cb->addItem("Option " + std::to_string(i));
    cb->activated.connect([&](int i) { say("combo: " + cb->itemText(i)); });
    r3->addWidget(cb);
    auto row4 = std::make_unique<HBox>();
    HBox* r4 = row4.get();
    r4->spacing = 12;
    v->addLayout(std::move(row4));
    auto* chk = body->add<CheckBox>("Metronome");
    chk->toggled.connect([&](bool on) { say(on ? "metronome on" : "metronome off"); });
    r4->addWidget(chk);
    auto* sl = body->add<Slider>();
    sl->setRange(-60, 6);
    sl->setValue(0);
    sl->setToolTip("Volume");
    sl->valueChanged.connect([&](int x) {
        sl->setToolTip("Volume " + std::to_string(x) + " dB (double-click: 0 dB)");
        say("volume " + std::to_string(x));
    });
    sl->doubleClicked.connect([&] { sl->setValue(0); });
    r4->addWidget(sl, 1);
    auto* link = body->add<Label>("exbit.example");
    link->setLink("https://example.org");
    link->onLink.connect([&](const std::string& u) { say("link: " + u); });
    r4->addWidget(link);
    auto row5 = std::make_unique<HBox>();
    HBox* r5 = row5.get();
    r5->spacing = 10;
    v->addLayout(std::move(row5), 1);
    auto* tree = body->add<TreeWidget>();
    tree->setMinimumWidth(200);
    std::function<void(TreeWidget::Node*)> fill = [&](TreeWidget::Node* n) {
        for (auto& e : util::listDir(n->data)) {
            if (e.name.empty() || e.name[0] == '.') continue;
            auto* c = n->add(e.name, util::joinPath(n->data, e.name), e.isDir);
            c->tooltip = c->data;
        }
    };
    tree->onFill = fill;
    tree->root()->add("Home", plat::homeDir(), true);
    tree->root()->add("/", "/", true);
    tree->refreshRows();
    tree->dragDataFor = [](TreeWidget::Node* n) {
        DragData d;
        d.files = {n->data};
        return d;
    };
    tree->activated.connect([&](TreeWidget::Node* n) { say("activated " + n->data); });
    tree->nodeContextMenu.connect([&](TreeWidget::Node* n, MouseEvent& e) {
        if (!n) return;
        Menu m;
        m.addAction("Copy path", [&] { setClipboard(n->data); say("copied " + n->data); });
        m.addAction("Add to project");
        m.exec(e.globalPos, tree);
    });
    r5->addWidget(tree, 1);
    auto* tabs = body->add<TabWidget>();
    auto* t1 = tabs->makeTab<Widget>("Scroll");
    auto* t1l = vbox(t1, 6, 6);
    auto* area = t1->add<ScrollArea>();
    auto* content = area->makeContent<Widget>();
    auto* cl = vbox(content, 6, 4);
    for (int i = 0; i < 30; ++i) {
        auto* rw = content->add<Widget>();
        auto* rl = hbox(rw, 0, 6);
        rl->addWidget(rw->add<Label>("Row " + std::to_string(i + 1)));
        auto* s = rw->add<Slider>();
        s->setWheelEnabled(false);
        rl->addWidget(s, 1);
        auto* c = rw->add<ComboBox>();
        c->setWheelEnabled(false);
        c->addItem("1/2");
        c->addItem("3/4");
        rl->addWidget(c);
        cl->addWidget(rw);
    }
    t1l->addWidget(area);
    auto* t2 = tabs->makeTab<Widget>("List");
    auto* t2l = vbox(t2, 6, 6);
    auto* list = t2->add<ListWidget>();
    list->setMultiSelection(true);
    for (int i = 0; i < 40; ++i) {
        ListWidget::Item it;
        it.text = "Plugin " + std::to_string(i + 1);
        it.tooltip = "Tooltip of plugin " + std::to_string(i + 1);
        it.checkable = i % 3 == 0;
        list->addItem(it);
    }
    list->itemActivated.connect([&](int r) { say("list activated " + std::to_string(r)); });
    t2l->addWidget(list);
    auto* t3 = tabs->makeTab<Widget>("Group");
    auto* t3l = vbox(t3, 6, 6);
    auto* gb = t3->add<GroupBox>("Sounds");
    auto* gl = vbox(gb, 0, 6);
    gl->addWidget(gb->add<CheckBox>("Accent the first beat"));
    gl->addWidget(gb->add<Label>("Click volume"));
    gl->addWidget(gb->add<Slider>());
    t3l->addWidget(gb);
    t3l->addStretch();
    r5->addWidget(tabs, 2);
    auto* dropBox = body->add<DropBox>();
    v->addWidget(dropBox);

    win.showWindow();
    status->showMessage("Ready. Try the menus (Alt-free), Tab, Space, Ctrl+S, the tree's drag, file drops.");
    if (seconds > 0) {
        app.singleShot((int)(seconds * 1000), [&] {
            if (!shot.empty()) {
                Image img;
                if (win.native() && win.native()->capture(img)) savePng(shot, img);
                else savePng(shot, win.framebuffer());
            }
            app.quit();
        });
    }
    app.exec();
    return 0;
}
