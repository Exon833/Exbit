// The track panel and the top bar, headless: level meter dB mapping, decay and bars; output pairs;
// LUFS text; the CPU monitor; version comparison and the update check (with a fake GitHub); the
// update notice; the master strip's settings; the transport bar (tap tempo through its shortcut,
// play/pause, timing); the lane panel's rows, meters, outputs and menus; boxes a skin can give the track rows, the
// timeline's frame, the Explorer's title bar, meters and the EQ's grid labels. Paints the panel, the
// master strip, the transport bar and the CPU monitor into PNGs in build/ui-samples/panel/.
#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>

#include "audio/engine.h"
#include "check.h"
#include "core/util.h"
#include "gfx/image.h"
#include "model/timing.h"
#include "ui/cpu_monitor.h"
#include "ui/lane_panel.h"
#include "ui/level_meter.h"
#include "ui/loudness_display.h"
#include "ui/master_strip.h"
#include "ui/theme.h"
#include "ui/timeline.h"
#include "ui/toolkit/menu.h"
#include "ui/transport_bar.h"
#include "ui/update_notice.h"
#include "ui/widgets.h"
#include "ui/formatting.h"
#include "ui/track_dialogs.h"
#include "update_check.h"

#include "ui/automation_dialog.h"
#include "core/css.h"
#include "effects/effects.h"
#include "ui/colors.h"
#include "ui/effect_window.h"
#include "ui/explorer.h"

namespace {

std::string outDir() {
    std::string d = "build/ui-samples/panel";
    util::makeDirs(d);
    return d;
}

std::string outDir2() {  // (the successor's features)
    std::string d = "build/ui-samples/panel2";
    util::makeDirs(d);
    return d;
}

template <class T>
std::vector<T*> findAll(ui::Widget* root) {
    std::vector<T*> out;
    std::function<void(ui::Widget*)> walk = [&](ui::Widget* w) {
        for (auto& c : w->children()) {
            if (auto* t = dynamic_cast<T*>(c.get())) out.push_back(t);
            walk(c.get());
        }
    };
    walk(root);
    return out;
}

ui::Button* buttonText(ui::Widget* root, const std::string& text) {
    for (auto* b : findAll<ui::Button>(root))
        if (b->text() == text) return b;
    return nullptr;
}

ui::Action* actionStarting(const ui::Menu& m, const std::string& prefix) {
    for (auto& a : m.actions())
        if (a->text.compare(0, prefix.size(), prefix) == 0) return a.get();
    return nullptr;
}

Image paintWidget(ui::Widget* w, Color bg = theme::c("window")) {
    ui::App::get().doLayouts();
    Image img(std::max(1, w->width()), std::max(1, w->height()), bg);
    Painter p(img);
    w->paintTree(p, w->rect());
    return img;
}

void save(ui::Widget* w, const std::string& name, Color bg = theme::c("window")) {
    savePng(outDir() + "/" + name + ".png", paintWidget(w, bg));
}

std::vector<std::string> menuTexts(const ui::Menu& m) {
    std::vector<std::string> out;
    for (auto& a : m.actions()) out.push_back(a->separator ? "---" : a->text);
    return out;
}

bool hasText(const std::vector<std::string>& v, const std::string& s) {
    for (auto& x : v)
        if (x == s) return true;
    return false;
}

}  // namespace

