// The steps around a Windows plugin install (see wine_install_steps.h).
#include "ui/wine_install_steps.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "core/util.h"
#include "plugins/install_log.h"
#include "plugins/installer_activity.h"
#include "plugins/installer_watchdog.h"
#include "plugins/wine.h"
#include "plugins/wine_prefix_fixes.h"
#include "ui/toolkit/dialogs.h"
#include "ui/toolkit/widget.h"

namespace wine_setup {

namespace {

const char* const INSTALL_TITLE = "Install Windows plugin";
constexpr double LOOK_EVERY_SECONDS = 15;   // how often the watchdog looks at the prefix
constexpr double LIMIT_LOG_EVERY_SECONDS = 30;
constexpr size_t LONGEST_SHOWN_LINE = 160;  // (a log line longer than this is cut in the message)

// seconds on a clock that only goes forward (for the watchdog's looks)
double steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// the watchdog's question; true if the prefix's programs were ended
bool askToEndHung(ui::Window* parent, const std::string& prefix, const std::string& installer,
                  const std::string& logFile) {
    int minutes = (int)(watchdog::HUNG_AFTER_SECONDS / 60);
    std::string text = util::baseName(installer) + " looks stuck: for " + std::to_string(minutes) +
                       " minutes it has shown no window and done next to nothing.\n\nEnd it? This ends every Windows "
                       "program running in " + prefix + " (Wine's wineserver -k, for that prefix only; plugins of it "
                       "open in Exbit end too, and can be loaded again).\n\nIts log: " + logFile;
    if (ui::msg::question(parent, INSTALL_TITLE, text) != ui::msg::Yes)
        return false;
    std::string why;
    if (!wine::endPrefixPrograms(prefix, why)) {
        ui::msg::warning(parent, INSTALL_TITLE, "Its programs couldn't be ended.\n\n" + why);
        return false;
    }
    return true;
}

}  // namespace

std::string shownLastLines(const std::string& logFile, int lines) {
    std::string shown;
    for (const auto& line : util::split(install_log::lastLines(logFile, lines), '\n'))
        shown += (line.size() > LONGEST_SHOWN_LINE ? line.substr(0, LONGEST_SHOWN_LINE) + "…" : line) + "\n";
    return shown;
}

void runWithProgress(ui::Window* parent, const std::string& title, const std::string& label,
                     const std::function<void()>& work) {
    std::atomic<bool> done{false};
    std::thread worker([&] {
        work();
        done = true;
    });
    {
        ui::ProgressDialog dialog(parent, title, label, false);
        dialog.setRange(0, 0);
        while (!done) {
            dialog.setValue(0);  // (repaints and handles what is pending)
            usleep(20000);
        }
        if (dialog.isWindowShown()) dialog.hideWindow();
    }
    worker.join();
}

bool applyFixesTo(ui::Window* parent, const std::string& prefix, bool quietIfFine) {
    const std::string title = "Wine fixes";
    std::string report;
    bool applied = false;
    runWithProgress(parent, title, "Putting Exbit's Wine fixes in " + prefix + "…",
                    [&] { applied = wine::applyFixes(prefix, report); });
    if (!applied)
        ui::msg::warning(parent, title, "Not all of Exbit's Wine fixes could be put in " + prefix +
                                            "; where one couldn't, Wine's own is used (as before).\n\n" + report);
    else if (!quietIfFine)
        ui::msg::information(parent, title, "Exbit's Wine fixes are in " + prefix + ".\n\n" + report);
    return applied;
}

void offerFixes(ui::Window* parent, const std::string& prefix) {
    if (wine::fixesFolder().empty())
        return;
    bool has = true;
    runWithProgress(parent, INSTALL_TITLE, "Looking at " + prefix + "…", [&] { has = wine::prefixHasFixes(prefix); });
    if (has)
        return;
    std::string text = "Exbit has fixes for Wine that some installers need, and " + prefix + " lacks them:\n\n"
                       "  - msi.dll: Wine could leave a setup's own database unreadable (Kontakt 8's setup stood "
                       "still at \"Computing space requirements\");\n"
                       "  - dxgi.dll: JUCE 8 programs wait for the screen's refresh, which Wine can't tell them "
                       "(Quanta 2's installer never showed its window).\n\n"
                       "Put them in now? Wine's own DLLs are kept, and Options → Wine prefixes → Wine fixes… puts "
                       "them back.";
    if (ui::msg::question(parent, INSTALL_TITLE, text) == ui::msg::Yes)
        applyFixesTo(parent, prefix, true);
}

InstallEnd waitForInstaller(ui::Window* parent, int pid, const std::string& prefix, const std::string& installer,
                            const std::string& logFile) {
    std::string label = "Installing " + util::baseName(installer) + " in " + prefix +
                        "…\n\nAnswer the installer in its own windows (for VST3, keep its usual folder, C:\\Program "
                        "Files\\Common Files\\VST3). Exbit looks for the new plugins when it ends.\n\nIts log: " +
                        logFile;
    InstallEnd end;
    std::vector<watchdog::Sample> samples;
    std::future<watchdog::Sample> look;  // (a look takes a moment: a program run in the prefix)
    double lastLook = steadySeconds(), lastLimit = lastLook;
    ui::ProgressDialog dialog(parent, INSTALL_TITLE, label, true);
    dialog.setRange(0, 0);  // (busy: how long it takes isn't known)
    for (;;) {
        if (wine::finished(pid, end.code)) {
            end.ended = true;
            break;
        }
        if (dialog.wasCanceled()) {
            wine::stop(pid);
            break;
        }
        double now = steadySeconds();
        if (look.valid() && look.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            samples.push_back(look.get());
            watchdog::trim(samples);
            if (!end.endedAsHung && watchdog::looksHung(samples)) {
                end.endedAsHung = askToEndHung(parent, prefix, installer, logFile);
                samples.clear();  // (asked again only after another whole span)
            }
        } else if (!look.valid() && now - lastLook >= LOOK_EVERY_SECONDS) {
            lastLook = now;
            look = std::async(std::launch::async, [prefix, now] { return installer_activity::sample(prefix, now); });
        }
        if (now - lastLimit >= LIMIT_LOG_EVERY_SECONDS) {
            lastLimit = now;
            install_log::limitSize(logFile);
        }
        dialog.setValue(0);  // (repaints and handles what is pending)
        usleep(20000);
    }
    if (look.valid())
        look.wait();
    if (dialog.isWindowShown())
        dialog.hideWindow();
    install_log::limitSize(logFile);
    return end;
}

void reportHungInstall(ui::Window* parent, const std::string& installer, const std::string& logFile) {
    std::string text = util::baseName(installer) + " was stuck, and was ended.\n\nWhat Wine and the installer printed "
                       "is in its log:\n" + logFile + "\n\nIts last lines are under Details.";
    ui::msg::box(parent, ui::msg::Icon::Warning, INSTALL_TITLE, text, ui::msg::Ok, ui::msg::Ok, shownLastLines(logFile));
}

std::string installEndNote(const InstallEnd& end, const std::string& logFile) {
    if (end.ended && end.code == 0 && !end.endedAsHung)
        return "";
    std::string how = end.endedAsHung ? "was ended as stuck"
                      : end.ended     ? "ended with exit status " + std::to_string(end.code)
                                      : "was stopped";
    return "The installer " + how + ". What Wine and the installer printed is in its log:\n" + logFile;
}

}  // namespace wine_setup
