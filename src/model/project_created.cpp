// When a project was made, as its file keeps it (model/project_created.h)
#include "model/project_created.h"

#include <ctime>

#include "core/file_birth_time.h"
#include "update_check.h"

namespace project_created {

std::string isoText(int64_t createdUnixSeconds) {
    if (createdUnixSeconds <= 0) return "";
    time_t seconds = (time_t)createdUnixSeconds;
    struct tm utc {};
    char text[32] = "";
    if (!gmtime_r(&seconds, &utc) || !strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc)) return "";
    return text;
}

int64_t readCreated(const Json& data, const std::string& path) {
    const Json& saved = data["created"];
    int64_t created = saved.isString() ? (int64_t)update_check::parseTime(saved.toString()) : 0;
    return created > 0 ? created : file_birth_time::fileBirthTime(path);
}

}  // namespace project_created
