// The update notice under the master row (update_notice.py): checks GitHub in the background when
// the program starts and every half hour after (update_check.h CHECK_EVERY_S), and says what it
// found: the main branch's newest commit and how long ago it was made ("b80d1db · 2 days ago") when
// this program is it, "Update available - 3f9a2c1 - 4 hours ago" when there is a newer one (never
// installed here: pulling and building it is yours to do), the newest commit alone when this build
// can't compare. A check that fails after one that worked leaves the notice as it was (its tooltip
// says). How long ago is counted again every minute. Clicking its link opens the commit's page.
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
    void start();  // (one at a time: while GitHub hasn't answered, asking again does nothing)
    bool running() const { return *running_; }
    ui::Signal<const update_check::Result&> done;
    // tests: check with this instead of GitHub
    update_check::FetchFn fetchFn;

private:
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::shared_ptr<bool> running_ = std::make_shared<bool>(false);
};

namespace update_notice {
// What the notice says (Python's notice_text, which returned rich text): `before` in the dimmed
// colour, then `link` underlined (opens `url`), then `after`. result null: still checking.
struct Text {
    std::string before, link, url, after;
    std::string plain() const { return before + link + after; }
};
// now: seconds since the epoch (how long ago things were); 0: the clock's
Text noticeText(const update_check::Result* result, double now = 0);
// its tooltip: more than its line has room for; failed: the last check's failure while an earlier answer is shown
std::string noticeTip(const update_check::Result* result, double now = 0, const update_check::Result* failed = nullptr);
std::string shortId(const std::string& commit);  // its first 7 characters (GitHub's short id)
}  // namespace update_notice

// The notice itself: one line of dimmed 8 pt text, its link part underlined and clickable. Styled as UpdateNotice
// (default.css): color and font; ::link, the line under the link (color).
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
    Font font() const;  // (the stylesheets' over the dimmed 8 pt font)
};
