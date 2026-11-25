// Exbit was called Tiny DAW: what was made under the old name still works.
//   environment variables  EXBIT_<NAME>, else TINYDAW_<NAME> (core/program_environment.h)
//   folders                the settings and data folders named TinyDAW are moved to the Exbit ones on the first
//                          start, copied when they can't be moved, and kept and used when neither works
//                          (core/folder_migration.h, plat::configDir(), paths::dataDir())
//   files                  projects, track and effect presets, synth patches and JSON skins marked "tinydaw-…",
//                          and archives named .tinydaw.zip, open as before (core/format_marker.h)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "audio/synth_patch.h"
#include "check.h"
#include "core/folder_migration.h"
#include "core/format_marker.h"
#include "core/program_environment.h"
#include "core/util.h"
#include "model/collect.h"
#include "model/model.h"
#include "model/presets.h"
#include "model/project_io.h"
#include "paths.h"
#include "platform/platform.h"
#include "ui/skin.h"

static std::string scratch;  // every file of the test, under build/tests

// a folder of the scratch one, emptied first
static std::string freshFolder(const std::string& name) {
    std::string folder = util::joinPath(scratch, name);
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    util::makeDirs(folder);
    return folder;
}

static std::string fileText(const std::string& path) {
    std::string text;
    return util::readFile(path, text) ? text : std::string("(missing)");
}

static void testEnvironmentVariables() {
    unsetenv("EXBIT_OLD_NAME_TEST");
    unsetenv("TINYDAW_OLD_NAME_TEST");
    CHECK(environment::readProgramVariable("OLD_NAME_TEST") == nullptr);
    setenv("TINYDAW_OLD_NAME_TEST", "old", 1);
    const char* value = environment::readProgramVariable("OLD_NAME_TEST");
    CHECK(value && std::strcmp(value, "old") == 0);  // (set by a script from before the rename)
    setenv("EXBIT_OLD_NAME_TEST", "new", 1);
    value = environment::readProgramVariable("OLD_NAME_TEST");
    CHECK(value && std::strcmp(value, "new") == 0);  // the new name first
    unsetenv("EXBIT_OLD_NAME_TEST");
    unsetenv("TINYDAW_OLD_NAME_TEST");
}

static void testFolderMigration() {
    std::string root = freshFolder("migration");
    std::string oldFolder = util::joinPath(root, "TinyDAW"), newFolder = util::joinPath(root, "Exbit");
    util::makeDirs(util::joinPath(oldFolder, "recovery"));
    util::writeFile(util::joinPath(oldFolder, "settings.json"), "{\"a\": 1}");
    util::writeFile(util::joinPath(oldFolder, "recovery/song.json"), "song");

    // moved: the new folder holds what the old one did
    CHECK(folder_migration::useNewFolderOrMoveOldOne(newFolder, oldFolder) == newFolder);
    CHECK(fileText(util::joinPath(newFolder, "settings.json")) == "{\"a\": 1}");
    CHECK(fileText(util::joinPath(newFolder, "recovery/song.json")) == "song");
    // once the new folder is there, the old name is left alone
    util::makeDirs(oldFolder);
    util::writeFile(util::joinPath(oldFolder, "settings.json"), "{\"b\": 2}");
    CHECK(folder_migration::useNewFolderOrMoveOldOne(newFolder, oldFolder) == newFolder);
    CHECK(fileText(util::joinPath(newFolder, "settings.json")) == "{\"a\": 1}");
    CHECK(fileText(util::joinPath(oldFolder, "settings.json")) == "{\"b\": 2}");
    // neither folder: the new one (made by the caller, as before)
    CHECK(folder_migration::useNewFolderOrMoveOldOne(util::joinPath(root, "none-new"), util::joinPath(root, "none-old")) ==
          util::joinPath(root, "none-new"));

    // the fallback when it can't be moved: copied, the old folder kept as it was
    std::string copied = util::joinPath(root, "copied");
    CHECK(folder_migration::copyFolderContents(oldFolder, copied));
    CHECK(fileText(util::joinPath(copied, "settings.json")) == "{\"b\": 2}");
    CHECK(fileText(util::joinPath(oldFolder, "settings.json")) == "{\"b\": 2}");

    // neither moved nor copied (the new folder can't be made: a file is where its parent would be): the old
    // folder is used, its files untouched
    std::string blocker = util::joinPath(root, "a-file");
    util::writeFile(blocker, "x");
    std::string unreachable = util::joinPath(blocker, "Exbit");
    CHECK(folder_migration::useNewFolderOrMoveOldOne(unreachable, oldFolder) == oldFolder);
    CHECK(fileText(util::joinPath(oldFolder, "settings.json")) == "{\"b\": 2}");
}

