// When a file was made: its birth time where the system keeps one (statx on Linux, the creation time on
// Windows), else when it was last changed. Projects saved before they remembered when they were made
// (model.h Project::createdUnixSeconds) take it from their file.
#pragma once
#include <cstdint>
#include <string>

namespace file_birth_time {

// when the file at `path` was made, in seconds since the epoch: its birth time, else its modification time;
// 0 when neither is known (no such file)
int64_t fileBirthTime(const std::string& path);

}  // namespace file_birth_time
