// The update notice under the master row (update_notice.py).
#include "ui/update_notice.h"

#include <thread>

#include "platform/platform.h"
#include "ui/theme.h"
#include "ui/toolkit/style.h"
#include "version.h"

UpdateChecker::~UpdateChecker() { *alive_ = false; }

void UpdateChecker::start() {
    std::weak_ptr<bool> alive = alive_;
    update_check::FetchFn fn = fetchFn;
    UpdateChecker* self = this;
    std::thread([alive, fn, self] {
        update_check::Result result = update_check::check(fn);
        plat::postToMain([alive, self, result] {
            auto a = alive.lock();
            if (a && *a) self->done.emit(result);  // (the window closed before GitHub answered: nobody to tell)
        });
    }).detach();
}

namespace update_notice {

Text noticeText(const update_check::Result* result) {
    if (!result) return {"Checking for updates…", "", "", ""};
    const std::string& state = result->state;
    std::string name = version::REPOSITORY;
    name = name.substr(name.rfind('/') + 1);  // (the name alone: the line is narrow)
    if (state == "waiting") {
        bool one = result->count == 1;
        return {"", one ? "1 update" : std::to_string(result->count) + " updates", result->url,
                std::string(" ready · rebuild to get ") + (one ? "it" : "them")};
    }
    if (state == "current") return {"Up to date · ", name, result->url, ""};
    if (state == "off") return {"Updates off here · ", name, result->url, ""};
    return {"Couldn't check · ", name, result->url, ""};
}

std::string noticeTip(const update_check::Result* result) {
    std::string state = result ? result->state : "";
    if (state == "waiting")
        return "New commits are on GitHub's main branch. Pull them (git pull) and build Tiny DAW again (make) to "
               "get them; the link lists what changed";
    if (state == "off")
        return "This build can't tell whether there are updates: it wasn't built from a git checkout of "
               "Tiny-DAW-Linux, or its commit isn't on GitHub (commits of its own)";
    if (state == "failed") return "Couldn't check GitHub for updates when the program started (" + result->value + ")";
    return "Checked on GitHub when the program starts: the commit this program was built from against the main branch";
}

}  // namespace update_notice

UpdateNotice::UpdateNotice() {
    setStyleType("UpdateNotice");
    setProperty("dimmed", "true");  // the skin's dimmed text
    setMouseTracking(true);
    setNotice(update_notice::noticeText(nullptr));
}

void UpdateNotice::setNotice(const update_notice::Text& t) {
    text_ = t;
    updateGeometry();
    update();
}

Font UpdateNotice::font() const { return ui::style::font(computedStyle(), theme::dimmedFont()); }

Point UpdateNotice::sizeHint() const {
    Font f = font();
    return Point((int)std::ceil(f.width(text_.plain())), (int)std::ceil(f.ascent() + f.descent()));
}

Point UpdateNotice::minimumSizeHint() const { return Point(16, sizeHint().y); }

Rect UpdateNotice::linkRect() const {
    if (text_.link.empty()) return Rect();
    Font f = font();
    int x0 = (int)f.width(text_.before);
    int x1 = (int)std::ceil(f.width(text_.before + text_.link));  // (`after` follows it, not underlined)
    return Rect(x0, 0, std::max(0, std::min(width(), x1) - x0), height());
}

void UpdateNotice::paint(Painter& p) {
    Font f = font();
    Color c = ui::style::foreground(computedStyle());  // (the link too: the Python program styled it so)
    RectF box(0, 0, (float)width(), (float)height());
    p.drawText(box, AlignLeft | AlignVCenter, text_.plain(), f, c, true);
    Rect lr = linkRect();
    if (!lr.empty()) {
        float th = f.ascent() + f.descent();
        int y = (int)std::floor((height() - th) / 2 + f.ascent() + 1.5f);
        p.drawHLine(lr.x, lr.right(), y, ui::style::foreground(computedStyle("link")));
    }
}

bool UpdateNotice::mousePress(ui::MouseEvent& e) {
    if (e.button == plat::LeftButton && linkRect().contains(e.pos)) {
        plat::openUrl(text_.url);
        return true;
    }
    return false;
}

bool UpdateNotice::mouseMove(ui::MouseEvent& e) {
    setCursor(linkRect().contains(e.pos) ? ui::Cursor::Hand : ui::Cursor::Arrow);
    return false;
}

void UpdateNotice::leave() { setCursor(ui::Cursor::Arrow); }

std::string UpdateNotice::toolTipAt(Point p) const { return toolTip(); }
