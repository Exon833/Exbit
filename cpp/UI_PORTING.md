# Porting the screens (daw/ui/*.py → cpp/src/ui/)

Read first: `cpp/PORTING.md` (rules), `cpp/API_NOTES.md` (engine, projects, files, plugins APIs).

- **Toolkit** (the Qt replacement): `src/ui/toolkit/{widget,layout,controls,menu,dialogs,style}.h` — read
  them fully. Qt → toolkit: QWidget → ui::Widget (paint(Painter&), mousePress/Move/Release/DoubleClick,
  wheel, keyPress, contextMenu, enter/leave, dragEnter/drop; return true = handled), QMainWindow/QDialog →
  ui::Window / ui::Dialog, QLabel → Label, QToolButton/QPushButton → Button (properties "lit", "toggle",
  "accent", "autoRaise"), QMenu → Menu (exec/popup, aboutToShow), QMessageBox → ui::msg::…, QInputDialog →
  ui::getText/getItem, QFileDialog → ui::files::…, QTimer → App::get().singleShot/every/onFrame,
  QSettings → `settings::` (core/util.h), Signal → ui::Signal<…>, QPainter → Painter (gfx/painter.h),
  QImage/QPixmap → Image, QColor → Color (gfx/geom.h), fonts → theme::uiFont(), theme::monoFont(pt).
- **Shared UI modules (done)**: `ui/theme.h`, `ui/formatting.h` (fmt::time/bars/rulerStep/stepDecimals/db),
  `ui/lanes.h` (lanes::layout), `ui/selection.h`, `ui/colors.h` (clip/track colours, contrast),
  `ui/paint_helpers.h` (paint::groupFill = timeline.group_fill, paint::cursor = timeline.paint_cursor).
  Contracts written by the lead, implemented by the dialogs agent: `ui/color_picker.h` (ColorPickerDialog),
  `ui/automation_dialog.h` (ParameterDialog — implemented by the plugins-UI agent).
- **Conventions** so code written in parallel fits together: `daw/ui/X.py` → `src/ui/X.h` + `X.cpp`;
  classes keep their Python names; methods and fields camelCase (`tracks_changed` → `tracksChanged`);
  Qt signals → `ui::Signal<…>` members; module-level functions → a namespace named after the module
  (`instrument_menu.fill_instrument_menu` → `instrument_menu::fillInstrumentMenu`).
- **Write your headers first** (in your first steps), as faithful ports of the Python classes' public
  interface, so the others can code against them. If a header you need from another agent isn't there
  yet, carry on and check again later; never write someone else's file.
- **The main window** (`main_window.py`) is ported by the lead: read how it builds and calls your
  classes and give them what it needs (constructor arguments, signals, methods).
- **Model access**: the GUI thread reads `project.tracks` etc. directly (`*list`, range-for) and replaces
  lists by assignment; never mutate a list in place. Clips are edited by making copies (edits.h).
- **Look**: exactly as the Python program (sizes from config.h, colours by theme name, the flat style).
  Check your drawing by painting widgets into an Image and saving PNGs (gfx/image.h savePng) to
  `cpp/build/ui-samples/<yourarea>/`, then LOOK at them with the Read tool. Unit tests (tests/test_ui_<area>.cpp)
  for logic (snapping, hit testing, menus' contents, edits).
- **Build**: `make build/src/ui/<file>.o`; link tests by hand against the objects you need. The program's
  `main.cpp` and `main_window` don't exist yet: don't try to run the program.
- **Keys (toolkit rule, as Qt):** a window's shortcuts get a key first, unless the focus widget claims it
  with `overridesShortcut()` (LineEdit/SpinBox claim printable keys, editing keys and Ctrl+A/C/V/X/Z/Y).
  Widgets that must see Space/Delete/S… themselves (the timeline, the MIDI editor) override
  `overridesShortcut()` or handle them in keyPress when no shortcut owns them. `TreeWidget::onFill` runs when a
  node is expanded while `filled` is false. `Layout`s are applied lazily before painting (`updateGeometry()`).
