#include "ui/skin_dialog.h"

#include <algorithm>
#include <cmath>

#include "core/util.h"
#include "gfx/font.h"
#include "ui/theme.h"
#include "ui/toolkit/layout.h"
#include "ui/toolkit/style.h"

using namespace ui;

namespace skin_dialog {
bool offersTransparency(const std::string& name) {
    static const char* const names[] = {"selection_fill", "selection_edge", "loop",       "loop_off",    "frozen_tint",
                                        "overview_view",  "overview_view_fill", "key_sounding", "roll_shade"};
    for (const char* n : names)
        if (name == n) return true;
    return false;
}
}  // namespace skin_dialog

// ---------------------------------------------------------------------------------------------
// ColorButton

ColorButton::ColorButton(const std::string& n, const std::string& l, const std::string& value, Changed changed)
    : name(n), label(l), changed_(std::move(changed)) {
    setMinimumWidth(130);
    clicked.connect([this] { pick(); });
    setValue(value);
}

void ColorButton::setValue(const std::string& value) {
    value_ = value;
    setText(value);
    update();
}

void ColorButton::paint(Painter& p) {
    Color c;
    skin::parseColor(value_, &c);
    Color text = c.lightnessF() * 255 < 128 || c.a < 90 ? Color(255, 255, 255) : Color(0, 0, 0);
    style::frame(p, rect(), c.withAlpha(255), theme::c("frame_edge"), 0);
    style::text(p, rect().adjusted(4, 3, -4, -3), AlignCenter, value_, font(), text, true);
    if (hasFocus()) p.drawRect(rect().adjusted(2, 2, -2, -2), text.withAlpha(90));
}

void ColorButton::pick() {
    bool alpha = skin_dialog::offersTransparency(name) || value_.size() == 9;
    SkinColorDialog dialog(window(), label, value_, alpha);
    if (dialog.exec() == Dialog::Accepted && dialog.value != value_) {
        setValue(dialog.value);
        if (changed_) changed_(name, value_);
    }
}

// ---------------------------------------------------------------------------------------------
// SkinColorDialog

namespace {
// the colour being chosen, over a checkerboard where it is see-through
class ColorSample : public Widget {
public:
    Color color;
    ColorSample() { setFixedSize(64, 64); }
    void paint(Painter& p) override {
        for (int y = 0; y < height(); y += 8)
            for (int x = 0; x < width(); x += 8)
                p.fillRect(Rect(x, y, 8, 8), ((x + y) / 8) % 2 ? Color(200, 200, 200) : Color(120, 120, 120));
        p.fillRect(Rect(0, 0, width() / 2, height()), color.withAlpha(255));  // the colour itself
        p.fillRect(Rect(width() / 2, 0, width() - width() / 2, height()), color);  // as it is drawn
        p.drawRect(rect(), theme::c("frame_edge"));
    }
};
}  // namespace

SkinColorDialog::SkinColorDialog(Window* parent, const std::string& title, const std::string& v, bool alpha)
    : Dialog(parent, title, 360, 320), value(v) {
    Color start;
    if (!skin::parseColor(v, &start)) start = Color(128, 128, 128);
    alpha_ = alpha ? start.a : 255;
    GridLayout* g = grid(this, 11, 6);
    wheel = add<ColorWheel>(180);
    auto* sample = add<ColorSample>();
    brightness = add<Slider>(Orientation::Horizontal);
    brightness->setRange(0, 100);
    brightness->setToolTip("Brightness");
    g->addWidget(wheel, 0, 0, 4, 1);
    g->addWidget(sample, 0, 1);
    g->addWidget(add<Label>("Hex"), 1, 1);
    hexEdit = add<LineEdit>();
    hexEdit->setMaxLength(alpha ? 9 : 7);
    hexEdit->setFixedWidth(90);
    g->addWidget(hexEdit, 2, 1);
    g->addWidget(brightness, 4, 0);
    int row = 5;
    if (alpha) {
        opacity = add<Slider>(Orientation::Horizontal);
        opacity->setRange(0, 255);
        opacity->setValueSilently(alpha_);
        opacity->setToolTip("Opacity (0: see-through, 255: solid)");
        g->addWidget(add<Label>("Opacity"), row++, 0);
        g->addWidget(opacity, row++, 0);
        opacity->valueChanged.connect([this](int a) {
            alpha_ = a;
            show();
        });
    }
    auto* bb = add<ButtonBox>(ButtonBox::Ok | ButtonBox::Cancel);
    g->addWidget(bb, row, 0, 1, 2);
    bb->accepted.connect([this] { accept(); });
    bb->rejected.connect([this] { reject(); });

    wheel->setColor(start.withAlpha(255));
    brightness->setValueSilently((int)std::lround(wheel->v * 100));
    wheel->changed.connect([this] { show(); });
    brightness->valueChanged.connect([this](int b) {
        wheel->setValue(b / 100.0);
        show();
    });
    hexEdit->editingFinished.connect([this] { fromHex(); });
    auto refreshSample = [this, sample] {
        Color c;
        skin::parseColor(value, &c);
        sample->color = c;
        sample->update();
    };
    // (the sample follows `value`)
    hexEdit->textChanged.connect([refreshSample](const std::string&) { refreshSample(); });
    value = start.withAlpha(alpha_).hex();
    hexEdit->setText(value);
    refreshSample();
    adjustSize();
}

