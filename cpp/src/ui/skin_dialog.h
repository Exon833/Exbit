// Options → Skin → Customize this skin…: every colour of the skin and its fonts, shown live as you
// change them (skin_dialog.py). Save keeps them in your own skin file (the program's own skins are
// saved as a new one of yours); Export writes the skin anywhere, to share it.
#pragma once
#include <functional>
#include <map>
#include <string>

#include "ui/color_picker.h"
#include "ui/skin.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"

namespace skin_dialog {
// usually see-through colours: their picker offers transparency (TRANSPARENT)
bool offersTransparency(const std::string& name);
}  // namespace skin_dialog

// A colour swatch with its hex value; click to pick another.
class ColorButton : public ui::Button {
public:
    using Changed = std::function<void(const std::string& name, const std::string& value)>;
    ColorButton(const std::string& name, const std::string& label, const std::string& value, Changed changed);
    void setValue(const std::string& value);
    const std::string& value() const { return value_; }
    void pick();                 // the colour chooser (with transparency where it applies)
    void paint(Painter& p) override;
    std::string name, label;
private:
    std::string value_;
    Changed changed_;
};

// The colour chooser of one skin colour (QColorDialog): wheel, brightness, opacity (where the
// colour may be see-through), hex. After Accepted: `value` "#rrggbb" or "#rrggbbaa".
class SkinColorDialog : public ui::Dialog {
public:
    SkinColorDialog(ui::Window* parent, const std::string& title, const std::string& value, bool alpha);
    std::string value;
    ColorWheel* wheel;
    ui::Slider* brightness;
    ui::Slider* opacity = nullptr;
    ui::LineEdit* hexEdit;
private:
    void show();
    void fromHex();
    int alpha_ = 255;
};

class SkinDialog : public ui::Dialog {
public:
    using SkinFn = std::function<void(const skin::Skin&)>;
    // skin: the Skin to start from; apply(Skin) shows a skin on the program; saved(Skin) is told
    // about a skin that was saved (to choose it). tab: the name of the tab to open on (a colour
    // group such as "Tracks and clips", or "Fonts").
    SkinDialog(ui::Window* parent, const skin::Skin& skin, SkinFn apply, SkinFn saved, const std::string& tab = "");

    skin::Skin original, working;  // (working: complete colours and fonts)
    bool dirty = false;
    std::map<std::string, ColorButton*> buttons;
    ui::LineEdit* nameEdit;
    ui::TabWidget* tabs;
    ui::Button* saveButton;
    ui::CheckBox* systemFont;
    ui::ComboBox* uiFont;
    ui::SpinBox* uiSize;
    ui::ComboBox* monoFont;

    void setColor(const std::string& name, const std::string& value);
    void fontsChanged();
    void revert();                 // back to the skin as it was when this window opened
    skin::Skin current() const;    // the working skin with the name typed
    bool save();                   // into this skin's own file (yours only), else saveAs()
    bool saveAs();
    void exportSkin();
    // Close, Esc and the window's ✕ all end here: unsaved changes are kept or dropped as you say,
    // or the window stays open (Cancel)
    void done(int result) override;
private:
    SkinFn apply_, saved_;
    bool quiet_ = false;           // (filling the font fields: not a change)
    void mark();
    void updateButtons();
    bool doneSaving(const skin::Skin& s);
    std::unique_ptr<ui::Widget> fontPage();
};
