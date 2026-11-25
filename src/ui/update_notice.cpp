// The update notice under the master row (update_notice.py).
#include "ui/update_notice.h"

#include <ctime>
#include <thread>

#include "platform/platform.h"
#include "ui/theme.h"
#include "ui/toolkit/style.h"
#include "version.h"

UpdateChecker::~UpdateChecker() { *alive_ = false; }

void UpdateChecker::start() {
    if (*running_) return;
    *running_ = true;
    std::weak_ptr<bool> alive = alive_;
    std::shared_ptr<bool> running = running_;
    update_check::FetchFn fn = fetchFn;
    UpdateChecker* self = this;
    std::thread([alive, running, fn, self] {
        update_check::Result result = update_check::check(fn);
        result.checkedAt = (double)std::time(nullptr);
        plat::postToMain([alive, running, self, result] {
            *running = false;
            auto a = alive.lock();
            if (a && *a) self->done.emit(result);  // (the window closed before GitHub answered: nobody to tell)
        });
    }).detach();
}

namespace update_notice {

namespace {
double clockNow(double now) { return now > 0 ? now : (double)std::time(nullptr); }
std::string fullDate(double t) {  // "2026-11-18 16:49" (local time)
    time_t tt = (time_t)t;
    struct tm lt {};
    localtime_r(&tt, &lt);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &lt);
    return buf;
}
}  // namespace

std::string shortId(const std::string& commit) { return commit.substr(0, 7); }

Text noticeText(const update_check::Result* result, double now) {
    if (!result) return {"Checking for updates…", "", "", ""};
    now = clockNow(now);
    const update_check::Result& r = *result;
    std::string name = version::REPOSITORY;
    name = name.substr(name.rfind('/') + 1);  // (the name alone: the line is narrow)
    bool known = !r.latest.empty();
    std::string when = known && r.latestTime > 0 ? update_check::ago(r.latestTime, now) : "";
    if (r.state == "waiting") {
        if (known) return {"Update available - ", shortId(r.latest), r.url, when.empty() ? "" : " - " + when};
        bool one = r.count == 1;
        return {"", one ? "1 update" : std::to_string(r.count) + " updates", r.url,
                std::string(" ready · rebuild to get ") + (one ? "it" : "them")};
    }
    if (r.state == "current") {
        if (known) return {"", shortId(r.latest), r.latestUrl, when.empty() ? "" : " · " + when};
        return {"Up to date · ", name, r.url, ""};
    }
    if (r.state == "off") {
        if (known) return {"Latest: ", shortId(r.latest), r.latestUrl, when.empty() ? "" : " · " + when};
        return {"Updates off here · ", name, r.url, ""};
    }
    return {"Couldn't check · ", name, r.url, ""};
}

std::string noticeTip(const update_check::Result* result, double now, const update_check::Result* failed) {
    now = clockNow(now);
    std::string state = result ? result->state : "";
    std::string newest;
    if (result && !result->latest.empty())
        newest = shortId(result->latest) + (result->latestTime > 0 ? ", made " + fullDate(result->latestTime) : "");
    std::string tip;
    if (state == "waiting") {
        bool one = result->count == 1;
        tip = (one ? std::string("A new commit is") : std::to_string(result->count) + " new commits are") +
              " on GitHub's main branch" + (newest.empty() ? "" : " (the newest: " + newest + ")") +
              ". Exbit doesn't update itself: pull " + (one ? "it" : "them") +
              " (git pull) and build again (make) when you choose; the link lists what changed";
    } else if (state == "current") {
        tip = "Up to date: this program is GitHub's main branch" + (newest.empty() ? "" : " (its newest commit: " + newest + ")");
    } else if (state == "off") {
        tip = "This build can't tell whether there are updates: it wasn't built from a git checkout of Exbit, or its "
              "commit isn't on GitHub (commits of its own)" +
              (newest.empty() ? std::string() : ". GitHub's newest commit: " + newest);
    } else if (state == "failed") {
        tip = "Couldn't check GitHub for updates (" + result->value + ")";
    } else {
        tip = "Checking GitHub for updates";
    }
    if (result && result->checkedAt > 0)
        tip += ". Checked every 30 minutes; last " + update_check::ago(result->checkedAt, now);
    if (failed) tip += ". The last check failed (" + failed->value + ")";
    return tip;
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
