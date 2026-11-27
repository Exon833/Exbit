// A word wider than a wrapped label's line (a long path, with no spaces in it) is broken: after a '/' or '\' where it
// can be, else between characters, as the TUI does. Its lines stay inside the label, the label grows as tall as
// they are, and a progress dialog showing such a path at its least width keeps it inside the dialog.
// Picture: build/ui-samples/label-long-words/progress-320.png
#include <string>
#include <vector>

#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "gfx/wrap_text_to_width.h"
#include "ui/toolkit/controls.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/widget.h"

using namespace ui;

// whether anything differs from the background (the pixel at bx, by) in the framebuffer's rows y0..y1, columns
// x0..x1 (logical pixels)
static bool inkIn(const Window& w, int x0, int x1, int y0, int y1, int bx, int by) {
    const Image& fb = w.framebuffer();
    float s = w.scale();
    uint32_t bg = fb.pixel((int)(bx * s), (int)(by * s));
    for (int y = (int)(y0 * s); y < (int)(y1 * s) && y < fb.height(); ++y)
        for (int x = (int)(x0 * s); x < (int)(x1 * s) && x < fb.width(); ++x)
            if (fb.pixel(x, y) != bg) return true;
    return false;
}

// whether the lines are each at most `width` wide and are `text` again when joined
static bool fitAndKeepText(const std::vector<std::string>& lines, const Font& f, float width, const std::string& text) {
    std::string joined;
    for (const std::string& line : lines) {
        if (f.width(line) > width) return false;
        joined += line;
    }
    return joined == text;
}

int main() {
    App::get().initHeadless();
    std::string path = "/home/someone/.local/share/exbit/wine-prefixes";
    while (path.size() < 300) path += "/prefix_folder";
    path.resize(300);
    const std::string noBreaks(300, 'x');

    Window w(plat::WindowOptions{});
    w.resizeWindow(260, 600);
    auto* label = w.add<Label>(path);
    label->setWordWrap(true);
    const int width = 200;
    label->setGeometry(10, 10, width, 20);
    Font f = label->font();

    // the path: broken after its slashes, each line inside the label
    std::vector<std::string> lines = wrapTextToWidth(path, f, (float)width);
    CHECK(lines.size() > 3);
    CHECK(fitAndKeepText(lines, f, (float)width, path));
    int afterSlash = 0;
    for (size_t i = 0; i + 1 < lines.size(); ++i) afterSlash += !lines[i].empty() && lines[i].back() == '/';
    CHECK(afterSlash == (int)lines.size() - 1);
    // a word with no slash: broken between characters
    std::vector<std::string> chars = wrapTextToWidth(noBreaks, f, (float)width);
    CHECK(chars.size() > 3 && fitAndKeepText(chars, f, (float)width, noBreaks));
    // words before the path stay on their line; the path starts a line of its own
    std::vector<std::string> mixed = wrapTextToWidth("Installing into " + path, f, (float)width);
    CHECK(!mixed.empty() && mixed[0] == "Installing into");

    // the label asks for every line's height, and paints its last line
    int n = (int)lines.size();
    int least = label->minimumSizeHint().y;
    CHECK(least >= (int)((n - 1) * f.lineSpacing() + f.ascent()));
    label->setGeometry(10, 10, width, least);
    w.showWindow();
    w.update();
    App::get().tick();
    int lastTop = 10 + least - (int)f.lineSpacing();
    CHECK(inkIn(w, 10, 10 + width, lastTop, 10 + least, 255, 595));
    CHECK(!inkIn(w, 10 + width + 1, 255, 10, 10 + least, 255, 595));  // (nothing beyond its right edge)
    w.hideWindow();

    // the install progress dialog: as narrow as the path's lines need, and as tall as its parts at its least width
    ProgressDialog pd(&w, "Installing", "Setting up the Wine prefix " + path, true);
    pd.setRange(0, 0);
    CHECK(pd.width() < 600);
    CHECK(pd.height() == pd.leastSize().y);
    pd.resizeWindow(320, pd.height());
    App::get().doLayouts();
    pd.resizeWindow(320, pd.leastSize().y);
    App::get().doLayouts();
    CHECK(pd.width() == 320);
    Label* shown = nullptr;
    for (auto& c : pd.children())
        if (auto* l = dynamic_cast<Label*>(c.get())) shown = l;
    CHECK(shown != nullptr);
    if (shown) {
        Rect r = shown->geometry();
        CHECK(r.x + r.w <= 320);
        int need = (int)wrapTextToWidth(shown->text(), shown->font(), (float)r.w).size();
        CHECK(need > 3 && r.h >= (int)((need - 1) * f.lineSpacing() + f.ascent()));
    }
    pd.showWindow();
    App::get().doLayouts();
    pd.update();
    App::get().tick();
    util::makeDirs("build/ui-samples/label-long-words");
    CHECK(savePng("build/ui-samples/label-long-words/progress-320.png", pd.framebuffer()));
    pd.cancel();
    TEST_MAIN_END();
}
