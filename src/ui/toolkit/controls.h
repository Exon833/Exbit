// The controls (QLabel, QPushButton/QToolButton, QCheckBox, QLineEdit, QSpinBox, QComboBox,
// QSlider, QScrollBar, QScrollArea, QListWidget, QTreeWidget, QTabWidget, QGroupBox, QFrame,
// QSplitter), drawn in the flat look of theme.py's stylesheet, in the skin's colours.
#pragma once
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/toolkit/layout.h"
#include "ui/toolkit/widget.h"

namespace ui {

class Menu;
class ComboPopup;

// --- text ---
// Styled as `Label` (default.css, Basic controls): its colour and font (color, font-*; .hint, .dimmed, #time, #lufs),
// padding (inside `margin`), :link (a link: underlined while text-decoration is underline), ::image (an image's
// opacity). setFont / setColor override the stylesheets'.
class Label : public Widget {
public:
    explicit Label(const std::string& text = "", Widget* parent = nullptr);
    void setText(const std::string& t);
    const std::string& text() const { return text_; }
    void setAlignment(int a) { align_ = a; update(); }
    void setWordWrap(bool on) { wrap_ = on; updateGeometry(); update(); }
    void setElide(bool on) { elide_ = on; updateGeometry(); update(); }
    void setFont(const Font& f) { font_ = f; hasFont_ = true; updateGeometry(); update(); }
    void clearFont() {  // the stylesheets' again
        if (!hasFont_) return;
        hasFont_ = false;
        updateGeometry();
        update();
    }
    void setColor(Color c) {  // overrides the style's (a repaint only when it changes: paint() may set it)
        if (hasColor_ && color_ == c) return;
        color_ = c;
        hasColor_ = true;
        update();
    }
    void clearColor() { hasColor_ = false; update(); }
    void setImage(const Image& img) { image_ = img; updateGeometry(); update(); }
    // a clickable link: the text is drawn underlined in the accent colour; onLink(url) when clicked
    void setLink(const std::string& url) { link_ = url; setCursor(url.empty() ? Cursor::Arrow : Cursor::Hand); update(); }
    Signal<const std::string&> onLink;
    Signal<> doubleClicked;
    Font font() const;      // setFont's, else the stylesheets'
    Color color() const;    // setColor's, else the stylesheets' (.hint: hint text, .dimmed: dimmed text, :link: accent)
    Point sizeHint() const override;
    Point minimumSizeHint() const override;
    void paint(Painter& p) override;
    bool mousePress(MouseEvent& e) override;
    bool mouseDoubleClick(MouseEvent& e) override;
    void styleStates(std::vector<std::string>& out) const override;  // + :link
    int margin = 0;         // px around it (added to the stylesheets' padding)
private:
    std::string text_, link_;
    int align_ = AlignLeft | AlignVCenter;
    bool wrap_ = false, elide_ = false, hasFont_ = false, hasColor_ = false;
    Font font_;
    Color color_;
    Image image_;
};

// --- buttons ---
// One class for QPushButton and QToolButton. Styles via properties: "lit"="true" (accent fill),
// "toggle"="mute"/"solo"/"record" (that colour when checked), "accent"="true" (accent when checked),
// "autoRaise"="true" (no fill until hovered); setDefault(true): a dialog's main button (accent).
class Button : public Widget {
public:
    enum class Style { Push, Tool };
    explicit Button(const std::string& text = "", Widget* parent = nullptr);
    void setText(const std::string& t);
    const std::string& text() const { return text_; }
    void setIcon(const Image& icon);               // drawn centred (or left of the text)
    void setIconSvg(const std::string& svgText, int px);  // re-tinted in the button's text colour on every skin
    void clearIcon();
    void setCheckable(bool on) { checkable_ = on; }
    bool isCheckable() const { return checkable_; }
    void setChecked(bool on);                      // emits toggled when it changes
    void setCheckedSilently(bool on) { checked_ = on; update(); }
    bool isChecked() const { return checked_; }
    void setDefault(bool on) { default_ = on; update(); }
    bool isDefault() const { return default_; }
    void setStyle(Style s);                        // (Tool: a fixed-size QToolButton, Tab focus)
    Style style() const { return style_; }
    void setFont(const Font& f) { font_ = f; hasFont_ = true; updateGeometry(); update(); }
    Font font() const;
    // a drop-down menu: with splitArrow (QToolButton::MenuButtonPopup) only the ▾ part opens it,
    // otherwise (InstantPopup / QPushButton::setMenu) the whole button does
    void setMenu(Menu* menu, bool splitArrow = false);
    Menu* menu() const { return menu_; }
    void showMenu();
    void setAutoRepeat(bool on) { autoRepeat_ = on; }
    void click();                                  // as if clicked
    ~Button() override;
    Signal<> clicked;
    Signal<bool> toggled;
    Signal<> pressed, released;
    Signal<MouseEvent&> rightClicked;              // (convenience: right button)
    Point sizeHint() const override;
    Point minimumSizeHint() const override { return sizeHint(); }
    void paint(Painter& p) override;
    bool mousePress(MouseEvent& e) override;
    bool mouseRelease(MouseEvent& e) override;
    bool mouseMove(MouseEvent& e) override;
    bool mouseDoubleClick(MouseEvent& e) override;
    bool keyPress(KeyEvent& e) override;
    void enter() override { update(); }
    void leave() override { hoverArrow_ = false; update(); }
    bool isDown() const { return down_; }          // drawn pressed (held with the pointer on it)
    // colours as the style gives them now (for custom-drawn buttons)
    Color fillColor() const;
    Color textColor() const;
    // its states for stylesheets: hover (not while held off it), pressed (held on it, or its menu open), checked,
    // default, lit (accent-coloured: lit / accent+checked / autoRaise+checked / toggle+checked / a dialog's
    // default), toggled (a toggle role's colour: checked with a toggle role, neither lit nor accent), icon,
    // arrow-hover (the split menu arrow under the mouse or open), disabled, focus
    void styleStates(std::vector<std::string>& out) const override;
private:
    std::string text_, iconSvg_;
    Image icon_;
    int iconPx_ = 0;
    std::string iconTintKey_;
    bool checkable_ = false, checked_ = false, default_ = false, hasFont_ = false, autoRepeat_ = false, split_ = false;
    bool down_ = false, menuArrowDown_ = false, hoverArrow_ = false, inside_ = false;
    int repeatTimer_ = -1;
    Image tinted_;
    Color tintedFor_;
    bool lit() const;
    void stopRepeat();
    Style style_ = Style::Push;
    Font font_;
    Menu* menu_ = nullptr;
};
// shorthands
Button* pushButton(Widget* parent, const std::string& text, std::function<void()> onClick = nullptr);
Button* toolButton(Widget* parent, const std::string& text, std::function<void()> onClick = nullptr,
                   const std::string& tooltip = "");

// Styled as `CheckBox`: its text (color, font-*), padding; ::indicator (the box: width, height, background-color,
// border, border-radius, padding; its check mark in `color`, margin-right: the gap to the text); states :checked
// :hover :disabled :focus.
class CheckBox : public Widget {
public:
    explicit CheckBox(const std::string& text = "", Widget* parent = nullptr);
    void setText(const std::string& t) { text_ = t; updateGeometry(); update(); }
    const std::string& text() const { return text_; }
    void setChecked(bool on);
    void setCheckedSilently(bool on) { checked_ = on; update(); }
    bool isChecked() const { return checked_; }
    Signal<bool> toggled;
    Point sizeHint() const override;
    Point minimumSizeHint() const override { return sizeHint(); }
    void paint(Painter& p) override;
    bool mousePress(MouseEvent& e) override;
    bool mouseRelease(MouseEvent& e) override;
    bool mouseMove(MouseEvent& e) override;
    bool keyPress(KeyEvent& e) override;
    void enter() override { update(); }
    void leave() override { update(); }
    void styleStates(std::vector<std::string>& out) const override;  // + :checked
private:
    std::string text_;
    bool checked_ = false, down_ = false;
};

// --- text entry ---
// Stylesheets: LineEdit. Its box (background, border, radius); its padding is the room around the text (and gives
// its size). States :hover :focus :disabled :pressed, :frameless (frame off: inside a SpinBox / ComboBox),
// :read-only, :clear-hover (the clear button under the mouse). Parts ::placeholder (color), ::selection (a box:
// background, border…; color: the selected text's; its padding reaches past the text), ::cursor (background-color,
// width), ::clear-button (color; width; margin-left: the room before it; margin-right: after it; icon-size: the
// cross; stroke-width). Its font is the stylesheets' unless the program set one (setFont).
class LineEdit : public Widget {
public:
    explicit LineEdit(const std::string& text = "", Widget* parent = nullptr);
    void setText(const std::string& t);          // emits textChanged
    void setTextSilently(const std::string& t);
    const std::string& text() const { return text_; }
    void setPlaceholder(const std::string& t) { placeholder_ = t; update(); }
    void setReadOnly(bool on) { readOnly_ = on; }
    void setMaxLength(int n) { maxLength_ = n; }
    void selectAll();
    void setClearButton(bool on) { clearButton_ = on; update(); }
    // accepts a key's text? (validators: hex colours, numbers); null: everything
    std::function<bool(const std::string& candidate)> validator;
    void setFont(const Font& f) { font_ = f; hasFont_ = true; updateGeometry(); update(); }
    void setAlignment(int a) { align_ = a; update(); }
    bool isReadOnly() const { return readOnly_; }
    const std::string& placeholder() const { return placeholder_; }
    // editing (as the keys do them; for programs and tests)
    size_t cursorPosition() const { return cursor_; }   // in bytes
    void setCursorPosition(size_t pos, bool select = false);
    std::string selectedText() const;
    bool hasSelectedText() const { return cursor_ != anchor_; }
    void deselect() { anchor_ = cursor_; update(); }
    void insertText(const std::string& s) { insert(s); }  // replaces the selection, as typing
    void copy() const;
    void cut();
    void paste();
    void undo();
    void redo();
    bool frame = true;                           // false: no field edge or fill (inside a SpinBox / ComboBox)
    ~LineEdit() override;
    Signal<const std::string&> textChanged;      // every edit
    Signal<const std::string&> textEdited;       // edits by the user only
    Signal<> returnPressed;
    Signal<> editingFinished;                    // Return, or focus lost
    Signal<> escapePressed;
    Signal<int> keyNavigation;                   // Up / Down / PageUp / PageDown pressed (lists under search boxes)
    Point sizeHint() const override;
    void paint(Painter& p) override;
    bool mousePress(MouseEvent& e) override;
    bool mouseMove(MouseEvent& e) override;
    bool mouseRelease(MouseEvent& e) override;
    bool mouseDoubleClick(MouseEvent& e) override;
    bool keyPress(KeyEvent& e) override;
    bool contextMenu(MouseEvent& e) override;
    bool overridesShortcut(const KeyEvent& e) const override;
    void focusIn() override;
    void focusOut() override;
    void enter() override { update(); }
    void leave() override { update(); }
    Point minimumSizeHint() const override;
    Font font() const;
    void styleStates(std::vector<std::string>& out) const override;
private:
    std::string text_, placeholder_;
    std::vector<std::pair<std::string, size_t>> undo_, redo_;
    bool clearHover_ = false, dirty_ = false;
    Rect textArea() const;
    Rect clearRect() const;
    void edited(const std::string& before, size_t beforeCursor);
    size_t posAt(int x) const;
    size_t cursor_ = 0, anchor_ = 0;
    float scroll_ = 0;
    bool readOnly_ = false, clearButton_ = false, hasFont_ = false, selecting_ = false;
    int maxLength_ = 32767;
    int align_ = AlignLeft | AlignVCenter;
    Font font_;
    double blinkStart_ = 0;
    int blinkTimer_ = -1;
    void insert(const std::string& s);
    void deleteSelection();
    bool hasSelection() const { return cursor_ != anchor_; }
    void ensureCursorVisible();
};

// QSpinBox / QDoubleSpinBox: a number field with ▴▾ buttons, the wheel and the arrow keys.
// Stylesheets: SpinBox (its editor is a LineEdit:frameless inside its border). Its box; its padding gives its size
// around the widest number. States :hover :focus (its editor has the focus) :disabled :pressed, :up-hover
// :down-hover (a button under the mouse), :up-pressed :down-pressed (held), :at-maximum :at-minimum (the value at
// that end, not wrapping: that button is off). Parts ::up-button and ::down-button (a box drawn over the inside of
// the border, clipped to the button; color: the arrow's; icon-size; stroke-width; width: the buttons' column, the
// up button's).
class SpinBox : public Widget {
public:
    explicit SpinBox(Widget* parent = nullptr);
    void setRange(double lo, double hi);
    void setDecimals(int d) { decimals_ = d; refresh(); }
    void setSingleStep(double s) { step_ = s; }
    void setSuffix(const std::string& s) { suffix_ = s; refresh(); }
    void setPrefix(const std::string& s) { prefix_ = s; refresh(); }
    void setValue(double v);                      // emits valueChanged when it changes
    void setValueSilently(double v);
    double value() const { return value_; }
    int intValue() const { return (int)std::lround(value_); }
    double minimum() const { return lo_; }
    double maximum() const { return hi_; }
    void setKeyboardTracking(bool on) { tracking_ = on; }   // false: valueChanged only on Return / focus out
    void setButtonsVisible(bool on) { buttons_ = on; update(); }
    void setWrapping(bool on) { wrapping_ = on; }
    void setAlignment(int a);
    void setWheelEnabled(bool on) { wheelOn_ = on; }
    std::function<std::string(double)> textFromValue;       // custom display
    std::function<bool(const std::string&, double&)> valueFromText;
    Signal<double> valueChanged;
    Signal<> editingFinished;
    Point sizeHint() const override;
    void paint(Painter& p) override;
    bool mousePress(MouseEvent& e) override;
    bool mouseRelease(MouseEvent& e) override;
    bool wheel(WheelEvent& e) override;
    bool keyPress(KeyEvent& e) override;
    void resized() override;
    void focusIn() override;
    void focusOut() override;
    void enter() override { update(); }
    void leave() override { hoverPart_ = 0; update(); }
    LineEdit* editor() const { return edit_; }
    void stepBy(int steps);
    std::string text() const;                     // as shown (prefix, number, suffix)
    double singleStep() const { return step_; }
    int decimals() const { return decimals_; }
    Point minimumSizeHint() const override { return sizeHint(); }
    bool mouseMove(MouseEvent& e) override;
    void styleStates(std::vector<std::string>& out) const override;
    ~SpinBox() override;
private:
    int hoverPart_ = 0, pressPart_ = 0;           // 1 up, -1 down
    float wheelAcc_ = 0;
    int partAt(Point p) const;
    int buttonsWidth() const;                     // the ▴▾ column (0 when hidden)
    bool parse(const std::string& text, double& v) const;
    double clampRound(double v) const;
    void stopRepeat();
    double lo_ = 0, hi_ = 99, value_ = 0, step_ = 1;
    int decimals_ = 0;
    bool tracking_ = true, buttons_ = true, wrapping_ = false, wheelOn_ = true;
    std::string suffix_, prefix_;
    LineEdit* edit_ = nullptr;
    int repeatTimer_ = -1;
    void refresh();
    void commit();
    std::string format(double v) const;
};

// QComboBox: items (text + data); a drop-down list when clicked.
// Stylesheets: ComboBox. Its box; its text from its padding's left (the padding gives its size). States :hover
// :focus :disabled :pressed, :open (its list is open), :editable. Part ::arrow (width: the right-hand box, over the
// border; padding-left: the arrow's place in it; a box: background…; color; icon-size; stroke-width). Its list is
// a ComboPopup: its box and padding; parts ::item (a box; padding; color; font; states :hover and :selected on the
// highlighted row, :disabled on a disabled one: the popup's states while an item's style is computed) and
// ::separator (height: its row; margin: the line's ends; a box: background…; stroke-width: the line's thickness).
class ComboBox : public Widget {
public:
    explicit ComboBox(Widget* parent = nullptr);
    void addItem(const std::string& text, const std::string& data = "");
    void addItem(const std::string& text, int data) { addItem(text, std::to_string(data)); }
    void insertSeparator();
    void clear();
    int count() const { return (int)items_.size(); }
    int currentIndex() const { return current_; }
    void setCurrentIndex(int i);                 // emits currentIndexChanged when it changes
    void setCurrentIndexSilently(int i);
    std::string currentText() const;
    std::string currentData() const;
    int currentDataInt(int def = 0) const;
    std::string itemText(int i) const;
    std::string itemData(int i) const;
    void setItemText(int i, const std::string& t);
    void setItemEnabled(int i, bool on);
    int findData(const std::string& data) const;
    int findData(int data) const { return findData(std::to_string(data)); }
    int findText(const std::string& text) const;
    void setWheelEnabled(bool on) { wheelOn_ = on; }   // _NoWheelComboBox: false
    void setEditable(bool on);                   // (QFontComboBox-like typing; the text is currentText())
    void showPopup();
    Signal<int> currentIndexChanged;
    Signal<int> activated;                       // chosen by the user (even the same one)
    Point sizeHint() const override;
    void paint(Painter& p) override;
    bool mousePress(MouseEvent& e) override;
    bool wheel(WheelEvent& e) override;
    bool keyPress(KeyEvent& e) override;
    void enter() override { update(); }
    void leave() override { update(); }
    Point minimumSizeHint() const override { return sizeHint(); }
    void resized() override;
    bool isPopupOpen() const;
    void hidePopup();
    bool isSeparator(int i) const { return i >= 0 && i < count() && items_[i].separator; }
    bool isItemEnabled(int i) const { return i >= 0 && i < count() && items_[i].enabled && !items_[i].separator; }
    LineEdit* lineEdit() const { return edit_; }
    int maxVisibleItems = 10;
    ~ComboBox() override;
    struct Item { std::string text, data; bool separator = false, enabled = true; };
    const std::vector<Item>& items() const { return items_; }
    void choose(int i);                          // as chosen in the list (currentIndexChanged, activated)
    void styleStates(std::vector<std::string>& out) const override;
private:
    int arrowWidth() const;                      // ::arrow's width (the right-hand box)
    std::vector<Item> items_;
    int current_ = -1;
    bool wheelOn_ = true, editable_ = false;
    LineEdit* edit_ = nullptr;
    std::unique_ptr<class ComboPopup> popup_;
    float wheelAcc_ = 0;
    int step(int from, int dir) const;
};

enum class Orientation { Horizontal, Vertical };

// The styles of the parts of a widget that have states of their own: a list's rows (ListWidget::item:selected),
// a slider's handle and a scroll bar's thumb (Slider::handle:hover, ScrollBar::thumb:pressed), a menu's items
// (Menu::item:hover). The part is matched as a node of its own: the widget's type, id, classes and ancestors,
// with the part's states and the widget's own, but the widget's hover and pressed (a part's :hover and
// :pressed are its own: the row, the handle under the mouse). It inherits from the widget's style, or from
// another part's (Menu::shortcut from its Menu::item). Each combination of states is computed once and kept
// for as long as the PartStyles: make one for a paint (a list's rows share a handful of styles) or an event.
class PartStyles {
public:
    explicit PartStyles(const Widget& w);
    ~PartStyles();
    PartStyles(const PartStyles&) = delete;
    PartStyles& operator=(const PartStyles&) = delete;
    // `part` in these states (and the widget's), inheriting from `from` (null: the widget's style)
    const css::Style& get(const std::string& part, const std::vector<const char*>& states = {},
                          const css::Style* from = nullptr);
    // the font of a style kept here (`def` with what the style changes of it), worked out once per style
    const Font& font(const css::Style& st, const Font& def);
    // a custom property of a style as a length in px ("--spare"): `def` when not set or not a length
    static double lengthVar(const css::Style& st, const std::string& name, double def);
private:
    struct PartNode;
    const Widget& w_;
    std::vector<std::string> own_;  // the widget's states (but hover and pressed)
    std::map<std::string, std::unique_ptr<css::Style>> cache_;
    std::map<const css::Style*, Font> fonts_;
};

// QSlider (flat: a groove, the part below the value in the accent colour, a round handle). Styles (default.css):
// Slider (:horizontal / :vertical; width / height: the size it asks for, min-width / min-height its shortest
// length), Slider::groove (its thickness: height when horizontal, width when vertical), Slider::fill (the groove
// below the value), Slider::handle (its own :hover and :pressed: under the mouse, dragged)
class Slider : public Widget {
public:
    explicit Slider(Orientation o = Orientation::Horizontal, Widget* parent = nullptr);
    void setRange(int lo, int hi);
    void setValue(int v);                        // emits valueChanged when it changes
    void setValueSilently(int v);
    int value() const { return value_; }
    int minimum() const { return lo_; }
    int maximum() const { return hi_; }
    void setPageStep(int s) { page_ = s; }
    void setWheelEnabled(bool on) { wheelOn_ = on; }
    void setTracking(bool on) { tracking_ = on; }
    Signal<int> valueChanged;
    Signal<> sliderPressed, sliderReleased;
    Signal<> doubleClicked;                      // (VolumeSlider: 0 dB) — the value is left alone unless handled
    Point sizeHint() const override;
    void paint(Painter& p) override;
    bool mousePress(MouseEvent& e) override;
    bool mouseMove(MouseEvent& e) override;
    bool mouseRelease(MouseEvent& e) override;
    bool mouseDoubleClick(MouseEvent& e) override;
    bool wheel(WheelEvent& e) override;
    bool keyPress(KeyEvent& e) override;
    void enter() override { update(); }
    void leave() override { hoverHandle_ = false; update(); }
    Point minimumSizeHint() const override;
    void styleStates(std::vector<std::string>& out) const override;
    void setSingleStep(int s) { single_ = std::max(1, s); }
    int sliderPosition() const { return pos_; }  // while dragging without tracking: where the handle is
    bool isSliderDown() const { return dragging_; }
    ~Slider() override;
    Orientation orientation;
private:
    int lo_ = 0, hi_ = 99, value_ = 0, page_ = 10, single_ = 1, pos_ = 0;
    bool wheelOn_ = true, tracking_ = true, dragging_ = false, hoverHandle_ = false;
    int dragOffset_ = 0, repeatTimer_ = -1, repeatDir_ = 0;
    float wheelAcc_ = 0;
    int valueAt(Point p) const;
    int handleCenter(int v) const;
    Point handleSize() const;                    // (Slider::handle's width and height in its states now)
    void handleStates(std::vector<const char*>& out) const;
    void stopRepeat();
};

// Styles: ScrollBar (:horizontal / :vertical; width / height: its size, the thickness from --scrollbar-width),
// ScrollBar::thumb (its own :hover and :pressed; margin: around it in the bar; min-height / min-width: its
// shortest length)
class ScrollBar : public Widget {
public:
    explicit ScrollBar(Orientation o = Orientation::Vertical, Widget* parent = nullptr);
    void setRange(int lo, int hi);               // value range (hi = content - page)
    void setPageStep(int page);                  // visible amount (sets the handle's size)
    void setSingleStep(int s) { single_ = s; }
    void setValue(int v);                        // clamped; emits valueChanged when it changes
    int value() const { return value_; }
    int minimum() const { return lo_; }
    int maximum() const { return hi_; }
    int pageStep() const { return page_; }
    Signal<int> valueChanged;
    Point sizeHint() const override;
    void paint(Painter& p) override;
    bool mousePress(MouseEvent& e) override;
    bool mouseMove(MouseEvent& e) override;
    bool mouseRelease(MouseEvent& e) override;
    bool wheel(WheelEvent& e) override;
    void enter() override { update(); }
    void leave() override { hoverHandle_ = false; update(); }
    int singleStep() const { return single_; }
    Point minimumSizeHint() const override { return sizeHint(); }
    void styleStates(std::vector<std::string>& out) const override;
    int thickness() const;                       // across it: its width when vertical, height when horizontal
    Rect handleRect() const;                     // the thumb
    ~ScrollBar() override;
    Orientation orientation;
private:
    int lo_ = 0, hi_ = 0, value_ = 0, page_ = 10, single_ = 20;
    bool dragging_ = false, hoverHandle_ = false;
    int dragOffset_ = 0, repeatTimer_ = -1;
    float wheelAcc_ = 0;
    void stopRepeat();
    Rect slotRect() const;                       // the thumb with its margins (what the mouse takes)
    void thumbStates(std::vector<const char*>& out) const;
};

// QScrollArea: shows a content widget through a viewport, with scroll bars as needed. Styled as ScrollArea: a box,
// its viewport and scroll bars inside its border and padding (read when it lays them out: a skin change does that)
class ScrollArea : public Widget {
public:
    explicit ScrollArea(Widget* parent = nullptr);
    // the area owns it; widgetResizable: the content is kept as wide (and at least as tall) as the viewport
    void setContent(std::unique_ptr<Widget> content, bool widgetResizable = true);
    template <class T, class... A> T* makeContent(A&&... a) {
        auto w = std::make_unique<T>(std::forward<A>(a)...);
        T* raw = w.get();
        setContent(std::move(w));
        return raw;
    }
    Widget* content() const { return content_; }
    Widget* viewport() const { return viewport_; }
    ScrollBar* verticalScrollBar() const { return vbar_; }
    ScrollBar* horizontalScrollBar() const { return hbar_; }
    enum class Policy { AsNeeded, AlwaysOff, AlwaysOn };
    void setHorizontalPolicy(Policy p) { hpol_ = p; relayout(); }
    void setVerticalPolicy(Policy p) { vpol_ = p; relayout(); }
    void ensureVisible(const Rect& r);           // r in the content's coordinates
    void scrollTo(int x, int y);
    void contentResized();                       // the content changed its size: recompute the bars
    bool wheel(WheelEvent& e) override;
    void resized() override;
    Point sizeHint() const override;
    Point minimumSizeHint() const override;
    void place();                                // (lays the content and the bars out now)
    // inside its border and padding (its style's box: drawn under them; none in the default look): where its
    // viewport and scroll bars go
    Rect innerRect() const;
    void paint(Painter& p) override;
private:
    Widget* viewport_;
    Widget* content_ = nullptr;
    ScrollBar* vbar_;
    ScrollBar* hbar_;
    Policy hpol_ = Policy::AsNeeded, vpol_ = Policy::AsNeeded;
    bool resizable_ = true, placing_ = false;
};

// QListWidget: rows of text (and optional data, colour, icon); single or extended selection. Styles: ListWidget
// (its rows inside its border and padding; width / height: the size it asks for, min-width / min-height),
// ListWidget::item (height: the rows'; states :selected :hover :alternate :disabled :checked :checkable :bold),
// ListWidget::indicator (the check box of checkable rows: :checked…), ListWidget::icon (width / height: the box
// icons are drawn in). An item's own colour (hasColor) is its text's, unless it is selected or disabled.
class ListWidget : public Widget {
public:
    explicit ListWidget(Widget* parent = nullptr);
    struct Item {
        std::string text, data, tooltip;
        bool enabled = true, selectable = true, bold = false, hasColor = false, checkable = false, checked = false;
        Color color;
        Image icon;
    };
    void addItem(const std::string& text, const std::string& data = "");
    void addItem(Item item);
    void insertItem(int row, Item item);
    void removeItem(int row);
    void clear();
    int count() const { return (int)items_.size(); }
    Item& item(int row) { return items_[row]; }
    const Item& item(int row) const { return items_[row]; }
    int currentRow() const { return current_; }
    void setCurrentRow(int row);                  // emits currentRowChanged
    std::vector<int> selectedRows() const;
    void setMultiSelection(bool on) { multi_ = on; }
    void setRowSelected(int row, bool on);
    void setAlternatingColors(bool on) { alternate_ = on; update(); }
    void scrollToRow(int row);
    int rowHeight = 22;                           // (read only: ListWidget::item's height, kept up to date)
    Signal<int> currentRowChanged;
    Signal<int> itemActivated;                    // double-click or Return
    Signal<int> itemClicked;
    Signal<int, bool> itemChecked;
    Signal<int, MouseEvent&> itemContextMenu;
    void paint(Painter& p) override;
    bool mousePress(MouseEvent& e) override;
    bool mouseMove(MouseEvent& e) override;
    bool mouseDoubleClick(MouseEvent& e) override;
    bool mouseRelease(MouseEvent& e) override;
    bool wheel(WheelEvent& e) override;
    bool keyPress(KeyEvent& e) override;
    bool contextMenu(MouseEvent& e) override;
    void resized() override;
    void leave() override { hoverRow_ = -1; update(); }
    std::string toolTipAt(Point p) const override;
    int rowAt(Point p) const;
    Point sizeHint() const override;
    Point minimumSizeHint() const override;
    bool isRowSelected(int row) const;
    void clearSelection();
    void setItemText(int row, const std::string& t) { if (row >= 0 && row < count()) { items_[row].text = t; update(); } }
    ScrollBar* scrollBar() const { return bar_; }
    int findData(const std::string& data) const;
private:
    std::vector<Item> items_;
    std::vector<bool> selected_;
    int current_ = -1, hoverRow_ = -1, anchor_ = -1, pressRow_ = -1;
    Rect checkRect(int row) const;
    Rect viewRect() const;                        // where the rows are: inside its border and padding, the bar
    void rowStates(int row, std::vector<const char*>& out) const;
    bool readStyle();                             // rowHeight from the stylesheets (true: it changed)
    void select(int row, int mods);
    bool multi_ = false, alternate_ = false;
    ScrollBar* bar_;
    void syncBar();
};

// QTreeWidget: an expandable tree (the Explorer's folders); children filled when first expanded. Styles:
// TreeWidget (as ListWidget), TreeWidget::item (height: the rows'; :selected :hover :disabled :open
// :has-children :bold), TreeWidget::branch (the arrow of nodes with children; width: the indent of each level,
// the arrow inside its padding), TreeWidget::icon
class TreeWidget : public Widget {
public:
    explicit TreeWidget(Widget* parent = nullptr);
    struct Node {
        std::string text, data, tooltip;
        bool expandable = false, expanded = false, filled = false, bold = false;
        Image icon;
        std::vector<std::unique_ptr<Node>> children;
        Node* parent = nullptr;
        Node* add(const std::string& text, const std::string& data = "", bool expandable = false);
    };
    Node* root() { return &root_; }
    void clear();
    // called when a node is expanded and its `filled` is false (the first time): fill its children
    // (filled is then true; set it false again to have it filled anew at the next expansion)
    std::function<void(Node*)> onFill;
    void setExpanded(Node* n, bool on);
    Node* current() const { return current_; }
    void setCurrent(Node* n);                     // emits currentChanged
    void refreshRows();                           // after adding/removing nodes
    void scrollToNode(Node* n);
    int rowHeight = 20;                           // (read only: TreeWidget::item's height, kept up to date)
    int indent = 16;                              // (read only: TreeWidget::branch's width, kept up to date)
    Signal<Node*> currentChanged;
    Signal<Node*> activated;                      // double-click / Return
    Signal<Node*, MouseEvent&> nodeContextMenu;
    Signal<Node*, MouseEvent&> nodePressed;
    // drags: called on a press-and-move over a node; return the data to drag (empty: no drag)
    std::function<DragData(Node*)> dragDataFor;
    void paint(Painter& p) override;
    bool mousePress(MouseEvent& e) override;
    bool mouseMove(MouseEvent& e) override;
    bool mouseRelease(MouseEvent& e) override;
    bool mouseDoubleClick(MouseEvent& e) override;
    bool wheel(WheelEvent& e) override;
    bool keyPress(KeyEvent& e) override;
    bool contextMenu(MouseEvent& e) override;
    void resized() override;
    void leave() override { hover_ = nullptr; update(); }
    std::string toolTipAt(Point p) const override;
    Node* nodeAt(Point p) const;
    Point sizeHint() const override;
    Point minimumSizeHint() const override;
    int rowCount() const { return (int)rows_.size(); }
    Node* nodeAtRow(int row) const { return row >= 0 && row < (int)rows_.size() ? rows_[row].first : nullptr; }
    int rowOf(const Node* n) const;
    ScrollBar* scrollBar() const { return bar_; }
private:
    bool onArrow(Point p, Node* n) const;
    Rect viewRect() const;
    void rowStates(const Node* n, std::vector<const char*>& out) const;
    bool readStyle();
    Node root_;
    Node* current_ = nullptr;
    Node* hover_ = nullptr;
    Node* pressNode_ = nullptr;
    Point pressPos_;
    std::vector<std::pair<Node*, int>> rows_;     // visible nodes and their depth
    ScrollBar* bar_;
    void rebuild(Node* n, int depth);
    void syncBar();
};

// QTabWidget: tabs on top, one page shown. Styled as `TabWidget`: ::tab (each tab: a box (background-color, border,
// border-radius) with padding around its title (color, font-*)); ::tab-line (a line along the tab's bottom, `height`
// tall: the selected tab's accent); both with :selected, :hover (the tab under the pointer) and :disabled; ::base
// (the line under all the tabs: its `height` and background-color).
class TabWidget : public Widget {
public:
    explicit TabWidget(Widget* parent = nullptr);
    Widget* addTab(std::unique_ptr<Widget> page, const std::string& title);  // returns the page
    template <class T, class... A> T* makeTab(const std::string& title, A&&... a) {
        auto w = std::make_unique<T>(std::forward<A>(a)...);
        T* raw = w.get();
        addTab(std::move(w), title);
        return raw;
    }
    int count() const { return (int)pages_.size(); }
    int currentIndex() const { return current_; }
    void setCurrentIndex(int i);
    Widget* page(int i) const { return pages_[i]; }
    Signal<int> currentChanged;
    void paint(Painter& p) override;
    bool mousePress(MouseEvent& e) override;
    bool mouseMove(MouseEvent& e) override;
    void leave() override { hoverTab_ = -1; update(); }
    void resized() override;
    Point sizeHint() const override;
    Point minimumSizeHint() const override;
    const std::string& tabText(int i) const { return titles_[i]; }
    void setTabText(int i, const std::string& t) { titles_[i] = t; update(); }
    int barHeight() const;
    // (while one tab is being styled: that tab's states, :selected and :hover, instead of the widget's)
    void styleStates(std::vector<std::string>& out) const override;
private:
    std::vector<Widget*> pages_;
    std::vector<std::string> titles_;
    int current_ = -1, hoverTab_ = -1;
    mutable int styledTab_ = -1;  // the tab styleStates() describes: -1 none (the widget), -2 no tab in particular
    int tabAt(Point p) const;
    Rect tabRect(int i) const;
    // a part of tab i ("tab", "tab-line") in that tab's states (-2: no tab in particular: for sizes)
    const css::Style& tabStyle(int i, const std::string& part) const;
};

// QGroupBox: a panel-coloured box with a title above. Styled as `GroupBox`: the box (background-color, border,
// border-radius) starts margin-top below its top; its layout goes inside the box's padding; ::title (color, font-*,
// height, margin: where the title goes above the box).
class GroupBox : public Widget {
public:
    explicit GroupBox(const std::string& title = "", Widget* parent = nullptr);
    void setTitle(const std::string& t) { title_ = t; updateGeometry(); update(); }
    void paint(Painter& p) override;
    Point sizeHint() const override;
    Point minimumSizeHint() const override;
    Rect contentsRect() const override;
    // the layout's area is inside the box (below the title)
    std::string title_;
};

// a thin line (QFrame HLine/VLine, or the toolbar separator). Styled as `Separator`, :vertical or :horizontal: the
// line's `color` and `line-width`, the empty space around it (margin: a vertical one, the toolbar separator, 6 px
// above and below and 3 px beside; a horizontal one none).
class Separator : public Widget {
public:
    explicit Separator(Orientation o = Orientation::Horizontal, Widget* parent = nullptr);
    void paint(Painter& p) override;
    Point sizeHint() const override;
    void styleStates(std::vector<std::string>& out) const override;  // + :vertical / :horizontal
    Orientation orientation;
};

// a draggable divider between two widgets placed by the owner (the Explorer's width; overview height). Styled as
// `SplitterHandle`, :horizontal (moves sideways: its `width`) or :vertical (its `height`): the line along its middle
// (color, line-width), :hover and :pressed.
class SplitterHandle : public Widget {
public:
    explicit SplitterHandle(Orientation o, Widget* parent = nullptr);
    Signal<int> dragged;      // delta px since the press
    Signal<> pressed, released, doubleClicked;
    void paint(Painter& p) override;
    bool mousePress(MouseEvent& e) override;
    bool mouseMove(MouseEvent& e) override;
    bool mouseRelease(MouseEvent& e) override;
    bool mouseDoubleClick(MouseEvent& e) override;
    void enter() override { update(); }
    void leave() override { update(); }
    Point sizeHint() const override;
    void styleStates(std::vector<std::string>& out) const override;  // + :horizontal / :vertical
    // Horizontal: it moves sideways (a vertical line between left and right; cursor ↔)
    Orientation orientation;
private:
    Point start_;
};

// a status line at the bottom of a window (QStatusBar.showMessage). Styled as `StatusBar`: a box (background-color,
// border) with padding around its text (color, font-*: centred over the height below ::separator); ::separator (the
// line along its top: `height`, background-color); ::permanent (the text at the right end: padding, color, font-*).
class StatusBar : public Widget {
public:
    explicit StatusBar(Widget* parent = nullptr);
    void showMessage(const std::string& text, int ms = 0);  // 0: until replaced
    void clearMessage();
    ~StatusBar() override;
    const std::string& message() const { return text_; }
    // (added) the hint line: while following hints, the first line of the tooltip under the pointer in
    // its window (App::hintChanged) replaces the message shown, until the pointer leaves; a timed
    // message (showMessage with ms > 0) wins over a hint while it lasts
    void followHints(bool on);
    bool followsHints() const { return hintSlot_ >= 0; }
    void setHint(const std::string& text);
    const std::string& hint() const { return hint_; }
    std::string shownText() const;  // what it shows: the timed message, else the hint, else the message
    // (added) a text kept at the right end (QStatusBar::addPermanentWidget with a label); "" none
    void setPermanentText(const std::string& text, const std::string& tip = "");
    const std::string& permanentText() const { return permanent_; }
    std::string toolTipAt(Point p) const override;
    void paint(Painter& p) override;
    Point sizeHint() const override;
private:
    std::string text_, permanent_, permanentTip_;
    int permanentW() const;
    int timer_ = -1;
    std::string hint_;
    int hintSlot_ = -1;  // (connected to App::hintChanged)
};

}  // namespace ui