void SkinColorDialog::show() {
    value = wheel->color().withAlpha(alpha_).hex();
    hexEdit->setText(value);
}

void SkinColorDialog::fromHex() {
    std::string t = util::lower(util::trim(hexEdit->text()));
    Color c;
    bool ok = skin::parseColor(t, &c);
    if (!ok) {
        std::string six;
        ok = color_picker::parseHex(t, six) && skin::parseColor(six, &c);
    }
    if (!ok || (!opacity && c.a != 255)) {
        hexEdit->setText(value);
        return;
    }
    if (opacity) {
        alpha_ = t.size() == 9 ? c.a : alpha_;
        opacity->setValueSilently(alpha_);
    }
    wheel->setColor(c.withAlpha(255));
    brightness->setValueSilently((int)std::lround(wheel->v * 100));
    value = c.withAlpha(alpha_).hex();
    hexEdit->setText(value);
}

// ---------------------------------------------------------------------------------------------
// SkinDialog

namespace {
skin::Skin completed(const skin::Skin& s) {
    skin::Skin w = s;
    w.colors.clear();
    for (auto& kv : s.completeColors()) w.colors[kv.first] = kv.second;
    return w;
}

// the families to choose from, with `current` among them
void fillFamilies(ComboBox* box, const std::string& current) {
    static std::vector<std::string> families = Font::families();
    box->clear();
    bool found = false;
    for (auto& f : families) found = found || util::lower(f) == util::lower(current);
    if (!found && !current.empty()) box->addItem(current, current);
    for (auto& f : families) box->addItem(f, f);
    int at = -1;
    for (int i = 0; i < box->count(); i++)
        if (util::lower(box->itemText(i)) == util::lower(current)) at = i;
    box->setCurrentIndexSilently(std::max(0, at));
}

// the numbers font: `name`, or the system's own fixed-width font when that isn't installed (the
// skins' Consolas on Linux), so times and LUFS readouts don't jump about (theme.mono_family)
std::string monoFamily(const std::string& name) {
    static std::vector<std::string> families = Font::families();
    auto installed = [&](const std::string& want) {
        for (auto& f : families)
            if (util::lower(f) == util::lower(want)) return f;
        return std::string();
    };
    if (!name.empty() && !installed(name).empty()) return installed(name);
    for (const char* want : {"DejaVu Sans Mono", "Noto Sans Mono", "Liberation Mono", "Ubuntu Mono"})
        if (!installed(want).empty()) return installed(want);
    return name.empty() ? std::string("Consolas") : name;
}

// the family the system's own interface font is (font.h: DejaVu Sans, Noto Sans, Liberation Sans…)
std::string systemFamily() {
    std::string named = Font::ui().family();
    if (!named.empty()) return named;
    static std::vector<std::string> families = Font::families();
    for (const char* want : {"DejaVu Sans", "Noto Sans", "Liberation Sans", "Segoe UI"})
        for (auto& f : families)
            if (util::lower(f) == util::lower(want)) return f;
    return families.empty() ? std::string() : families[0];
}
}  // namespace

