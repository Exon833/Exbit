// Zip archives (PKWARE's APPNOTE): Exbit's own writer and reader, for project archives (model/collect.h).
//
// Writing: each file stored, or deflated (gfx/image.h's zlibDeflate) when that makes it smaller; names in
// UTF-8. The archive is written beside its place as "<path>.part" and renamed over it only once complete, so
// a crash or a full disk never leaves a half-written archive under its name. No zip64: an archive of more
// than 4 GB or 65535 files is refused (shortcut: add zip64 when projects that big need archiving).
//
// Reading: stored and deflated entries, each checked against its CRC-32 and size. Every name is checked
// first (safeName: no absolute paths, no "..", no backslashes or control characters: zip-slip), and an
// archive with one bad name or one damaged entry is refused whole. extract() unpacks into a new folder,
// through "<folder>.part" renamed only once every file is out and checked: a damaged archive leaves nothing
// that looks complete (its partial folder is removed again).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace zip {

struct Entry {
    std::string name;  // in the archive: '/'-separated, relative
    std::string file;  // on disk
};
// `progress(done, total)`: as the files go in
bool write(const std::string& path, const std::vector<Entry>& entries, std::string& error,
           const std::function<void(int done, int total)>& progress = nullptr);

// a name safe to unpack under a folder: relative, no "." or ".." parts, no empty parts (a trailing '/' marks
// a folder), no backslashes, no control characters
bool safeName(const std::string& name);

// the names of the files in the archive (folders left out); false and `error` if it isn't a zip archive, is
// damaged, or holds a name that isn't safe
bool list(const std::string& path, std::vector<std::string>& names, std::string& error);

// everything unpacked into `folder` (which must not exist yet: made); `files`: the files written (absolute).
// False and `error` if anything is wrong, with nothing left behind.
bool extract(const std::string& path, const std::string& folder, std::vector<std::string>& files, std::string& error,
             const std::function<void(int done, int total)>& progress = nullptr);

}  // namespace zip