static void testMeterLogic() {
    using namespace level_meter;
    CHECK_NEAR(dbFraction(-60), 0, 1e-12);
    CHECK_NEAR(dbFraction(0), 1, 1e-12);
    CHECK_NEAR(dbFraction(-30), 0.5, 1e-12);
    CHECK_NEAR(dbFraction(-90), 0, 1e-12);
    CHECK_NEAR(dbFraction(6), 1, 1e-12);
    CHECK(outputLabel({4, 5}) == "outputs 5 and 6");
    CHECK(outputLabel({0}) == "output 1");
    CHECK(outputLabel({0, 1, 2}) == "outputs 1, 2 and 3");
    std::vector<float> a{0.2f, 0.1f}, b{0.1f, 0.5f, 0.3f};
    auto l = loudest({&a, nullptr, &b});
    CHECK(l.size() == 3 && l[0] == 0.2f && l[1] == 0.5f && l[2] == 0.3f);
    CHECK(loudest({}).empty());

    LevelMeter m(2);
    CHECK(m.shown() == std::vector<int>({0, 1}));
    CHECK(m.toolTip() == "Peak level of outputs 1 and 2, top to bottom (the thin line: the last peak; the thin bar "
                         "inside: RMS, the average level).\n"
                         "Loudest since Play: nothing yet. Click to clear it.");
    m.showChannels({4, 5});  // the device lacks them: output 1
    CHECK(m.shown() == std::vector<int>({0}));
    m.setChannels(8);  // a bigger device: the outputs asked for come back
    CHECK(m.shown() == std::vector<int>({4, 5}));
    std::vector<float> peaks{0, 0, 0, 0, 1.0f, 0.5f};
    m.push(peaks);
    CHECK_NEAR(m.levels()[0], 1.0, 1e-6);
    CHECK_NEAR(m.levels()[1], 0.5, 1e-6);
    m.push(nullptr);  // silence: falls back slowly
    CHECK_NEAR(m.levels()[0], 0.85, 1e-6);
    std::vector<float> small{0, 0, 0, 0, 0.9f};  // (shorter than the outputs: the rest are 0)
    m.push(small);
    CHECK_NEAR(m.levels()[0], 0.9, 1e-6);
    CHECK_NEAR(m.levels()[1], 0.5 * 0.85 * 0.85, 1e-6);
    for (int i = 0; i < 80; ++i) m.push(nullptr);
    CHECK(m.levels()[0] == 0.0 && m.levels()[1] == 0.0);  // settled

    m.resize(200, 20);
    auto r = m.barRects();  // 2 bars: 1 px apart, at most a tenth of the width thick
    CHECK(r.size() == 2 && r[0] == std::make_pair(0, 9) && r[1] == std::make_pair(10, 9));
    m.resize(50, 40);  // thickness limited to 5 px, centred
    r = m.barRects();
    CHECK(r[0] == std::make_pair(14, 5) && r[1] == std::make_pair(20, 5));
    LevelMeter many(16);
    many.showChannels({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    many.resize(200, 10);  // no room for gaps or even 1 px each: spread over the height
    r = many.barRects();
    CHECK(r.size() == 16 && r[0].first == 0 && r[15].first == 9 && r[15].second == 1);
}

static void testMeterHolds() {
    // held peaks (HOLD_S), the loudest since Play, the clip light and its clearing
    using namespace level_meter;
    testNow = 100.0;
    LevelMeter m(2);
    m.resize(200, 20);
    std::vector<float> peak{0.5f, 0.25f};
    m.push(peak);
    CHECK_NEAR(m.held()[0], 0.5, 1e-6);
    CHECK_NEAR(m.loudest(), 0.5, 1e-6);
    CHECK(!m.clipped());
    CHECK(m.toolTip().find("Loudest since Play: -6.0 dBFS. Click") != std::string::npos);
    testNow = 101.0;  // within HOLD_S: the held line stays while the bar falls
    m.push(nullptr);
    CHECK_NEAR(m.held()[0], 0.5, 1e-6);
    CHECK_NEAR(m.levels()[0], 0.5 * config::METER_DECAY, 1e-6);
    testNow = 101.6;  // past HOLD_S: it falls too
    m.push(nullptr);
    CHECK_NEAR(m.held()[0], 0.5 * config::METER_DECAY, 1e-6);
    CHECK_NEAR(m.loudest(), 0.5, 1e-6);  // (the loudest stays until Play or a click)
    // a track clips over 0 dB, the master from -0.1 dB
    std::vector<float> full{1.0f, 0.2f}, over{1.05f, 0.2f};
    m.push(full);
    CHECK(!m.clipped());
    m.push(over);
    CHECK(m.clipped() && m.toolTip().find("clipping") != std::string::npos);
    m.push(nullptr);
    CHECK(m.clipped());  // (stays lit)
    LevelMeter master(2, MASTER_CLIP_DB);
    master.push(full);
    CHECK(master.clipped());
    // the number at the right end, when there is room
    CHECK(m.barsW() == 200 - NUMBER_W);
    m.resize(80, 20);
    CHECK(m.barsW() == 80);
    m.resize(200, 20);
    savePng(outDir() + "/meter-clipped.png", paintWidget(&m));
    // a click clears it
    ui::MouseEvent e;
    e.button = plat::LeftButton;
    m.mousePress(e);
    CHECK(!m.clipped() && m.loudest() == 0.0);
    // a rebuilt row's meter goes on from the old one
    m.push(over);
    LevelMeter next(2);
    next.takeOver(m);
    CHECK(next.clipped() && next.loudest() > 1.0 && next.levels() == m.levels());
    savePng(outDir() + "/meter-held.png", paintWidget(&m));
    testNow = -1;
}

static void testSmallPieces() {
    auto pairs = widgets::outputPairs(5);
    CHECK(pairs.size() == 3 && pairs[0].second == "1/2" && pairs[1].first == 2 && pairs[1].second == "3/4" &&
          pairs[2].second == "5");
    CHECK(widgets::outputPairs(2).size() == 1);
    CHECK(loudness_display::fmtLufs(-INFINITY) == "–");
    CHECK(loudness_display::fmtLufs(-23.44) == "-23.4");
    CHECK(loudness_display::text(-9.05, -INFINITY) == "S  -9.1\nI     –" ||
          loudness_display::text(-9.05, -INFINITY) == "S  -9.0\nI     –");
    LoudnessDisplay d;
    d.showValues(-14.2, -18.0, -20.5);
    CHECK(d.text() == "S -18.0\nI -20.5");
    CHECK(d.toolTip().find("momentary, last 0.4 s: -14.2") != std::string::npos);
    CHECK(widgets::effectsSummary({}) == "No effects yet: click to add some");
    auto s1 = std::make_shared<PluginSlot>();
    s1->name = "EQ";
    auto s2 = std::make_shared<PluginSlot>();
    s2->name = "Reverb";
    s2->bypass = true;
    CHECK(widgets::effectsSummary({s1, s2}) == "Effects: EQ, Reverb (bypassed)");

    CHECK(cpu_monitor::loadColor(0.9) == theme::c("meter_hot"));
    CHECK(cpu_monitor::loadColor(0.5) == theme::c("meter_warn"));
    CHECK(cpu_monitor::loadColor(0.2) == theme::c("meter_ok"));
    CpuMonitor cpu;
    CHECK(cpu.width() == config::PANEL_W && cpu.height() == config::RULER_H);
    save(&cpu, "cpu-idle");
    cpu.showLoad(std::make_pair(0.32, 0.25));
    save(&cpu, "cpu-normal");
    cpu.showLoad(std::make_pair(0.92, 0.61), 3);
    CHECK(cpu.toolTip().find("likely clicks): 3") != std::string::npos);
    CHECK(cpu.toolTip().find("avg = rolling ~5 s") != std::string::npos);
    save(&cpu, "cpu-late");

    // the volume slider and its reading
    ui::Widget holder;
    double set = 99;
    auto* slider = holder.add<VolumeSlider>(-6.5, [&](double db) { set = db; });
    CHECK(slider->value() == -6 && set == 99);  // (round half to even; no call while building)
    auto* reading = holder.add<DbReading>(slider);
    CHECK(reading->text() == "-6 dB");
    slider->setValue(3);
    CHECK(set == 3 && reading->text() == "+3 dB");
    CHECK(slider->toolTip() == "Volume +3 dB (double-click: 0 dB)");
    slider->doubleClicked.emit();
    CHECK(set == 0 && slider->value() == 0);
    slider->setValue(config::MIN_DB);
    CHECK(reading->text() == "-inf dB");
}


// GitHub, as the check asks it: the comparison with main (`answer`, or an HTTP error), and main's newest commit
static const char* const NEWEST = "3f9a2c1d00000000000000000000000000000000";
static Json fakeCompare(const std::string& url, const std::string& answer, int status) {
    if (url.size() > 13 && url.substr(url.size() - 13) == "/commits/main")
        return Json::parse(std::string(R"({"sha": ")") + NEWEST + R"(", "commit": {"committer": {"date": "2026-11-18T14:49:29Z"}}})");
    CHECK(url.find("/compare/") != std::string::npos && url.size() > 7 && url.substr(url.size() - 7) == "...main");
    if (status) throw update_check::FetchError("HTTP Error " + std::to_string(status) + ": Forbidden", status);
    return Json::parse(answer);
}

static void testUpdateCheck() {
    using namespace update_check;
    CHECK(parse("v0.2.1") == std::vector<int>({0, 2, 1}));  // (the numbered releases' helpers, still there)
    CHECK(isNewer("0.4.1", "0.4") && !isNewer("junk", "0.4"));
    CHECK(API == "https://api.github.com/repos/Exon833/Exbit");
    CHECK(PAGE == "https://github.com/Exon833/Exbit");
    CHECK(CHECK_EVERY_S == 30 * 60);
    // GitHub's times, and how long ago they were
    const double made = 1795013369;  // 2026-11-18 14:49:29 UTC
    CHECK(parseTime("2026-11-18T14:49:29Z") == made);
    CHECK(parseTime("2026-11-18T16:49:29+02:00") == made && parseTime("2026-11-18T09:49:29-05:00") == made);
    CHECK(parseTime("2026-11-18T14:49:29.123Z") == made);
    CHECK(parseTime("yesterday") == 0);
    CHECK(ago(made, made + 30) == "just now" && ago(made, made - 600) == "just now");  // (a wrong clock: never negative)
    CHECK(ago(made, made + 60) == "1 minute ago" && ago(made, made + 59 * 60 + 59) == "59 minutes ago");
    CHECK(ago(made, made + 3600) == "1 hour ago" && ago(made, made + 5 * 3600 + 10) == "5 hours ago");
    CHECK(ago(made, made + 86400) == "1 day ago" && ago(made, made + 29 * 86400) == "29 days ago");
    CHECK(ago(made, made + 31 * 86400) == "1 month ago" && ago(made, made + 200 * 86400) == "6 months ago");
    CHECK(ago(made, made + 366 * 86400) == "1 year ago" && ago(made, made + 3 * 366 * 86400) == "3 years ago");
    const std::string commit = "0123456789abcdef0123456789abcdef01234567";
    // the build's commit: a full id, or nothing (built outside a git checkout)
    std::string built = builtFrom();
    CHECK(built.empty() || built.size() == 40);
    // no commit known: off
    Result r = update_check::check([](const std::string& u) -> Json { throw FetchError("should not be asked"); }, "");
    CHECK(r.state == "off" && r.url == PAGE);
    // the main branch is ahead: updates waiting
    r = update_check::check([](const std::string& u) { return fakeCompare(u, R"({"status": "ahead", "ahead_by": 3})", 0); }, commit);
    CHECK(r.state == "waiting" && r.count == 3 && r.value == "3" && r.url == PAGE + "/commits/main");
    CHECK(r.latest == NEWEST && r.latestTime == made && r.latestUrl == PAGE + "/commit/" + NEWEST);  // (main's newest)
    r = update_check::check([](const std::string& u) { return fakeCompare(u, R"({"status": "diverged", "ahead_by": 1})", 0); }, commit);
    CHECK(r.state == "waiting" && r.count == 1);
    // the same commit, or one ahead of main: up to date
    r = update_check::check([](const std::string& u) { return fakeCompare(u, R"({"status": "identical", "ahead_by": 0})", 0); }, commit);
    CHECK(r.state == "current" && r.url == PAGE);
    r = update_check::check([](const std::string& u) { return fakeCompare(u, R"({"status": "behind", "ahead_by": 0, "behind_by": 2})", 0); },
              commit);
    CHECK(r.state == "current");
    // a commit GitHub doesn't know: off
    r = update_check::check([](const std::string& u) { return fakeCompare(u, "", 404); }, commit);
    CHECK(r.state == "off");
    r = update_check::check([](const std::string& u) { return fakeCompare(u, "", 422); }, commit);
    CHECK(r.state == "off");
    // GitHub refuses, can't be reached, or answers strangely
    r = update_check::check([](const std::string& u) { return fakeCompare(u, "", 403); }, commit);
    CHECK(r.state == "failed" && r.value == "HTTP Error 403: Forbidden" && r.url == PAGE);
    r = update_check::check([](const std::string& u) -> Json { throw FetchError("could not resolve api.github.com (no network?)"); }, commit);
    CHECK(r.state == "failed" && r.value == "could not resolve api.github.com (no network?)");
    r = update_check::check([](const std::string& u) { return fakeCompare(u, "[1, 2]", 0); }, commit);
    CHECK(r.state == "failed" && r.value == "GitHub answered strangely");
    r = update_check::check([](const std::string& u) { return fakeCompare(u, R"({"status": "ahead", "ahead_by": "x"})", 0); }, commit);
    CHECK(r.state == "failed");

    // main's newest commit not to be had (its request refused): the comparison stands alone
    r = update_check::check([](const std::string& u) -> Json {
        if (u.find("/commits/main") != std::string::npos) throw FetchError("HTTP Error 403: Forbidden", 403);
        return fakeCompare(u, R"({"status": "identical"})", 0);
    }, commit);
    CHECK(r.state == "current" && r.latest.empty() && r.latestTime == 0);
    // a build that can't compare still learns the newest commit
    r = update_check::check([](const std::string& u) { return fakeCompare(u, "", 404); }, commit);
    CHECK(r.state == "off" && r.latest == NEWEST);

    // the notice's text: up to date, the newest commit and how long ago; an update: "Update available - id - ago"
    auto t = update_notice::noticeText(nullptr);
    CHECK(t.plain() == "Checking for updates…" && t.link.empty());
    Result fresh{"current", "", PAGE, 0, NEWEST, made, PAGE + "/commit/" + NEWEST};
    t = update_notice::noticeText(&fresh, made + 2 * 86400 + 5);
    CHECK(t.plain() == "3f9a2c1 · 2 days ago" && t.link == "3f9a2c1" && t.url == PAGE + "/commit/" + NEWEST);
    Result update{"waiting", "2", PAGE + "/commits/main", 2, NEWEST, made, PAGE + "/commit/" + NEWEST};
    t = update_notice::noticeText(&update, made + 4 * 3600);
    CHECK(t.plain() == "Update available - 3f9a2c1 - 4 hours ago" && t.link == "3f9a2c1" && t.url == PAGE + "/commits/main");
    CHECK(update_notice::noticeText(&update, made + 4 * 3600 + 60 * 40).plain() == "Update available - 3f9a2c1 - 4 hours ago");
    CHECK(update_notice::noticeText(&update, made + 5 * 3600).plain() == "Update available - 3f9a2c1 - 5 hours ago");
    Result unknown{"off", "", PAGE, 0, NEWEST, made, PAGE + "/commit/" + NEWEST};
    CHECK(update_notice::noticeText(&unknown, made + 90).plain() == "Latest: 3f9a2c1 · 1 minute ago");
    // the tooltip: what was found, never installing; when it was checked; a later check's failure
    update.checkedAt = made + 4 * 3600 - 120;
    std::string tip = update_notice::noticeTip(&update, made + 4 * 3600);
    CHECK(tip.find("2 new commits") != std::string::npos && tip.find("doesn't update itself") != std::string::npos &&
          tip.find("git pull") != std::string::npos && tip.find("last 2 minutes ago") != std::string::npos);
    Result offline{"failed", "could not resolve api.github.com (no network?)", PAGE};
    CHECK(update_notice::noticeTip(&fresh, made, &offline).find("The last check failed (could not resolve") != std::string::npos);
    // (without the newest commit: the words of before)
    Result waiting{"waiting", "3", PAGE + "/commits/main", 3};
    t = update_notice::noticeText(&waiting);
    CHECK(t.before.empty() && t.link == "3 updates" && t.after == " ready · rebuild to get them" &&
          t.url == PAGE + "/commits/main");
    Result one{"waiting", "1", PAGE + "/commits/main", 1};
    CHECK(update_notice::noticeText(&one).plain() == "1 update ready · rebuild to get it");
    Result current{"current", "", PAGE};
    CHECK(update_notice::noticeText(&current).plain() == "Up to date · Exbit");
    Result off{"off", "", PAGE};
    CHECK(update_notice::noticeText(&off).plain() == "Updates off here · Exbit");
    Result failed{"failed", "timeout", PAGE};
    CHECK(update_notice::noticeText(&failed).plain() == "Couldn't check · Exbit");
    CHECK(update_notice::noticeTip(&failed) == "Couldn't check GitHub for updates (timeout)");
    CHECK(update_notice::noticeTip(&waiting).find("git pull") != std::string::npos);

    // the checker runs on a thread and answers on the GUI thread (with this build's own commit)
    UpdateChecker checker;
    checker.fetchFn = [](const std::string& u) { return fakeCompare(u, R"({"status": "ahead", "ahead_by": 7})", 0); };
    std::optional<Result> got;
    checker.done.connect([&](const Result& res) { got = res; });
    checker.start();
    CHECK(checker.running());
    int answers = 0;
    checker.done.connect([&](const Result&) { answers++; });
    checker.start();  // (while GitHub hasn't answered: nothing more)
    double until = plat::now() + 5;
    while (!got && plat::now() < until) plat::processEvents(20);  // (headless: run the posted work)
    CHECK(got && (built.empty() ? got->state == "off" : got->state == "waiting" && got->count == 7));
    CHECK(got && got->latest == NEWEST && got->checkedAt > 0);
    CHECK(answers == 1 && !checker.running());
}

static void testTransportBar() {
    ui::Window win(plat::WindowOptions{});
    win.resizeWindow(1280, 40);
    auto* bar = win.add<TransportBar>();
    Point hint = bar->sizeHint();
    bar->setGeometry(0, 0, 1280, hint.y);
    win.showWindow();
    ui::App::get().doLayouts();
    CHECK(hint.y >= config::TRANSPORT_BUTTON_PX + 4);
    CHECK(bar->middleButtons.size() == 7);
    CHECK(bar->middleButtons[5]->iconName() == "keyboard" && bar->middleButtons[6]->iconName() == "capture");
    CHECK(bar->addReturnTrackAction->button->text() == "+ Return" && bar->addReturnTrackAction->text == "Return track");
    CHECK(bar->playButton == bar->middleButtons[0]);
    CHECK(bar->recordButton->iconName() == "record");
    CHECK(bar->followAction->isChecked());
    // the playback buttons sit in the middle of the bar (when the sides fit beside them)
    bar->setGeometry(0, 0, 1800, hint.y);
    ui::App::get().doLayouts();
    int mid = bar->middleButtons.front()->mapTo(bar, Point(0, 0)).x;
    int end = bar->middleButtons.back()->mapTo(bar, Point(0, 0)).x + config::TRANSPORT_BUTTON_PX;
    CHECK(std::abs((mid + end) / 2 - 900) <= 2);
    bar->setGeometry(0, 0, 1280, hint.y);
    ui::App::get().doLayouts();
    // icons from the data folder: Loop has none
    CHECK(!transport_bar::tinted("play", Color(255, 255, 255), 14).empty());
    CHECK(!transport_bar::tinted("keyboard", Color(255, 255, 255), 14).empty());
    CHECK(!transport_bar::tinted("capture", Color(255, 255, 255), 14).empty());
    CHECK(transport_bar::tinted("no-such-icon", Color(255, 255, 255), 14).empty());

    bar->setPlaying(true);
    CHECK(bar->playButton->iconName() == "pause" && bar->playAction->text == "Pause");
    CHECK(bar->playAction->toolTip() == "Pause (Space)");
    save(bar, "transport-playing");
    bar->setPlaying(false);
    CHECK(bar->playButton->iconName() == "play");

    // tap tempo: the T key presses Tap
    TapTempo tap;
    double clock = 10;
    double bpm = 0;
    int taps = 0;
    bar->tapButton->pressed.connect([&] {
        ++taps;
        bpm = tap.tap(clock);
        clock += 0.5;
    });
    ui::KeyEvent k;
    k.key = 'T';
    k.text = "t";
    for (int i = 0; i < 3; ++i) ui::App::get().triggerShortcut(&win, k);
    CHECK(taps == 3);
    CHECK_NEAR(bpm, 120, 1e-6);

    // timing shown without signals
    int bpmChanges = 0, beatsToggles = 0;
    bar->bpmBox->valueChanged.connect([&](double) { ++bpmChanges; });
    bar->beatsAction->toggled.connect([&](bool) { ++beatsToggles; });
    Timing t;
    t.bpm = 96.5;
    t.beatsPerBar = 3;
    t.beats = false;
    bar->showTiming(t);
    CHECK(bpmChanges == 0 && beatsToggles == 0);
    CHECK_NEAR(bar->bpmBox->value(), 96.5, 1e-9);
    CHECK(bar->signatureBox->intValue() == 3);
    CHECK(!bar->beatsAction->isChecked() && !bar->thirdsAction->isEnabled());
    bar->setTime(75.5, t);
    CHECK(bar->timeLabel->text() == "1:15.500");
    t.beats = true;
    bar->setTime(0, t);
    CHECK(bar->timeLabel->text() == "1.1.1   0:00.000");
    // B toggles bars and beats, Ctrl+M the metronome
    ui::KeyEvent b;
    b.key = 'B';
    ui::App::get().triggerShortcut(&win, b);
    CHECK(beatsToggles == 1 && bar->beatsAction->isChecked());
    ui::KeyEvent m;
    m.key = 'M';
    m.mods = plat::ModCtrl;
    bool metronome = false;
    bar->metronomeButton->toggled.connect([&](bool on) { metronome = on; });
    ui::App::get().triggerShortcut(&win, m);
    CHECK(metronome && bar->metronomeButton->isChecked());
    bar->setRecording(true);
    CHECK(bar->recordButton->isChecked());
    t.bpm = 120;
    t.beatsPerBar = 4;
    t.looping = true;
    bar->showTiming(t);
    bar->setTime(12.25, t);
    save(bar, "transport");
    bar->setRecording(false);
    CHECK(!bar->recordButton->isChecked() && !bar->recordButton->isCheckable());
    bar->setGeometry(0, 0, 900, hint.y);  // narrower than everything: nothing overlaps
    ui::App::get().doLayouts();
    save(bar, "transport-narrow");
    win.hideWindow();
}

static TrackPtr addTrack(Project& p, Kind kind, const std::string& name);

static void testTransportFitting() {
    ui::Window win(plat::WindowOptions{});
    win.resizeWindow(1800, 40);
    auto* bar = win.add<TransportBar>();
    Point hint = bar->sizeHint();
    win.showWindow();
    Timing timing;
    timing.grid = 0.25;
    bar->showTiming(timing);
    int lastLevel = -1;
    for (int w : {1800, 1400, 1200, 1000, 900, 760, 600, 480, 360, 240, 120}) {
        bar->setGeometry(0, 0, w, hint.y);
        ui::App::get().doLayouts();
        CHECK(bar->level >= lastLevel);  // narrower: as many steps taken, or more
        lastLevel = bar->level;
        if (bar->level < bar->stepCount()) CHECK(bar->needed() <= w - 4);
        // nothing overlaps: the shown controls of each part, and the parts
        std::vector<std::pair<int, int>> spans;
        for (ui::Widget* part : bar->children().size() >= 3 ? std::vector<ui::Widget*>{bar->children()[0].get(),
                                                                                        bar->children()[1].get(),
                                                                                        bar->children()[2].get()}
                                                            : std::vector<ui::Widget*>{})
            for (auto& c : part->children())
                if (c->isVisible()) {
                    int x = c->mapTo(bar, Point(0, 0)).x;
                    spans.push_back({x, x + c->width()});
                }
        std::sort(spans.begin(), spans.end());
        bool overlap = false;
        for (size_t i = 1; i < spans.size(); ++i)
            if (spans[i].first < spans[i - 1].second) overlap = true;
        CHECK(!overlap);
        CHECK(bar->moreButton->isVisible() == (bar->level > TransportBar::LOSSLESS));
        savePng(outDir2() + "/transport-" + std::to_string(w) + ".png", paintWidget(bar));
    }
    // wide: everything as it is; the narrowest with every control: short labels, one Add button
    bar->setGeometry(0, 0, 1800, hint.y);
    ui::App::get().doLayouts();
    CHECK(bar->level == 0 && bar->addFileAction->button->isVisible() && !bar->addAudioTrackAction->button->isVisible());
    CHECK(bar->beatsButton->text() == "♩ Beats" && bar->gridButton->text() == "Grid 1/16 ▾");
    int narrowest = bar->narrowestWidth();
    CHECK(bar->minimumSizeHint().x == narrowest + 4);
    bar->setGeometry(0, 0, narrowest + 4, hint.y);
    ui::App::get().doLayouts();
    CHECK(bar->level == TransportBar::LOSSLESS);
    CHECK(bar->addFileAction->button->isVisible() && !bar->addMidiTrackAction->button->isVisible() && !bar->moreButton->isVisible());
    CHECK(bar->beatsButton->text() == "♩" && bar->thirdsButton->text() == "⅓" && bar->metronomeButton->text() == "Metro");
    CHECK(bar->gridButton->text() == "1/16 ▾");
    savePng(outDir2() + "/transport-narrowest.png", paintWidget(bar));
    // narrower: into the » menu, which has what is hidden
    bar->setGeometry(0, 0, narrowest - 120, hint.y);
    ui::App::get().doLayouts();
    CHECK(bar->level > TransportBar::LOSSLESS && bar->moreButton->isVisible());
    CHECK(!bar->middleButtons[6]->isVisible());  // (Capture goes first)
    ui::Menu more;
    bar->fillMoreMenu(more);
    CHECK(actionStarting(more, "Capture") != nullptr);
    // the shortcuts of hidden controls still work
    bar->setGeometry(0, 0, 120, hint.y);  // (Play and Rec stay longest)
    ui::App::get().doLayouts();
    CHECK(bar->middleButtons[0]->isVisible() && bar->middleButtons[1]->isVisible() && !bar->middleButtons[2]->isVisible());
    bar->setGeometry(0, 0, 40, hint.y);
    ui::App::get().doLayouts();
    CHECK(bar->level == bar->stepCount());
    CHECK(!bar->middleButtons[5]->isVisible() && !bar->middleButtons[0]->isVisible());
    ui::KeyEvent k;
    k.key = 'K';
    k.mods = plat::ModCtrl;
    CHECK(ui::App::get().triggerShortcut(&win, k) && bar->keysAction->isChecked());
    ui::Menu all;
    bar->fillMoreMenu(all);
    auto texts = menuTexts(all);
    CHECK(hasText(texts, "Add file…") && !hasText(texts, "Return track") && hasText(texts, "Play") &&
          hasText(texts, "Recording settings…") && hasText(texts, "Tap tempo") && hasText(texts, "Metronome sounds…") &&
          hasText(texts, "♩ Beats") && hasText(texts, "Grid: 1/16"));
    CHECK(actionStarting(all, "Tempo: 120.00 BPM") && actionStarting(all, "Time signature: 4 / 4"));
    bool settings = false;
    bar->recordButton->rightClicked.connect([&](ui::MouseEvent&) { settings = true; });
    ui::Menu::activate(actionStarting(all, "Recording settings…"));
    CHECK(settings);
    // the grid: its name, snapping off, triplets
    timing.snapOn = false;
    bar->showTiming(timing);
    CHECK(bar->gridName.empty());
    bar->setGeometry(0, 0, 1800, hint.y);
    ui::App::get().doLayouts();
    CHECK(bar->gridButton->text() == "No snap ▾");
    timing.snapOn = true;
    timing.grid = 0;
    bar->showTiming(timing);
    CHECK(bar->gridButton->text() == "Grid auto ▾");
    timing.thirds = true;
    timing.grid = Frac(1, 6).value();
    bar->showTiming(timing);
    CHECK(bar->gridButton->text().find("T ▾") != std::string::npos);
    timing.beats = false;
    bar->showTiming(timing);
    CHECK(!bar->gridButton->isEnabled());
    // the grid's keys and menu
    int finer = 0;
    bar->gridActions["finer"]->triggered.connect([&] { ++finer; });
    ui::KeyEvent one;
    one.key = '1';
    one.mods = plat::ModCtrl;
    ui::App::get().triggerShortcut(&win, one);
    CHECK(finer == 1);
    Timing grid;
    grid.grid = 0.25;
    ui::Menu gm;
    std::vector<GridChange> chosen;
    transport_bar::fillGridMenu(gm, grid, [&](const GridChange& c) { chosen.push_back(c); });
    texts = menuTexts(gm);
    CHECK(texts.front() == "Follows the zoom" && hasText(texts, "Snap to the grid") && hasText(texts, "Finer") &&
          hasText(texts, "Coarser") && hasText(texts, grid.gridName()));
    for (auto& a : gm.actions())
        if (a->text == grid.gridName()) CHECK(a->checked);
    ui::Menu::activate(actionStarting(gm, "Snap to the grid"));
    ui::Menu::activate(actionStarting(gm, "Follows the zoom"));
    CHECK(chosen.size() == 2 && chosen[0].what == GridChange::Snap && !chosen[0].snap && chosen[1].what == GridChange::Grid &&
          chosen[1].grid == 0);
    win.hideWindow();
}

static void testPanel2() {
    // the successor's rows: pan, sends, returns, takes, a track's delay, automation shown on the fader
    CHECK(fmt::pan(-0.3) == "30 L" && fmt::pan(0) == "C" && fmt::pan(1) == "100 R");
    Project project;
    audio::Engine engine(project);
    TrackPtr drums = addTrack(project, Kind::Audio, "Drums");
    TrackPtr keys = addTrack(project, Kind::Midi, "Keys");
    keys->needsInstrument = false;
    TrackPtr reverb = project.addTrack(Kind::Return);
    reverb->name = "Reverb";
    drums->sends = std::vector<Send>{Send{reverb, -6}};
    drums->delayMs = 12;
    drums->pan = -0.25;
    auto take = makeAudioClip("Take 1", std::make_shared<AudioBuffer>(), engine.sr, engine.sr);
    auto samples = std::make_shared<AudioBuffer>();
    samples->samples.assign((size_t)engine.sr * 2 * 2, 0.1f);
    take = makeAudioClip("Take 1", samples, engine.sr, engine.sr);
    drums->takes = std::vector<ClipPtr>{take};
    drums->showTakes = true;
    auto vol = std::make_shared<Automation>();
    vol->slot = drums->controls;
    vol->param = "Volume";
    vol->setPoints({{0.0, 1.0}, {10.0, 0.0}});
    drums->automation = std::vector<AutomationPtr>{vol};

    ui::Window win(plat::WindowOptions{});
    win.resizeWindow(1000, 700);
    auto* scroll = win.add<ui::ScrollArea>();
    scroll->setGeometry(0, 0, 700, 700);
    auto* timeline = scroll->makeContent<Timeline>(project, engine, scroll);
    MasterStrip master(engine, [] {});
    auto* panel = win.add<LanePanel>(project, *timeline, master, engine.channels, [](const TrackPtr&) {});
    std::vector<TrackPtr> fxOpened;
    panel->openMidiEffects = [&](const TrackPtr& t) { fxOpened.push_back(t); };
    panel->rebuild();
    win.showWindow();
    ui::App::get().doLayouts();
    ui::App::get().collectGarbage();
    CHECK(panel->frames.size() == 3 && panel->takeFrames.size() == 1 && panel->frames.count(reverb));
    // a return's row (lane_panel_return.cpp): no solo, no sends button, mute as a ✕ toggle; the others: pan left of
    // M, → when there are returns
    ui::Button* returnMute = nullptr;
    for (auto* b : findAll<ui::Button>(panel->frames[reverb]))
        if (b->text() == "✕" && b->isCheckable()) returnMute = b;
    CHECK(!buttonText(panel->frames[reverb], "S") && !buttonText(panel->frames[reverb], "M") && returnMute);
    CHECK(buttonText(panel->frames[drums], "S") && findAll<PanKnob>(panel->frames[drums]).size() == 1);
    CHECK(findAll<SendsButton>(panel->frames[reverb]).empty());
    auto sends = findAll<SendsButton>(panel->frames[drums]);
    CHECK(sends.size() == 1);
    CHECK(sends[0]->property("lit") == "true");
    CHECK(sends[0]->toolTip() == "Sends to the return tracks: Reverb -6 dB (click to set them)");
    // (the return's Auto: an icon, beside its FX)
    ui::Button* returnAuto = nullptr;
    for (auto* b : findAll<ui::Button>(panel->frames[reverb]))
        if (b->text().empty() && b->isCheckable()) returnAuto = b;
    CHECK(buttonText(panel->frames[drums], "Auto") && buttonText(panel->frames[drums], "Auto")->width() == 36 && returnAuto &&
          !buttonText(panel->frames[reverb], "Auto"));
    // the delay after the name; the take's row
    bool named = false;
    for (auto* l : findAll<ui::Label>(panel->frames[drums]))
        if (l->text() == "Drums  (+12 ms)") named = true;
    CHECK(named);
    bool takeNamed = false;
    for (auto* l : findAll<ui::Label>(panel->takeFrames[take]))
        if (l->text() == "Take 1  (2.0 s)") takeNamed = true;
    CHECK(takeNamed && buttonText(panel->takeFrames[take], "Use"));
    // the sends' window content: a slider per return
    SendsPanel sp(drums, {reverb}, [&](const TrackPtr& t, double db) { sends[0]->setLevel(t, db); },
                  [&](const TrackPtr& t, bool on) { sends[0]->setPreFader(t, on); });
    sp.resize(sp.sizeHint().x, sp.sizeHint().y);
    CHECK(sp.sliders.size() == 1 && sp.sliders[0]->value() == -6);
    savePng(outDir2() + "/sends.png", paintWidget(&sp));
    sp.sliders[0]->setValue(-3);
    CHECK(drums->sends.size() == 1 && drums->sends[0].db == -3);
    // Pre: the send before the fader (model.h Send); kept as it is when the level changes
    CHECK(sp.preButtons.size() == 1 && !sp.preButtons[0]->isChecked() && !sends[0]->preFader(reverb));
    CHECK(!sp.preButtons[0]->toolTip().empty() && sp.preButtons[0]->toolTip().rfind("Pre-fader:", 0) == 0);
    sp.preButtons[0]->setChecked(true);
    CHECK(drums->sends.size() == 1 && drums->sends[0].preFader && drums->sends[0].db == -3 && sends[0]->preFader(reverb));
    CHECK(sends[0]->toolTip() == "Sends to the return tracks: Reverb -3 dB (pre-fader) (click to set them)");
    sp.sliders[0]->setValue(-9);
    CHECK(drums->sends.size() == 1 && drums->sends[0].preFader && drums->sends[0].db == -9);
    {  // the window opened again shows it
        SendsPanel again(drums, {reverb}, nullptr);
        CHECK(again.preButtons[0]->isChecked());
    }
    savePng(outDir2() + "/sends_pre.png", paintWidget(&sp));
    // turned right down it stays (silent) so it is still pre-fader when turned up; Pre off then removes it
    sends[0]->setLevel(reverb, config::MIN_DB);
    CHECK(drums->sends.size() == 1 && drums->sends[0].preFader && sends[0]->property("lit").empty());
    sends[0]->setPreFader(reverb, false);
    CHECK(drums->sends.empty());
    // Pre with no send yet: remembered for when it is turned up
    sends[0]->setPreFader(reverb, true);
    sends[0]->setLevel(reverb, -12);
    CHECK(drums->sends.size() == 1 && drums->sends[0].preFader && drums->sends[0].db == -12);
    sends[0]->setPreFader(reverb, false);
    CHECK(drums->sends.size() == 1 && !drums->sends[0].preFader);
    sends[0]->setLevel(reverb, config::MIN_DB);
    CHECK(drums->sends.empty() && sends[0]->property("lit").empty());
    // the pan knob: drag up or right turns it right, Shift finer, double-click centres
    PanKnob* knob = findAll<PanKnob>(panel->frames[drums])[0];
    CHECK_NEAR(knob->pan, -0.25, 1e-9);
    ui::MouseEvent e;
    e.button = plat::LeftButton;
    e.pos = Point(10, 10);
    knob->mousePress(e);
    e.pos = Point(20, 0);  // (10 right and 10 up: 20 px)
    knob->mouseMove(e);
    CHECK_NEAR(drums->pan, -0.05, 1e-9);
    e.mods = plat::ModShift;
    e.pos = Point(60, 10);  // (50 right: Shift, a fifth as much)
    knob->mouseMove(e);
    CHECK_NEAR(drums->pan, -0.15, 1e-9);
    CHECK(knob->toolTip() == "Pan 15 L (drag up/down; double-click: centre)");
    knob->mouseRelease(e);
    knob->setPan(0.6);
    savePng(outDir2() + "/pan-knob.png", paintWidget(knob));
    knob->mouseDoubleClick(e);
    CHECK(drums->pan == 0.0);
    // automation shown on the fader while playing, the track's own when stopped
    drums->volumeDb = -12;
    drums->pan = 0.5;
    panel->showMixerValues((int64_t)(5.0 * engine.sr), engine.sr);
    CHECK(panel->mixers[drums].slider->value() == (int)std::nearbyint(config::MIN_DB + 0.5 * (config::MAX_DB - config::MIN_DB)));
    CHECK(drums->volumeDb == -12);  // (shown only)
    panel->showMixerValues(std::nullopt, engine.sr);
    CHECK(panel->mixers[drums].slider->value() == -12 && panel->mixers[drums].reading->text() == "-12 dB");
    CHECK_NEAR(panel->mixers[drums].pan->pan, 0.5, 1e-9);
    // menus: delay, takes, MIDI effects; a return's has no groups
    ui::Menu m1;
    panel->fillTrackMenu(m1, drums);
    auto texts = menuTexts(m1);
    CHECK(hasText(texts, "Track delay… (+12 ms)") && hasText(texts, "Hide takes") && hasText(texts, "Group tracks…"));
    CHECK(!actionStarting(m1, "MIDI effects"));
    ui::Menu m2;
    panel->fillTrackMenu(m2, reverb);
    texts = menuTexts(m2);
    CHECK(hasText(texts, "Track delay…") && !hasText(texts, "Group tracks…"));
    keys->midiEffects = std::vector<MidiEffect>{MidiEffect{"Chord", {}}};
    ui::Menu m3;
    panel->fillTrackMenu(m3, keys);
    ui::Action* fx = actionStarting(m3, "MIDI effects… (1)");
    CHECK(fx != nullptr);
    if (fx) ui::Menu::activate(fx);
    CHECK(fxOpened.size() == 1 && fxOpened[0] == keys);
    // a MIDI track's bend range (the bend wheel of a MIDI keyboard): shown when not ±2
    CHECK(hasText(menuTexts(m3), "Pitch bend range…") && !hasText(texts, "Pitch bend range…"));
    keys->bendRange = 12;
    ui::Menu m4;
    panel->fillTrackMenu(m4, keys);
    CHECK(hasText(menuTexts(m4), "Pitch bend range… (±12)"));
    keys->bendRange = 2;
    // Track delay… asks (the dialog answered by the test)
    track_dialogs::answer = [](const std::string& title, std::string& value) {
        CHECK(title == "Track delay" && value == "12.0");
        value = "-20.04";
        return true;
    };
    panel->setDelay(drums);
    track_dialogs::answer = nullptr;
    CHECK_NEAR(drums->delayMs, -20.0, 1e-9);
    ui::App::get().collectGarbage();
    // Hide takes
    panel->toggleTakes(drums);
    ui::App::get().collectGarbage();
    CHECK(!drums->showTakes && panel->takeFrames.empty());
    panel->toggleTakes(drums);
    ui::App::get().doLayouts();
    save(panel, "../panel2/panel");
    // meters go on across rebuilds
    std::vector<float> over{1.2f, 0.4f};
    panel->meters[drums]->push(over);
    CHECK(panel->meters[drums]->clipped());
    panel->rebuild();
    CHECK(panel->meters[drums]->clipped());
    ui::App::get().collectGarbage();
    win.hideWindow();
}

static TrackPtr addTrack(Project& p, Kind kind, const std::string& name) {
    TrackPtr t = p.addTrack(kind);
    t->name = name;
    return t;
}

static void testPanel() {
    Project project;
    audio::Engine engine(project);
    engine.channels = 4;
    TrackPtr drums = addTrack(project, Kind::Audio, "Drums");
    TrackPtr bass = addTrack(project, Kind::Audio, "Bass");
    TrackPtr keys = addTrack(project, Kind::Midi, "Keys");
    TrackPtr lead = addTrack(project, Kind::Midi, "Lead synth");
    TrackPtr fresh = addTrack(project, Kind::Midi, "MIDI 3");
    TrackPtr vox = addTrack(project, Kind::Audio, "Vocals");
    lead->needsInstrument = false;
    keys->needsInstrument = false;
    fresh->needsInstrument = true;
    auto eq = std::make_shared<PluginSlot>();
    eq->name = "Tiny EQ";
    eq->kind = "effect";
    vox->effects = std::vector<SlotPtr>{eq};
    auto a = std::make_shared<Automation>();
    a->slot = eq;
    a->param = "Low gain";
    vox->automation = std::vector<AutomationPtr>{a};
    vox->showAutomation = true;
    vox->color = "#a03050";
    vox->output = 2;
    bass->muted = true;
    keys->soloed = true;
    GroupPtr group = project.groupTracks({drums, bass});
    group->name = "Rhythm";
    group->color = "#3070c0";

    ui::Window win(plat::WindowOptions{});
    win.resizeWindow(1000, 900);
    auto* scroll = win.add<ui::ScrollArea>();
    scroll->setGeometry(0, 0, 700, 900);
    auto* timeline = scroll->makeContent<Timeline>(project, engine, scroll);
    MasterStrip master(engine, [] {});
    int fxOpened = 0;
    auto* panel = win.add<LanePanel>(project, *timeline, master, engine.channels,
                                     [&](const TrackPtr&) { ++fxOpened; },
                                     [](ui::Menu& menu, const TrackPtr&) { menu.addAction("Freeze track"); },
                                     LanePanel::Instruments{[](ui::Menu& m, const TrackPtr&) { m.addAction("Built-in synth"); },
                                                            [](const TrackPtr&) {}});
    win.showWindow();
    ui::App::get().doLayouts();
    // rows: group header, its two tracks, the other tracks, the automation lane under Vocals
    CHECK(panel->frames.size() == 6);
    CHECK(panel->groupFrames.size() == 1 && panel->groupMeters.empty());  // (a bus: its controls in its header)
    CHECK(panel->automationFrames.size() == 1);
    CHECK(panel->meters.size() == 6 && panel->meters.count(group->bus) && !panel->meters.count(fresh));  // (waiting for its instrument: no meter)
    int expectH = lanes::layout(project).back().bottom() + theme::SCROLLBAR_W;
    CHECK(panel->height() == expectH);
    CHECK(panel->width() == config::PANEL_W);
    CHECK(panel->frames[drums]->height() == drums->height);
    // outputs: the master meter shows every output in use, a group meter its tracks'
    CHECK(master.meter->shown() == std::vector<int>({0, 1, 2, 3}));
    CHECK(panel->meters[group->bus]->shown() == std::vector<int>({0, 1}));
    CHECK(panel->meters[vox]->shown() == std::vector<int>({2, 3}));
    // the selected track's row is outlined
    timeline->setCurrentTrack(keys);
    CHECK(panel->frames[keys]->isCurrent() && !panel->frames[drums]->isCurrent());
    // a coloured row's labels take its text colour
    CHECK(panel->frames[vox]->textColor().has_value());

    // the row's own controls
    panel->frames[bass]->onPress();
    CHECK(timeline->currentTrack == bass && panel->frames[bass]->isCurrent());
    ui::Button* muteDrums = nullptr;
    for (auto& c : panel->frames[drums]->children())
        if (auto* b = dynamic_cast<ui::Button*>(c.get()); b && b->text() == "M") muteDrums = b;
    CHECK(muteDrums && !drums->muted);
    if (muteDrums) muteDrums->click();
    CHECK(drums->muted);
    if (muteDrums) muteDrums->click();
    CHECK(!drums->muted);
    timeline->setCurrentTrack(keys);
    // menus
    ui::Menu menu;
    panel->fillTrackMenu(menu, vox);
    auto texts = menuTexts(menu);
    CHECK(texts.size() >= 7 && texts[0] == "Rename track…" && texts[1] == "Track colour…");
    CHECK(hasText(texts, "Automate a parameter…") && hasText(texts, "Hide automation"));
    CHECK(hasText(texts, "Group tracks…") && hasText(texts, "Add to group") && hasText(texts, "Freeze track"));
    CHECK(!hasText(texts, "Remove from Rhythm"));
    ui::Menu menu2;
    panel->fillTrackMenu(menu2, drums);
    texts = menuTexts(menu2);
    CHECK(hasText(texts, "Remove from Rhythm") && !hasText(texts, "Add to group") && !hasText(texts, "Show automation"));
    ui::Menu menu3;
    panel->fillGroupMenu(menu3, group);
    CHECK(menuTexts(menu3) == std::vector<std::string>({"Rename group…", "Group colour…", "Fold its tracks away", "Add track",
                                                        "Fold only (no bus)", "Effects…", "Automate a parameter…",
                                                        "Bus delay…", "Output: Master", "Put in group", "---",
                                                        "Ungroup (the tracks stay)"}));

    // meters at various levels, LUFS readouts
    std::vector<float> quiet{0.03f, 0.02f}, loud{0.5f, 0.8f}, hot{1.0f, 0.97f}, out34{0, 0, 0.25f, 0.6f};
    panel->meters[drums]->push(hot);
    panel->meters[bass]->push(quiet);
    panel->meters[keys]->push(loud);
    panel->meters[vox]->push(out34);
    panel->meters[group->bus]->push(level_meter::loudest({&hot, &quiet}));
    panel->loudness[drums]->showValues(-8, -9.4, -11.2);
    panel->loudness[keys]->showValues(-20, -18.3, -21.0);
    std::vector<float> all{0.7f, 0.9f, 0.25f, 0.6f};
    master.meter->push(all);
    master.loudness->showValues(-10, -11.5, -12.8);
    master.showUpdate({"waiting", "2", update_check::PAGE + "/commits/main", 2});
    save(panel, "panel");

    // the master strip on its own
    ui::Widget holder;
    holder.adopt(master.releaseFrame());
    master.frame->setGeometry(0, 0, config::PANEL_W, config::MASTER_H);
    save(master.frame, "master");
    master.refreshFx({eq});
    CHECK(master.fx->property("lit") == "true");
    CHECK(master.fx->toolTip() == "Effects: Tiny EQ\nMaster effects work on outputs 1/2.");
    master.apply({-6, true});
    CHECK_NEAR(engine.masterGain.load(), std::pow(10.0, -6 / 20.0), 1e-9);
    CHECK(engine.masterMute.load());
    auto st = master.settings();
    CHECK(st.volumeDb == -6 && st.mute);
    master.apply({});
    CHECK(engine.masterGain.load() == 1.0 && !engine.masterMute.load());
    master.showUpdate({"failed", "HTTP Error 403", update_check::PAGE});
    // (an answer was shown above: it stays; the tooltip says this check failed)
    CHECK(master.notice->notice().plain() == "2 updates ready · rebuild to get them" && !master.versionLabel);
    CHECK(master.notice->toolTip().find("The last check failed (HTTP Error 403)") != std::string::npos);
    // an answer, then a check that fails (offline a while): the answer stays shown, its tooltip says the check failed
    update_check::Result found{"waiting", "1", update_check::PAGE + "/commits/main", 1, std::string(NEWEST, 40),
                               (double)std::time(nullptr) - 3 * 3600, update_check::PAGE + "/commit/x"};
    master.showUpdate(found);
    CHECK(master.notice->notice().plain() == "Update available - 3f9a2c1 - 3 hours ago");
    master.showUpdate({"failed", "could not resolve api.github.com (no network?)", update_check::PAGE});
    CHECK(master.notice->notice().plain() == "Update available - 3f9a2c1 - 3 hours ago");
    CHECK(master.notice->toolTip().find("The last check failed (could not resolve") != std::string::npos);
    master.showUpdate({"failed", "HTTP Error 403", update_check::PAGE});
    save(master.frame, "master-fx");

    // hiding the meters and LUFS (Options)
    panel->setMetersVisible(false, false);
    CHECK(!panel->meters[drums]->isVisible() && !master.meter->isVisible() && !master.loudness->isVisible());
    panel->setMetersVisible(true, true);

    // a device with fewer outputs: tracks on outputs it lacks go back to 1/2
    panel->setChannels(2);
    ui::App::get().collectGarbage();
    CHECK(vox->output == 0);
    CHECK(master.meter->shown() == std::vector<int>({0, 1}));
    // resizing a track by its row's bottom edge
    keys->height = 120;
    timeline->heightsChanged.emit();
    ui::App::get().doLayouts();
    CHECK(panel->frames[keys]->height() == 120);
    // folding the group: its tracks' rows go
    group->folded = true;
    panel->rebuild();
    ui::App::get().collectGarbage();
    CHECK(panel->frames.size() == 4 && panel->groupFrames.size() == 1);
    save(panel, "panel-folded");
    win.hideWindow();
}

static void testPinToBottom() {
    ui::Window win(plat::WindowOptions{});
    win.resizeWindow(300, 400);
    auto* scroll = win.add<ui::ScrollArea>();
    scroll->setGeometry(0, 0, 300, 400);
    auto* content = scroll->makeContent<ui::Widget>();
    content->setFixedHeight(1000);
    auto pinned = std::make_unique<ui::Widget>();
    ui::Widget* raw = pinned.get();
    PinToBottom pin(std::move(pinned), scroll->viewport(), 100, theme::SCROLLBAR_W);
    win.showWindow();
    ui::App::get().doLayouts();
    pin.place();
    int vh = scroll->viewport()->height(), vw = scroll->viewport()->width();
    CHECK(raw->geometry() == Rect(0, vh - theme::SCROLLBAR_W - 100, vw, 100));
    CHECK(pin.cover()->geometry() == Rect(0, vh - theme::SCROLLBAR_W, vw, theme::SCROLLBAR_W));
    CHECK(scroll->viewport()->children().back().get() == pin.cover());
    // the viewport is resized: it follows
    scroll->setGeometry(0, 0, 250, 300);
    ui::App::get().doLayouts();
    CHECK(raw->geometry().bottom() == scroll->viewport()->height() - theme::SCROLLBAR_W);
    CHECK(raw->width() == scroll->viewport()->width());
    // contents added later don't cover it (checked every frame)
    scroll->viewport()->add<ui::Widget>();
    ui::App::get().tick();
    CHECK(scroll->viewport()->children().back().get() == pin.cover());
    win.hideWindow();
}

// Boxes drawn where only flat colours were: a track's row (its own colour inside TrackRow's box), the timeline's
// frame (ScrollArea#timeline), the Explorer's title bar, a meter's track and the meter itself, the EQ curve's grid
// labels. Only a stylesheet asking for them changes anything: the default look comes back pixel for pixel.
static void testSurfaceBoxes() {
    auto sheetOf = [](const std::string& text) {
        std::vector<css::Problem> problems;
        css::SheetPtr sheet = css::parse(text, "surfaces.css", problems);
        CHECK(problems.empty());
        for (auto& pr : problems) std::printf("  %s\n", pr.text().c_str());
        return sheet;
    };
    auto sameImage = [](const Image& a, const Image& b) {
        if (a.width() != b.width() || a.height() != b.height()) return false;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x)
                if (a.pixel(x, y) != b.pixel(x, y)) return false;
        return true;
    };
    auto brightness = [](Color c) { return c.r + c.g + c.b; };
    auto near = [](Color c, Color want) {
        return std::abs(c.r - want.r) <= 40 && std::abs(c.g - want.g) <= 40 && std::abs(c.b - want.b) <= 40;
    };
    const Color backdrop(1, 2, 3);  // (under what is painted: what shows where nothing is)

    Project project;
    audio::Engine engine(project);
    engine.channels = 2;
    TrackPtr plain = addTrack(project, Kind::Audio, "Plain");
    TrackPtr coloured = addTrack(project, Kind::Audio, "Coloured");
    coloured->color = "#3070c0";
    ui::Window win(plat::WindowOptions{});
    win.resizeWindow(1000, 700);
    auto* view = win.add<TimelineView>();
    view->setGeometry(0, 0, 700, 600);
    auto* timeline = view->makeContent<Timeline>(project, engine, view);
    MasterStrip master(engine, [] {});
    auto* panel = win.add<LanePanel>(project, *timeline, master, engine.channels, [](const TrackPtr&) {});
    auto* dock = win.add<ExplorerDock>(new ui::Widget(), [] {});
    dock->setGeometry(0, 0, 240, 300);
    LevelMeter meter(2);
    meter.resize(200, 30);
    meter.push(std::vector<float>{1.0f, 1.0f});
    CurveView curve(std::shared_ptr<effects::Effect>(effects::create("eq")));
    curve.resize(300, 130);
    win.showWindow();
    ui::App::get().doLayouts();
    timeline->setCurrentTrack(plain);  // (the coloured row: not outlined)
    auto paintAlone = [&](ui::Widget& w) {  // (the widget's own paint(), not its children)
        Image img(w.width(), w.height(), backdrop);
        Painter p(img);
        w.paint(p);
        return img;
    };
    auto restyle = [&](const std::vector<css::SheetPtr>& sheets) {
        if (sheets.empty()) theme::setSkin();
        else theme::setStyles(sheets);
        panel->rebuild();  // (as the main window does after a skin change: MainWindow::restyle)
        ui::App::get().collectGarbage();
        ui::App::get().repaintAll();
        ui::App::get().doLayouts();
    };

    // --- the default look ---
    Color own, text;
    colors::trackColors(coloured->color, coloured->tone, own, text);
    LaneFrame* row = panel->frames[coloured];
    Image rowBefore = paintWidget(row, backdrop);
    CHECK(rowBefore.colorAt(row->width() / 2, 0) == own);
    CHECK(rowBefore.colorAt(row->width() / 2, row->height() - 1) == own);
    CHECK(row->contentsRect().y == 0);
    int panelH = panel->height();
    CHECK(view->insetTop() == 0 && view->insetBottom() == 0);
    CHECK(view->viewport()->geometry().x == 0 && view->viewport()->geometry().y == 0);
    Image viewBefore = paintWidget(view, backdrop);
    Image barBefore = paintWidget(dock->titleBar(), backdrop);
    CHECK(barBefore.colorAt(3, 1) == backdrop);  // (the bar draws nothing of its own)
    Color titleColor = dock->title()->color();
    bool titleBold = dock->title()->font().bold();
    Image meterBefore = paintAlone(meter);
    Image curveBefore = paintAlone(curve);
    auto countNear = [&](const Image& img, Color want) {
        int n = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                if (near(img.colorAt(x, y), want)) ++n;
        return n;
    };
    const Color magenta(255, 0, 255), cyan(0, 255, 255);
    CHECK(countNear(curveBefore, magenta) == 0);

    restyle({theme::builtinSheet(),
             sheetOf("TrackRow { border: 2px outset #c0c0c0; }\n"
                     "ScrollArea#timeline { border: 2px inset #808080; }\n"
                     "ExplorerDock::title-bar { background-image: linear-gradient(to right, #000080, #1084d0);"
                     " color: #ffffff; font-weight: bold; }\n"
                     "LevelMeter { border: 2px solid #00ff00; padding: 1px; }\n"
                     "LevelMeter::track { border: 1px solid #ff0000; background-color: #000000; }\n"
                     "CurveView::grid-label { color: #ff00ff; }\n")});

    // a raised track row: lighter along its top, darker along its bottom and right, the track's colour inside
    row = panel->frames[coloured];
    CHECK(row && row->height() == coloured->height);
    if (row) {
        Image raised = paintWidget(row, backdrop);
        int w = row->width(), h = row->height(), mid = w / 2;
        Color top = raised.colorAt(mid, 0), bottom = raised.colorAt(mid, h - 1), right = raised.colorAt(w - 1, h / 2);
        CHECK(top != own && bottom != own && brightness(top) > brightness(bottom));
        CHECK(right == bottom);  // (outset: the right and bottom edges in the same shade)
        CHECK(raised.colorAt(mid, 3) == own);  // the track's own colour inside the bevel (above its controls)
        CHECK(raised.colorAt(0, h / 2) == rowBefore.colorAt(0, h / 2));  // the stripe stays as it was
        Rect in = row->contentsRect();  // its controls inside the border
        CHECK(in.y >= 2 && in.right() <= w - 2 && in.bottom() <= h - 2);
    }

    // the timeline's frame: a sunken well around the ruler and the lanes, the panel's rows still lined up
    CHECK(view->insetTop() == 2 && view->insetBottom() == 2);
    Rect vp = view->viewport()->geometry();
    CHECK(vp.x == 2 && vp.y == 2);
    CHECK(panel->height() == panelH + 2);  // (the frame's bottom: the same room to scroll as the timeline)
    Image framed = paintWidget(view, backdrop);
    Color left = framed.colorAt(0, 300), topEdge = framed.colorAt(300, 0);
    Color rightEdge = framed.colorAt(699, 300), bottomEdge = framed.colorAt(300, 599);
    CHECK(left == topEdge && rightEdge == bottomEdge && brightness(left) < brightness(rightEdge));  // (inset)
    CHECK(left != viewBefore.colorAt(0, 300));
    int moved = 0;  // the lanes and ruler inside it, as they were, 2 px further in
    for (int y = 0; y < lanes::rulerHeight() + 60; ++y)
        for (int x = 0; x < 400; ++x)
            if (framed.pixel(x + 2, y + 2) != viewBefore.pixel(x, y)) ++moved;
    CHECK(moved == 0);

    // the Explorer's title bar: a caption bar's gradient, its title white and bold
    {
        Image bar = paintWidget(dock->titleBar(), backdrop);
        int w = bar.width();
        Color l = bar.colorAt(2, 1), r = bar.colorAt(w - 3, 1);
        CHECK(near(l, Color(0, 0, 0x80)) && near(r, Color(0x10, 0x84, 0xd0)) && l != r);
        CHECK(dock->title()->color() == Color(255, 255, 255) && dock->title()->font().bold());
    }

    // the meter: its own box around it all, each bar inside its track's border, which stays there with signal
    {
        Image img = paintAlone(meter);
        CHECK(img.colorAt(0, 0) == Color(0, 255, 0));
        auto bars = meter.barRects();
        CHECK(bars.size() == 2 && bars[0].first >= 3);  // (inside the meter's 2 px border and 1 px padding)
        if (!bars.empty()) {
            int y0 = bars[0].first, t = bars[0].second;
            CHECK(t >= 3);
            CHECK(img.colorAt(3, y0 + t / 2) == Color(255, 0, 0));       // the track's left border
            CHECK(img.colorAt(20, y0) == Color(255, 0, 0));              // …its top
            CHECK(img.colorAt(20, y0 + t - 1) == Color(255, 0, 0));      // …its bottom, under a full bar
            CHECK(img.colorAt(20, y0 + t / 2) == theme::c("meter_ok"));  // the bar inside
        }
    }

    // the EQ's grid labels in their own colour; the grid's lines as they were
    {
        Image img = paintAlone(curve);
        CHECK(countNear(img, magenta) > 5);
        int lineY = (curve.height() - 1) / 2;  // (the 0 dB line, under no label at x = 150)
        CHECK(!near(img.colorAt(150, lineY), magenta) && !near(img.colorAt(150, lineY + 1), magenta));
        CHECK(img.colorAt(150, lineY) == curveBefore.colorAt(150, lineY));
    }
    // …which take the grid's colour unless they set one
    restyle({theme::builtinSheet(), sheetOf("CurveView::grid { color: #00ffff; }\n")});
    CHECK(countNear(paintAlone(curve), cyan) > 5);

    // back to the default look: as before, pixel for pixel
    restyle({});
    row = panel->frames[coloured];
    CHECK(row && sameImage(paintWidget(row, backdrop), rowBefore));
    CHECK(view->insetTop() == 0 && view->viewport()->geometry().x == 0 && panel->height() == panelH);
    CHECK(sameImage(paintWidget(view, backdrop), viewBefore));
    CHECK(sameImage(paintWidget(dock->titleBar(), backdrop), barBefore));
    CHECK(dock->title()->color() == titleColor && dock->title()->font().bold() == titleBold);
    CHECK(sameImage(paintAlone(meter), meterBefore));
    CHECK(sameImage(paintAlone(curve), curveBefore));
    win.hideWindow();
}

int main() {
    ui::App::get().initHeadless();
    settings::setPath("build/ui-samples/panel/settings.json");
    theme::setSkin();
    transport_bar::iconFolder = "data/icons";  // (the program finds them next to itself)
    testMeterLogic();
    testMeterHolds();
    testSmallPieces();
    testUpdateCheck();
    testTransportBar();
    testTransportFitting();
    testPanel();
    testPanel2();
    testPinToBottom();
    testSurfaceBoxes();
    TEST_MAIN_END();
}
