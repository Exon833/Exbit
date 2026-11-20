// Numbers shared across the program (daw/config.py). Colours live in ui/theme.h.
#pragma once

namespace config {

// --- timeline geometry (pixels) ---
// The sizes marked "skin" are the defaults of the stylesheets' :root variables (data/skins/default.css, read with
// theme::metric): a skin may change them; these are used where no stylesheet sets them.
constexpr int RULER_H = 24;           // (skin: --ruler-height) time ruler at the top; the CPU monitor uses the same height
constexpr int TEMPO_LANE_H = 56;     // (skin: --tempo-lane-height) the tempo lane under the ruler (View → Tempo lane)
constexpr int TRACK_H = 72;           // default lane height
constexpr int MASTER_FOOTER_H = 14;   // under the master's controls: the update notice
constexpr int SCOPE_H = 78;            // the master scope (ui/scope_view.h), under them while shown
constexpr int MASTER_H = TRACK_H + MASTER_FOOTER_H;  // (skin: --master-height) the master lane and its row (pinned to the bottom)
constexpr int GROUP_H = 52;           // (skin: --group-height) a group's header lane (and its row in the panel)
constexpr int TAKE_H = 46;            // (skin: --take-height) a take lane (recorded takes shown under their track, for comping)
constexpr int AUTOMATION_H = 64;      // an automation lane under its track (and its row in the panel)
constexpr int AUTOMATION_MIN_H = 40, AUTOMATION_MAX_H = 320;
constexpr int AUTOMATION_PAD = 6;     // (skin: --automation-padding) px above and below the curve in an automation lane
constexpr int AUTOMATION_GRAB_PX = 6; // how close to a point the mouse must be to drag it
constexpr int MIN_TRACK_H = 60, MAX_TRACK_H = 320;
constexpr int LANE_EDGE_GRAB_PX = 4;  // how close to a lane's bottom edge the mouse must be to resize it
constexpr int PANEL_W = 250;          // (skin: --panel-width) right-hand panel with the track buttons
constexpr int OVERVIEW_H = 44;        // (skin: --overview-height) project overview bar above the editor (its default height)
constexpr double LOOP_MIN_S = 0.1;    // the shortest loop range
constexpr int OVERVIEW_MAX_H = 320;   // (skin: --overview-max-height)
constexpr double OVERVIEW_MIN_S = 30; // the overview spans at least this many seconds
constexpr int EXPLORER_W = 250;       // sample explorer on the left (its width can be dragged)

// --- zoom ---
constexpr double DEFAULT_PPS = 100.0;  // pixels per second
constexpr double MIN_PPS = 5.0, MAX_PPS = 2000.0;
constexpr double ZOOM_STEP = 1.25;     // per Ctrl+wheel notch
constexpr double EMPTY_SECONDS_AFTER_END = 60;

// --- ruler ---
constexpr double RULER_STEPS[] = {0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600};  // seconds
constexpr int RULER_MIN_TICK_PX = 70;

// --- bars and beats (timing.h) ---
constexpr double DEFAULT_BPM = 120.0;
constexpr double MIN_BPM = 20.0, MAX_BPM = 999.0;
constexpr int DEFAULT_BEATS_PER_BAR = 4;
constexpr int TAP_COUNT = 8;
constexpr double TAP_RESET_S = 2.0;
constexpr int MAX_BEATS_PER_BAR = 16;
constexpr int GRID_MIN_PX = 16;
constexpr int BAR_LABEL_MIN_PX = 40;
constexpr int BEAT_LABEL_MIN_PX = 40;

// --- audio ---
constexpr int FALLBACK_SAMPLE_RATE = 48000;  // used until an audio device is opened
constexpr int PEAK_BLOCK = 256;              // samples per waveform peak
constexpr int EDGE_FADE_FRAMES = 64;         // fade at every clip edge (~1.5 ms) so cut points don't click
constexpr int MIN_DB = -60, MAX_DB = 6;      // volume sliders; MIN_DB means silence
constexpr const char* FILE_FILTER = "Audio and MIDI (*.wav *.flac *.ogg *.mp3 *.aif *.aiff *.mid *.midi)";

// --- sample explorer ---
constexpr int EXPLORER_MAX_FILES = 200000;
constexpr int EXPLORER_MAX_RESULTS = 500;
constexpr double PREVIEW_GAIN = 0.7;

// --- freezing and bouncing tracks (audio/render.h) ---
constexpr int RENDER_BLOCK = 1024;
constexpr double RENDER_TAIL_S = 4.0;
constexpr double RENDER_SILENCE = 1e-5;
constexpr double RENDER_PREROLL_S = 2.0;

// --- editing ---
constexpr double LOOP_SEAM_S = 0.003;      // a looping audio clip crossfades this long into each repeat of its loop
constexpr double LOOP_DRAG_AHEAD_S = 8.0;  // dragging a looping audio clip longer, its audio is built this far ahead
constexpr int DRAG_THRESHOLD_PX = 3;
constexpr int CURSOR_ARROW_W = 11, CURSOR_ARROW_H = 7;  // (skin: --cursor-arrow-width, --cursor-arrow-height)
constexpr int TRANSPORT_BUTTON_PX = 28;
constexpr int TRANSPORT_ICON_PX = 14;
constexpr int CLIP_HANDLE_H = 16;     // (skin: --clip-header-height)
constexpr int CLIP_EDGE_PX = 6;       // (skin: --clip-edge-width)
constexpr double CLIP_MIN_S = 0.01;

// --- GUI refresh ---
constexpr int CPU_REFRESH_MS = 250;
constexpr int SKIN_WATCH_MS = 500;   // how often the skin's stylesheet is checked for changes (saved in an editor)
constexpr double CPU_AVERAGE_SECONDS = 5.0;
constexpr double METER_DECAY = 0.85;

}  // namespace config
