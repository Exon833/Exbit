// When a project was made (model.h Project::createdUnixSeconds), as its file keeps it: "created":
// "2026-11-25T14:03:00Z" (ISO-8601, UTC). Set when a project is made (File → New, a template, the empty
// start-up project, an import), kept by every save after. A file from before it has none: then the file's own
// creation time stands in (core/file_birth_time.h), else its modification time.
#pragma once
#include <cstdint>
#include <string>

#include "core/json.h"

namespace project_created {

// the time as saved: ISO-8601 in UTC ("2026-11-25T14:03:00Z"); "" for 0 or less (not known)
std::string isoText(int64_t createdUnixSeconds);
// when the project in `data`, read from the file at `path`, was made: its "created", else the file's birth time,
// else its modification time; 0 when nothing is known
int64_t readCreated(const Json& data, const std::string& path);

}  // namespace project_created
