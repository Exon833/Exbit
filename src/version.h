// Which program this is, and where its updates are published (version.py, update_check.py).
//
// Tiny DAW is a rolling release: there are no version numbers, the GitHub repository's main branch
// is the program. A build knows the commit it was built from (COMMIT: the Makefile passes
// -DTINYDAW_COMMIT="$(git rev-parse HEAD)"; "" when it was built outside a git checkout), and the
// update notice under the master row compares it with the main branch on GitHub.
#pragma once

#ifndef TINYDAW_COMMIT
#define TINYDAW_COMMIT ""
#endif

namespace version {
constexpr const char* COMMIT = TINYDAW_COMMIT;  // the commit this program was built from, or ""
constexpr const char* REPOSITORY = "Exon833/Tiny-DAW";  // on GitHub: its main branch is the program
constexpr const char* VERSION = "0.4";  // (the last numbered version, Tiny DAW 0.4; no longer shown)
}  // namespace version
