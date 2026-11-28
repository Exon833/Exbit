#include "reference_program.h"

#include <cstdio>
#include <iterator>
#include <sstream>

#include "speed_settings.h"

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace plugin_speed {

std::vector<std::vector<double>> runReferenceProgram(const std::vector<std::string>& commandWords, std::string& error) {
    std::string command;
    for (const auto& word : commandWords)
#ifdef _WIN32
        command += "\"" + word + "\" ";
    command = "\"" + command + "\"";  // (cmd /c takes the outer quotes off)
#else
        command += "'" + word + "' ";  // (no word of ours holds a ')
#endif
    printf("   running %s\n", command.c_str());
    fflush(stdout);
    std::string output;
    if (FILE* program = popen(command.c_str(), "r")) {
        char chunk[65536];
        for (size_t n; (n = fread(chunk, 1, sizeof chunk, program)) > 0;) output.append(chunk, n);
        if (pclose(program) != 0) error = "the reference program failed";
    } else {
        error = "the reference program couldn't be started";
    }
    std::vector<std::vector<double>> timesPerSize;
    std::istringstream lines(output);
    for (std::string line; std::getline(lines, line);) {
        std::istringstream words(line);
        std::string label;
        int frames = 0;
        if (!(words >> label >> frames) || label != "frames") continue;
        std::vector<double> seconds;
        for (double microseconds; words >> microseconds;) seconds.push_back(microseconds * 1e-6);
        timesPerSize.push_back(seconds);
    }
    if (error.empty() && timesPerSize.size() != std::size(FRAME_SIZES)) error = "the reference program printed no times";
    if (!error.empty()) timesPerSize.clear();
    return timesPerSize;
}

}  // namespace plugin_speed