SkinDialog::SkinDialog(Window* parent, const skin::Skin& s, SkinFn apply, SkinFn saved, const std::string& tab)
    : Dialog(parent, "Customize skin", 560, 620), original(s), apply_(std::move(apply)), saved_(std::move(saved)) {
    working = completed(s);
    auto* col = vbox(this, 11, 6);

    auto nameRowOwner = std::make_unique<HBox>();
    HBox* nameRow = nameRowOwner.get();
    col->addLayout(std::move(nameRowOwner));
    nameRow->addWidget(add<Label>("Skin name"));
    nameEdit = add<LineEdit>(s.name);
    nameEdit->textEdited.connect([this](const std::string&) { mark(); });
    nameRow->addWidget(nameEdit, 1);
    Label* hint = add<Label>("Changes show at once. Your own skins are kept in " + skin::userDir());
    hint->setProperty("hint", "true");
    hint->setWordWrap(true);
    col->addWidget(hint);

    tabs = add<TabWidget>();
    std::vector<std::string> groups;
    for (auto& info : theme::colors())
        if (std::find(groups.begin(), groups.end(), info.group) == groups.end()) groups.push_back(info.group);
    for (auto& group : groups) {
        auto scroll = std::make_unique<ScrollArea>();
        Widget* page = scroll->makeContent<Widget>();
        FormLayout* f = form(page, 9, 6);
        for (auto& info : theme::colors()) {
            if (group != info.group) continue;
            std::string name = info.name;
            auto* b = page->add<ColorButton>(name, info.label, working.colors[name],
                                             [this](const std::string& n, const std::string& v) { setColor(n, v); });
            buttons[name] = b;
            f->addRow(info.label, b);
        }
        tabs->addTab(std::move(scroll), group);
    }
    tabs->addTab(fontPage(), "Fonts");
    for (int i = 0; i < tabs->count(); i++)
        if (tabs->tabText(i) == tab) tabs->setCurrentIndex(i);
    col->addWidget(tabs, 1);

    auto buttonsOwner = std::make_unique<HBox>();
    HBox* row = buttonsOwner.get();
    col->addLayout(std::move(buttonsOwner));
    saveButton = add<Button>("Save");
    saveButton->clicked.connect([this] { save(); });
    Button* saveAsButton = add<Button>("Save as new skin…");
    saveAsButton->clicked.connect([this] { saveAs(); });
    Button* exportButton = add<Button>("Export…");
    exportButton->setToolTip("Write this skin to a JSON file anywhere, to share it");
    exportButton->clicked.connect([this] { exportSkin(); });
    Button* revertButton = add<Button>("Revert");
    revertButton->setToolTip("Back to the skin as it was when this window opened");
    revertButton->clicked.connect([this] { revert(); });
    Button* closeButton = add<Button>("Close");
    closeButton->clicked.connect([this] { reject(); });
    for (Button* b : {saveButton, saveAsButton, exportButton, revertButton}) row->addWidget(b);
    row->addStretch(1);
    row->addWidget(closeButton);
    updateButtons();
    // (wide enough for every tab: they don't scroll)
    int tabsWidth = 0;
    for (int i = 0; i < tabs->count(); i++) tabsWidth += style::textWidth(theme::uiFont(), tabs->tabText(i)) + 24;
    resizeWindow(std::max(560, tabsWidth + 22 + 8), 620);
}

std::unique_ptr<Widget> SkinDialog::fontPage() {
    const theme::Fonts& f = working.fonts;
    auto page = std::make_unique<Widget>();
    FormLayout* fl = form(page.get(), 9, 6);
    systemFont = page->add<CheckBox>("The system's own");
    systemFont->setCheckedSilently(f.ui.empty());
    uiFont = page->add<ComboBox>();
    fillFamilies(uiFont, f.ui.empty() ? systemFamily() : f.ui);
    uiFont->setEnabled(!f.ui.empty());
    uiFont->setFixedWidth(240);
    uiSize = page->add<SpinBox>();
    uiSize->setRange(0, 40);
    uiSize->setDecimals(1);
    uiSize->textFromValue = [](double v) {
        return v == 0 ? std::string("The system's own") : util::fixed(v, 1);
    };
    uiSize->setValueSilently(f.uiSize);
    monoFont = page->add<ComboBox>();
    monoFont->setFixedWidth(240);
    fillFamilies(monoFont, monoFamily(f.mono));
    auto rowOwner = std::make_unique<HBox>();
    HBox* row = rowOwner.get();
    fl->addRow(theme::FONT_LABEL_UI, std::move(rowOwner));
    row->addWidget(uiFont);
    row->addWidget(systemFont);
    row->addStretch(1);
    fl->addRow(theme::FONT_LABEL_UI_SIZE, uiSize);
    auto monoRowOwner = std::make_unique<HBox>();
    HBox* monoRow = monoRowOwner.get();
    fl->addRow(theme::FONT_LABEL_MONO, std::move(monoRowOwner));
    monoRow->addWidget(monoFont);
    monoRow->addStretch(1);
    systemFont->toggled.connect([this](bool on) {
        uiFont->setEnabled(!on);
        fontsChanged();
    });
    uiFont->currentIndexChanged.connect([this](int) { fontsChanged(); });
    uiSize->valueChanged.connect([this](double) { fontsChanged(); });
    monoFont->currentIndexChanged.connect([this](int) { fontsChanged(); });
    return page;
}

