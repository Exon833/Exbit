// When a file was made (core/file_birth_time.h)
#include "core/file_birth_time.h"

#include <fcntl.h>
#include <sys/stat.h>

namespace file_birth_time {

int64_t fileBirthTime(const std::string& path) {
#if defined(__linux__) && defined(STATX_BTIME)
    // (statx: glibc 2.28 and Linux 4.11 on; a file system without birth times leaves STATX_BTIME out of the mask)
    struct statx extended {};
    if (statx(AT_FDCWD, path.c_str(), 0, STATX_BTIME | STATX_MTIME, &extended) == 0) {
        if ((extended.stx_mask & STATX_BTIME) && extended.stx_btime.tv_sec > 0) return extended.stx_btime.tv_sec;
        if (extended.stx_mask & STATX_MTIME) return extended.stx_mtime.tv_sec;
    }
#endif
    struct stat status;
    if (stat(path.c_str(), &status) != 0) return 0;
#ifdef EXBIT_WINDOWS
    if (status.st_ctime > 0) return (int64_t)status.st_ctime;  // (Windows' C library: the file's creation time)
#endif
    return (int64_t)status.st_mtime;
}

}  // namespace file_birth_time