// the settings folder and the data folder, in a home of the test's own
static void testProgramFolders() {
    std::string home = freshFolder("home");
#ifndef _WIN32  // (Windows' settings folder is the system's Roaming AppData: not one a test can move)
    std::string config = util::joinPath(home, "config");
    util::makeDirs(util::joinPath(config, "TinyDAW"));
    util::writeFile(util::joinPath(config, "TinyDAW/settings.json"), "{\"theme\": \"dark\"}");
    setenv("XDG_CONFIG_HOME", config.c_str(), 1);
    CHECK(plat::configDir() == util::joinPath(config, "Exbit"));
    CHECK(fileText(util::joinPath(config, "Exbit/settings.json")) == "{\"theme\": \"dark\"}");
    CHECK(!util::isDir(util::joinPath(config, "TinyDAW")));  // (moved, not copied: nothing left behind twice)
    // a fresh home: the new folder is made, nothing else
    std::string fresh = util::joinPath(home, "fresh-config");
    util::makeDirs(fresh);
    setenv("XDG_CONFIG_HOME", fresh.c_str(), 1);
    CHECK(plat::configDir() == util::joinPath(fresh, "Exbit") && util::isDir(util::joinPath(fresh, "Exbit")));
    const char* dataVariable = "XDG_DATA_HOME";
#else
    const char* dataVariable = "APPDATA";
#endif
    std::string data = util::joinPath(home, "data");
    util::makeDirs(util::joinPath(data, "TinyDAW/recovery"));
    util::writeFile(util::joinPath(data, "TinyDAW/recovery/autosave.json"), "kept");
    setenv(dataVariable, data.c_str(), 1);
    paths::setDataDir("");
    CHECK(paths::dataDir() == util::joinPath(data, "Exbit"));
    CHECK(fileText(util::joinPath(data, "Exbit/recovery/autosave.json")) == "kept");
    CHECK(paths::dataDir("recovery") == util::joinPath(data, "Exbit/recovery"));
}

static void testFileMarkers() {
    CHECK(format_marker::isFormat("exbit-project", "exbit-project"));
    CHECK(format_marker::isFormat("tinydaw-project", "exbit-project"));
    CHECK(!format_marker::isFormat("tinydaw-synth", "exbit-project"));
    CHECK(!format_marker::isFormat("", "exbit-project"));
    CHECK(std::string(project_io::FORMAT) == "exbit-project");
    CHECK(std::string(subsynth::FORMAT) == "exbit-synth");
    CHECK(std::string(presets::FORMAT) == "exbit-preset");

    std::string folder = freshFolder("files");
    const int sampleRate = 48000;
    // a project saved by Tiny DAW opens; saved again, it says Exbit
    std::string oldProject = util::joinPath(folder, "old.json");
    util::writeFile(oldProject, R"({"format": "tinydaw-project", "version": 1, "tracks": [{"name": "Old", "kind": "midi"}],
                                    "clips": []})");
    Project project;
    project_io::MasterSettings master;
    std::string error;
    CHECK(project_io::loadProject(oldProject, project, sampleRate, master, error));
    CHECK(project.tracks.size() == 1 && project.tracks[0]->name == "Old");
    std::string newProject = util::joinPath(folder, "new.json");
    CHECK(project_io::saveProject(newProject, project, sampleRate, master, error));
    CHECK(fileText(newProject).find("\"exbit-project\"") != std::string::npos);
    // an old track preset is one
    std::string trackPreset = util::joinPath(folder, "Keys.json");
    util::writeFile(trackPreset, R"({"format": "tinydaw-project", "preset": "track", "tracks": [], "clips": []})");
    CHECK(presets::isTrackPresetFile(trackPreset));
    // an old effect preset
    std::string effectPreset = util::joinPath(folder, "Warm.json");
    util::writeFile(effectPreset, R"({"format": "tinydaw-preset", "version": 1, "kind": "effect",
                                      "plugin": {"name": "Fx"}, "state": "AAAA"})");
    std::string state, plugin;
    CHECK(presets::readEffect(effectPreset, state, plugin, error) && plugin == "Fx" && state.size() == 3);
    // an old synth patch
    std::string patchPath = util::joinPath(folder, "Lead.json");
    util::writeFile(patchPath, R"({"format": "tinydaw-synth", "name": "Lead", "params": {}})");
    subsynth::Patch patch;
    CHECK(subsynth::load(patchPath, patch, error) && patch.name == "Lead");
    // an old JSON skin
    std::string css;
    CHECK(skin::cssFromJson(R"({"format": "tinydaw-skin", "name": "Old"})", "Fallback", css, error));

    // archives: new ones are .exbit.zip, old ones still open, and the file chooser shows both
    CHECK(std::string(collect::ARCHIVE_EXTENSION) == ".exbit.zip");
    CHECK(collect::isArchive("a/Song.exbit.zip") && collect::isArchive("a/Song.tinydaw.zip"));
    CHECK(collect::archiveStem("a/Song.exbit.zip") == "Song");
    CHECK(collect::archiveStem("a/Song.tinydaw.zip") == "Song");
    std::string patterns = collect::archivePatterns();
    CHECK(patterns.find("*.exbit.zip") != std::string::npos && patterns.find("*.tinydaw.zip") != std::string::npos);
}

int main() {
    scratch = util::absolutePath("build/tests/old-name-scratch");
    util::makeDirs(scratch);
    testEnvironmentVariables();
    testFolderMigration();
    testProgramFolders();
    testFileMarkers();
    TEST_MAIN_END();
}
