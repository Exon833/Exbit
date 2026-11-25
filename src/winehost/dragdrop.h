// Drag and drop between a plugin's window and Exbit (the Wine host; dragdrop.cpp).
//
// Out: a plugin drags files out of its window with ole32's DoDragDrop (MIDI or audio a drum machine
// or a chord tool made). Wine's drag runs between Wine's own windows only, so the loaded modules'
// imports of DoDragDrop are pointed at the host's: a drag of files (CF_HDROP) is taken at once (the
// plugin is told they were copied) and the files, as Unix paths, are kept for Exbit, which goes on
// dragging them under the pointer (takeDraggedFiles; IDLE's news). Other drags go to Wine's own.
//
// In: files Exbit dropped on the plugin's window are handed to the window under that point as
// Windows does: to its OLE drop target (RegisterDragDrop; DragEnter, DragOver, Drop with a data
// object holding the files as CF_HDROP), else, for a window that accepts files, as WM_DROPFILES.
#pragma once
#include <windows.h>

#include <string>
#include <vector>

// (again) points the modules loaded now at the host's DoDragDrop: after the plugin loads, and after
// it opens its window (it may load more then)
void catchDragsOut();
// the files dragged out since last asked (Unix paths)
std::vector<std::string> takeDraggedFiles();
// files (Unix paths) dropped at (x, y) of `window` (its client area's pixels): how they were taken
// ("ole": by a drop target, "dropfiles": as WM_DROPFILES; "" not taken)
std::string dropFiles(HWND window, int x, int y, const std::vector<std::string>& files);
