// Dialogs: Dialog (QDialog: modal exec() or modeless show), the standard buttons row
// (QDialogButtonBox), message boxes, text input, a progress window, and Exbit's own file
// chooser (open / save / folder), drawn in the skin like everything else.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/toolkit/controls.h"
#include "ui/toolkit/widget.h"

namespace ui {

class Dialog : public Window {
public:
    explicit Dialog(Window* parent, const std::string& title = "", int w = 400, int h = 300);
    enum Result { Rejected = 0, Accepted = 1 };
    // shows it modal and waits until accept() / reject() / closed; returns the result
    int exec();
    void open();                  // shows it modeless (returns at once); finished(result) when done
    void accept() { done(Accepted); }
    void reject() { done(Rejected); }
    virtual void done(int result);
    int result() const { return result_; }
    Signal<int> finished;
    Window* parentWindow() const { return parent_; }
    // fit the window to its layout's size hint (call after building it)
    void adjustSize();
    bool keyPress(KeyEvent& e) override;  // Escape: reject; Return: the default button
    void close() override;                // the title bar's ✕ (closeRequested first): reject
    bool isRunning() const { return running_; }
private:
    Window* parent_;
    int result_ = Rejected;
    bool running_ = false, placed_ = false;
    static plat::WindowOptions makeOptions(Window* parent, const std::string& title, int w, int h);
    void place();
};

// the row of buttons at the bottom of a dialog (OK / Cancel / Close / Save / Discard / Yes / No / Apply)
class ButtonBox : public Widget {
public:
    enum StandardButton { Ok = 1, Cancel = 2, Close = 4, Save = 8, Discard = 16, Yes = 32, No = 64, Apply = 128, Reset = 256 };
    // Order as on Windows / KDE: [Reset] … Yes, OK / Save, Don't Save, No, Cancel / Close, Apply; the
    // accept button (OK / Save / Yes) is the default one
    explicit ButtonBox(int buttons, Widget* parent = nullptr);
    Button* button(StandardButton b) const;
    Button* addButton(const std::string& text, int role /* 1 accept, 0 reject, 2 other */);
    Signal<> accepted, rejected;
    Signal<int> clicked;          // the StandardButton (0 for added ones)
    Point sizeHint() const override;
    static std::string standardText(StandardButton b, int all);  // "OK", "Cancel", "Don't Save"…
private:
    std::vector<std::pair<int, Button*>> buttons_;
    std::vector<int> roles_;      // per button: its place in the order
    void arrange();
};

// a dialog's own OK / Cancel / Close… button outside a ButtonBox: class .dialog-button, at least its min-width wide
// (default.css: as a ButtonBox's buttons, 80 px)
void dialogButton(Button* b);

namespace msg {
enum Button { Ok = 1, Cancel = 2, Yes = 4, No = 8, Save = 16, Discard = 32, Close = 64 };
enum class Icon { None, Information, Warning, Critical, Question };
// returns the button pressed (Escape / ✕: the Cancel/No/Close one present)
int box(Window* parent, Icon icon, const std::string& title, const std::string& text, int buttons, int defaultButton = 0,
        const std::string& details = "");
inline int question(Window* parent, const std::string& title, const std::string& text, int buttons = Yes | No, int def = 0) {
    return box(parent, Icon::Question, title, text, buttons, def);
}
inline void information(Window* parent, const std::string& title, const std::string& text) {
    box(parent, Icon::Information, title, text, Ok, Ok);
}
inline void warning(Window* parent, const std::string& title, const std::string& text) {
    box(parent, Icon::Warning, title, text, Ok, Ok);
}
inline void critical(Window* parent, const std::string& title, const std::string& text) {
    box(parent, Icon::Critical, title, text, Ok, Ok);
}
}  // namespace msg

// QInputDialog.getText / getItem
bool getText(Window* parent, const std::string& title, const std::string& label, std::string& text);
bool getItem(Window* parent, const std::string& title, const std::string& label, const std::vector<std::string>& items,
             int& index);

// a "please wait" / progress window (QProgressDialog): stays responsive through App::processEvents()
class ProgressDialog : public Dialog {
public:
    ProgressDialog(Window* parent, const std::string& title, const std::string& label, bool cancellable = true);
    void setLabel(const std::string& t);
    void setRange(int lo, int hi);       // (0, 0): busy, no percentage
    void setValue(int v);                // repaints and handles pending events
    bool wasCanceled() const { return canceled_; }
    void paint(Painter& p) override;
    void cancel();                       // as the Cancel button
    int value() const { return value_; }
    int minimum() const { return lo_; }
    int maximum() const { return hi_; }
    Signal<> canceled;
    bool keyPress(KeyEvent& e) override;
    void close() override { cancel(); }
    ~ProgressDialog() override;
private:
    Label* label_;
    Button* cancel_ = nullptr;
    int lo_ = 0, hi_ = 100, value_ = 0;
    bool canceled_ = false;
    Widget* bar_;
    int frameId_ = -1;
    void paintBar(Painter& p);
};

// Exbit's own file chooser. `filter` as Qt writes it: "Audio and MIDI (*.wav *.mid);;All files (*)".
// Remembers the last folder per `key` (in the settings) when given.
namespace files {
std::string openFile(Window* parent, const std::string& title, const std::string& dir, const std::string& filter);
std::vector<std::string> openFiles(Window* parent, const std::string& title, const std::string& dir,
                                   const std::string& filter);
// asks before overwriting; adds the filter's first extension when the name has none
std::string saveFile(Window* parent, const std::string& title, const std::string& dir, const std::string& filter,
                     std::string* chosenFilter = nullptr);
std::string folder(Window* parent, const std::string& title, const std::string& dir);
}  // namespace files

}  // namespace ui
