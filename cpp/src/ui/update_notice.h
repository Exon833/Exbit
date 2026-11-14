// The update notice under the master row (update_notice.py): checks GitHub once, in the
// background, when the program starts (update_check.h), and says what it found: updates waiting
// (to pull and build), up to date, that this build can't tell ("Updates off here"), or that it
// couldn't check. Clicking its link opens the page.
#pragma once
#include <memory>
#include <string>

#include "ui/toolkit/widget.h"
#include "update_check.h"

// Runs update_check::check() on a thread of its own; `done` is emitted on the GUI thread
// (plat::postToMain). Destroying the checker before GitHub answered just drops the answer.
class UpdateChecker {
public:
    UpdateChecker() = default;
    ~UpdateChecker();
    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;
    void start();
    ui::Signal<const update_check::Result&> done;
    // tests: check with this instead of GitHub
    update_check::FetchFn fetchFn;

private:
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

namespace update_notice {
// What the notice says (Python's notice_text, which returned rich text): `before` in the dimmed
// colour, then `link` underlined (opens `url`), then `after`. result null: still checking.
struct Text {
    std::string before, link, url, after;
    std::string plain() const { return before + link + after; }
};
Text noticeText(const update_check::Result* result);
std::string noticeTip(const update_check::Result* result);  // its tooltip: more than its line has room for
}  // namespace update_notice

// The notice itself: one line of dimmed 8 pt text, its link part underlined and clickable.
class UpdateNotice : public ui::Widget {
public:
    UpdateNotice();
    void setNotice(const update_notice::Text& t);
    const update_notice::Text& notice() const { return text_; }
    Point sizeHint() const override;
    Point minimumSizeHint() const override;
    void paint(Painter& p) override;
    bool mousePress(ui::MouseEvent& e) override;
    bool mouseMove(ui::MouseEvent& e) override;
    void leave() override;
    std::string toolTipAt(Point p) const override;  // over the link: its address

private:
    update_notice::Text text_;
    Rect linkRect() const;
};
