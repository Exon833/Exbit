// The steps around a Windows plugin install that Install Windows plugin… and New prefix… take
// (wine_setup.h): Exbit's Wine fixes offered and put in (plugins/wine_prefix_fixes.h), the wait for the
// installer with its log and the hung-installer watchdog (plugins/install_log.h, installer_watchdog.h),
// and what is said when an install fails. All through the toolkit's dialogs: the same in the window
// system and in the terminal (exbit --tui draws them as text).
#pragma once
#include <functional>
#include <string>

namespace ui {
class Window;
}

namespace wine_setup {

// runs `work` on a thread of its own while a busy progress window (no Cancel) keeps the program
// responsive; returns when it is done
void runWithProgress(ui::Window* parent, const std::string& title, const std::string& label,
                     const std::function<void()>& work);

// puts Exbit's Wine fixes in a prefix (a progress window meanwhile); says so if they couldn't all be
// put in. `quietIfFine`: no message when they were. True if they are in
bool applyFixesTo(ui::Window* parent, const std::string& prefix, bool quietIfFine);

// before an install: if Exbit has fixes the prefix lacks, asks whether to put them in (and does)
void offerFixes(ui::Window* parent, const std::string& prefix);

// how an install ended
struct InstallEnd {
    bool ended = false;      // it ended (else Cancel was pressed, and it was stopped)
    int code = 0;            // its exit status (when it ended)
    bool endedAsHung = false;  // the watchdog found it stuck and the person had the prefix's programs ended
};

// waits for an installer (`pid`, run in `prefix`, its output going to `logFile`) with a progress window;
// every 15 s the watchdog looks at the prefix, and when the installer looks hung (no window, next to no
// work for minutes) asks whether to end it (wineserver -k, that prefix only). The log is kept under its
// size limit meanwhile
InstallEnd waitForInstaller(ui::Window* parent, int pid, const std::string& prefix, const std::string& installer,
                            const std::string& logFile);

// an installer ended as hung: says so, with the log's path and its last lines (Details)
void reportHungInstall(ui::Window* parent, const std::string& installer, const std::string& logFile);

// how an install ended, for the message after it when no new plugin was found: "" when it ended well
// (exit status 0), else its exit status and the log's path (some installers end with another status
// after a good install: Kontakt 8's ends with 100, so it isn't called a failure by itself)
std::string installEndNote(const InstallEnd& end, const std::string& logFile);

// the log's last lines, each cut to a length a message can show (for a message's Details)
std::string shownLastLines(const std::string& logFile, int lines = 12);

}  // namespace wine_setup
