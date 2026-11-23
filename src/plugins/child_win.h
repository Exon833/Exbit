// Windows: child processes and pipes for the plugin code (the plugin hosts, plugins/wine_win.cpp and
// bridged.cpp; the kind finder's probe, probe.cpp): what posix_spawn, waitpid, kill and pipes are
// on Linux. Processes are known by their ids, as there; their handles are kept here until they
// have ended and been asked about (finished) or stopped (kill).
#pragma once
#include <string>
#include <vector>

namespace child {

// starts `exe` (UTF-8) with these arguments (quoted as Windows' programs read them), its stdin and
// stdout these CRT fds (-1: none), its stderr this process's; only those are inherited (not every
// handle this process has). Its process id, or -1 and why
int spawn(const std::string& exe, const std::vector<std::string>& args, int inFd, int outFd, std::string& why);
bool finished(int pid, int& code);  // has it ended? (`code`: its exit code)
void kill(int pid);                 // ends it, and waits for it (a moment)
// a pipe of CRT fds, not inherited (spawn gives the child its own copy of the end it gets)
bool pipe(int fds[2]);
// (a pipe's reading end) 1: something to read, 0: nothing within ms, -1: its writers are gone
int readable(int fd, int ms);
// the command line Windows gives a program for these arguments (tests)
std::wstring commandLine(const std::string& exe, const std::vector<std::string>& args);

}  // namespace child