void SkinDialog::mark() {
    dirty = true;
    updateButtons();
}

void SkinDialog::updateButtons() {
    saveButton->setEnabled(dirty && !working.builtin);
    saveButton->setToolTip(working.builtin ? "The program's own skins can't be changed: use Save as new skin"
                                           : "Save the changes in this skin's file");
}

void SkinDialog::setColor(const std::string& name, const std::string& value) {
    working.colors[name] = value;
    if (apply_) apply_(working);
    mark();
}

void SkinDialog::fontsChanged() {
    if (quiet_) return;
    working.fonts.ui = systemFont->isChecked() ? std::string() : uiFont->currentText();
    working.fonts.uiSize = uiSize->value();
    working.fonts.mono = monoFont->currentText();
    if (apply_) apply_(working);
    mark();
}

void SkinDialog::revert() {
    working = completed(original);
    for (auto& kv : buttons) kv.second->setValue(working.colors[kv.first]);
    nameEdit->setTextSilently(original.name);
    // (the font fields too, so they show what is back on)
    const theme::Fonts& f = working.fonts;
    quiet_ = true;
    systemFont->setCheckedSilently(f.ui.empty());
    uiFont->setEnabled(!f.ui.empty());
    if (!f.ui.empty()) fillFamilies(uiFont, f.ui);
    uiSize->setValueSilently(f.uiSize);
    fillFamilies(monoFont, monoFamily(f.mono));
    quiet_ = false;
    if (apply_) apply_(working);
    dirty = false;
    updateButtons();
}

skin::Skin SkinDialog::current() const {
    skin::Skin s = working;
    std::string typed = util::trim(nameEdit->text());
    if (!typed.empty()) s.name = typed;
    return s;
}

bool SkinDialog::save() {
    if (working.builtin || working.path.empty()) return saveAs();
    skin::Skin s = current();
    std::string err;
    if (!skin::write(s, s.path, &err)) {
        msg::warning(this, "Cannot save skin", err);
        return false;
    }
    return doneSaving(s);
}

bool SkinDialog::saveAs() {
    std::string name = current().name + (working.builtin ? " (mine)" : "");
    if (!getText(this, "Save as new skin", "Name of the new skin:", name) || util::trim(name).empty()) return false;
    name = util::trim(name);
    std::string path = util::joinPath(skin::userDir(), skin::fileName(name));
    if (util::fileExists(path) &&
        msg::question(this, "Skin exists",
                      "There is already a skin file " + util::baseName(path) + ". Replace it?") != msg::Yes)
        return false;
    skin::Skin s = current();
    s.name = name;
    s.path = path;
    s.builtin = false;
    std::string err;
    if (!skin::write(s, path, &err)) {
        msg::warning(this, "Cannot save skin", err);
        return false;
    }
    return doneSaving(s);
}

bool SkinDialog::doneSaving(const skin::Skin& s) {
    working = completed(s);
    original = s;
    nameEdit->setTextSilently(s.name);
    dirty = false;
    updateButtons();
    if (saved_) saved_(s);
    return true;
}

void SkinDialog::exportSkin() {
    skin::exportSkin(this, current());
}

void SkinDialog::done(int result) {
    if (result != Accepted && dirty) {
        int answer = msg::question(this, "Unsaved skin", "Keep the changes to this skin?",
                                   msg::Save | msg::Discard | msg::Cancel);
        if (answer == msg::Cancel || (answer == msg::Save && !save())) return;
        if (answer == msg::Discard) revert();
    }
    Dialog::done(result);
}
