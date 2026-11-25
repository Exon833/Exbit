// Exbit was called Tiny DAW. The names it used then, read still, so folders, files and settings made before the
// rename keep working (core/program_environment.h, core/folder_migration.h, core/format_marker.h).
#pragma once

namespace old_name {
constexpr const char* ENVIRONMENT_PREFIX = "TINYDAW_";      // its environment variables (now EXBIT_…)
constexpr const char* FOLDER = "TinyDAW";                   // its settings and data folders (now Exbit)
constexpr const char* NAME_PREFIX = "tinydaw-";            // its files' "format" markers, its other lower-case names (now exbit-…)
constexpr const char* ARCHIVE_EXTENSION = ".tinydaw.zip";   // its project archives (now .exbit.zip)
}  // namespace old_name
