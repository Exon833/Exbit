// The program's own folders under its old name (old_name::FOLDER): on the first start under the new name, the
// old folder becomes the new one, so settings, recovery files, presets and the rest carry over.
#pragma once
#include <string>

namespace folder_migration {

// The folder to use: `newFolder`, unless it is missing and `oldFolder` is there; then the old one is moved to
// the new name, or copied when it can't be moved (the old one kept), and, if neither works, the old folder is
// used as it is (nothing of it deleted). Each step is logged (util::debugf).
std::string useNewFolderOrMoveOldOne(const std::string& newFolder, const std::string& oldFolder);

// Copies every file under `from` to the same place under `to` (folders made as needed); false at the first
// file that can't be copied. `from` is left as it is.
bool copyFolderContents(const std::string& from, const std::string& to);

}  // namespace folder_migration
