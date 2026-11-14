# What the main window must do with each screen (from the screen ports' reports)

Spec: `daw/ui/main_window.py` and `main.py`. Read the headers named here for exact signatures.

## Timeline (`ui/timeline.h`, also `ui/overview.h`, `ui/master_lane.h`)
- `Timeline(Project&, audio::Engine&, ui::ScrollArea*)`, e.g. `scroll->makeContent<Timeline>(project, engine, scroll)`.
- Master lane: adopt `takeMasterView()` into `scroll->viewport()`, pin it to the bottom (MASTER_H), `raise()` it
  (PinToBottom from `ui/widgets.h` if it fits).
- Every frame `setPlayhead(engine.heardPos())`; `relayout()` on window resize (it also refits on viewport resize, by
  chaining the viewport's `onEventFilter`).
- Signals: `tracksChanged`, `editNotes(ClipPtr)`, `heightsChanged`, `loopChanged`, `automationEdited`,
  `currentTrackChanged`, `loopRangeChosen`. Fields: `pps`, `selection`, `cursor`, `cursorX`, `playheadX`, `viewLeft`,
  `randomTrackColors`, `currentTrack`, `recording` (optional pair track/start frame), `masterView`.
- Methods are the Python's in camelCase (`addFile`, `addTrack`, `setCursor(int64_t)`, `seek`, `setLoop`, `relayout`,
  `lanesChanged`, `trackTops()` keyed by `const Track*`…).
- `Overview(project, engine, timeline, scroll, height)`: `refresh()` every frame, `forgetImage()` after a skin
  change; Python's `resized` signal is `resizedTo(int)`.

## MIDI editor and Explorer (`ui/midi_editor.h`, `ui/explorer.h`)
- `new MidiEditor(ui::Window* parent, ClipPtr, Project&, audio::Engine&, std::function<void()> onChange,
  MidiEditor::TimelineLink{cursor, setCursor, seek})` — non-modal, deletes itself on close; keep a map Clip* →
  editor, erase on `editor->closed`; `showWindow()`, `raiseWindow()`; check: `editor->clip`, `updateTitle()`,
  `close()`. Space/Home work in it when the transport's shortcuts are `ShortcutScope::ApplicationWide`.
- `new Explorer(std::function<void(const std::string&)> addFile, audio::Engine&)` — a Widget for the left dock
  (EXPLORER_W, width draggable); drags out `ui::DragData{files, kind="files"}`.

## Dialogs, skins, recording, rendering (`ui/skin.h`, `skin_dialog.h`, `metronome_dialog.h`, `audio_dialog.h`,
`recording.h`, `track_render.h`, `mix_export.h`, `color_picker.h`)
- skin: `Skin{name, colors, fonts, path, builtin}`, `read`, `write`, `fileName`, `library()`, `find(name)`,
  `apply(skin)` (theme + colors cache; then restyle: App::repaintAll, panel rebuild, overview forgetImage),
  `onApply(fn)`, `importSkin(parent, choose)`, `exportSkin(parent, skin)` → path, `openSkinsFolder()`.
- `SkinDialog(parent, skin, apply, saved, tab)` ("Tracks and clips" for Colours → Default…);
  `MetronomeDialog(parent, settings&, apply, test)`, `metronome_dialog::loadSettings()`,
  `metronome_dialog::testClick(engine, click)`; `AudioPropertiesDialog(parent, driverType, deviceId, bufferSize,
  exclusive, engine)` → after Accepted: `driverType`, `device`, `bufferSize`, `exclusive`.
- `Recording(RecordingHost, engine)`: `toggle()`, `showSettings()`, `active()`; hooks struct (see header;
  `showRecording(track, start)` sets the timeline's `recording`).
- `TrackRenderer(TrackRenderHost, engine)`: `fillMenu(menu, track)` = the lane panel's fill_menu.
- `MixExporter(MixExportHost, engine)`: `renderSelection()`, `renderProject()`.

## Lane panel, master strip, transport, CPU, update notice (`ui/lane_panel.h`, `master_strip.h`,
`transport_bar.h`, `cpu_monitor.h`, `update_notice.h`, `widgets.h`, `level_meter.h`)
- TransportBar (widget; height = sizeHint().y): `std::unique_ptr<TransportAction>` members with `triggered`/`toggled`,
  `isChecked()`, shortcuts (Play/Start ApplicationWide): addFileAction, addAudioTrackAction, addMidiTrackAction,
  playAction, recordAction, startAction, loopAction, followAction, beatsAction, thirdsAction. `recordButton` /
  `metronomeButton` `rightClicked`; `tapButton->pressed` (T emits it too); `bpmBox` / `signatureBox` `valueChanged`,
  `->editor()->returnPressed`; `metronomeButton->toggled`. Calls: `showTiming`, `setPlaying`, `setRecording`, `setTime`.
- `MasterStrip(engine, openMasterPlugins)`; pin `PinToBottom(master.releaseFrame(), panelScroll->viewport(), MASTER_H,
  theme::SCROLLBAR_W)`. `refreshFx`, `settings()`/`apply()` (project_io::MasterSettings), `restyle`, `showUpdate`,
  fields `meter`, `loudness`.
- `LanePanel(project, timeline, master, engine.channels, openTrackPlugins, fillRenderMenu, {fillInstrumentMenu,
  editInstrument})`: setFixedHeight itself → scroll content with widgetResizable=false; connects to the timeline's
  signals itself. `setChannels`, `setMetersVisible`, `rebuild`; maps `meters`, `loudness` (TrackPtr), `groupMeters`.
- `CpuMonitor::showLoad(optional<pair<cur, avg>>, late)`. `UpdateChecker`: `done.connect(master.showUpdate)`, `start()`.
- Every frame: `metering->takePeaks(m, tracks)`; `master.meter->push(m)`; panel meters `push(found ? &tracks[t.get()] :
  nullptr)`; group meters `push(level_meter::loudest({member peaks}))`.
- Every 250 ms: `cpu.showLoad(...)`; if loudness on: `updateLoudness()`, `showValues(m, s, i)` for master.loudness and
  each panel.loudness from `metering->read(key, …)`.
## Plugins UI (`ui/plugin_host.h`, `plugin_dialog.h`, `instrument_menu.h`, `plugin_folders_dialog.h`)
Each header starts with a "THE MAIN WINDOW:" comment.
- `PluginHost(this, project, engine, [&]{ return prefs.vst2List(); })`; set its `statusBar` (or `showStatus`).
  Every frame `idle(project.pluginSlots())`. `findKinds()` 1.5 s after start; `restore(slots)` after opening a project
  (returns the names that failed); `closeEditor()` + `stopFindingKinds()` on close; `rescan()` after VST2 folders change;
  `edited` flag for unsaved changes (read and clear).
- FX windows: `new PluginsDialog(this, host, title, project, track /*nullptr = master*/, onChange)`,
  `new InstrumentPanelDialog(…, track, onChange)`; they delete themselves on close → forget them on `finished`; show
  with `open()` + `raiseWindow()`; after tracks change use `owner`, `INSTRUMENTS`, `refresh()`, `close()`.
- `instrument_menu::fillInstrumentMenu(menu, parent, host, track, chosen, openPanel)`,
  `instrument_menu::editInstrument(parent, host, track, openPanel)`.
- `PluginFoldersDialog(parent, folders)` → `folders()`.
