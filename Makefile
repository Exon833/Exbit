# Tiny DAW (C++): one `make` builds the whole program, with nothing to fetch.
#
#   make            build ./tinydaw (release, -O2)
#   make DEBUG=1    build ./tinydaw-debug with -O0 -g and sanitizers (objects in build-debug/)
#   make test       build and run the unit tests (tests/*.cpp)
#   make clean
#
# Needs only a C++17 compiler and the system's own libraries: X11 (the windows), ALSA and
# PulseAudio (sound), libdl and pthreads (plugins, threads). Everything else (the GUI toolkit,
# font rendering, SVG and PNG, WAV/AIFF/FLAC/MP3/Ogg Vorbis, MIDI files, JSON, VST2 and VST3
# hosting) is Tiny DAW's own code in src/.
#
# With Wine's winegcc installed, it also builds tinydaw-wine-host.exe (src/winehost: the host
# that runs Windows plugins under Wine, plugins/bridge.h), on Tiny DAW's own Win32 declarations
# (src/winehost/include); without it, Windows plugins say they need it. WINEGCC=... picks another.
# With 32-bit build support (gcc-multilib, g++-multilib), also tinydaw-wine-host-32.exe, for 32-bit plugins.

CXX      ?= g++
TARGET   := tinydaw
BUILD    := build

ifeq ($(DEBUG),1)
  OPT := -O0 -g -fsanitize=address,undefined
  LDOPT := -fsanitize=address,undefined
  BUILD := build-debug
  TARGET := tinydaw-debug
else
  OPT := -O2 -g1 -DNDEBUG
  LDOPT :=
endif

CXXFLAGS ?=
CXXFLAGS += -std=c++17 $(OPT) -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            -MMD -MP -Isrc -pthread -DTINYDAW_COMMIT='"$(shell git rev-parse HEAD 2>/dev/null)"'
LDLIBS   := -lX11 -lXext -lXrandr -lXcursor -lXi -lasound -lpulse-simple -lpulse -ldl -pthread -lm

SRCS  := $(shell find src -name '*.cpp' -not -path 'src/winehost/*' | sort)
OBJS  := $(SRCS:%.cpp=$(BUILD)/%.o)
LIB_OBJS := $(filter-out $(BUILD)/src/main.o,$(OBJS))

TEST_SRCS := $(wildcard tests/*.cpp)
TEST_BINS := $(TEST_SRCS:tests/%.cpp=$(BUILD)/tests/%)

# test plugins (built from tests/plugins/: a VST2 synth and a VST3 effect, on Tiny DAW's own headers)
TEST_PLUGINS := $(BUILD)/test-plugins/TinyTestSynth.so \
                $(BUILD)/test-plugins/TinyTestFx.vst3/Contents/x86_64-linux/TinyTestFx.so

# --- the Wine host (Linux code inside Wine: built with $(CXX) for Windows' calling convention,
# linked by winegcc against Wine's DLLs) ---
WINEGCC  ?= $(shell command -v winegcc 2>/dev/null)
WINE_HOST := tinydaw-wine-host.exe
WINE_SRCS := src/winehost/host.cpp src/winehost/platform.cpp src/winehost/d2dfix.cpp src/winehost/dragdrop.cpp src/plugins/vst3host.cpp src/plugins/vst2host.cpp \
             src/plugins/plugin.cpp src/plugins/probe.cpp src/core/util.cpp src/core/json.cpp
WINE_OBJS := $(WINE_SRCS:%.cpp=build/winehost/%.o)  # (one host for both builds, always optimized)
WINE_BASE := -std=c++17 -O2 -g1 -DNDEBUG -fPIC -Wall -Wextra \
             -Wno-unused-parameter -Wno-missing-field-initializers -MMD -MP -Isrc/winehost/include -Isrc \
             -D_WIN32 -DTINYDAW_WINE_HOST
WINE_FLAGS := $(WINE_BASE) -D_WIN64 '-D__stdcall=__attribute__((ms_abi))' '-D__cdecl=__attribute__((ms_abi))'
# the 32-bit host, for 32-bit plugins (winegcc -m32: needs gcc-multilib/g++-multilib and Wine's i386 part;
# the same sources, Windows' 32-bit calling conventions)
WINE_HOST32 := tinydaw-wine-host-32.exe
WINE32_OBJS := $(WINE_SRCS:%.cpp=build/winehost32/%.o)
WINE32_FLAGS := $(WINE_BASE) -m32 '-D__stdcall=__attribute__((stdcall))' '-D__cdecl=__attribute__((cdecl))'
ifneq ($(WINEGCC),)
  ALL_WINE := $(WINE_HOST) $(WINE_HOST32)
endif

# --- Windows builds of the test plugins, and a test installer (for test_wine; with MinGW's
# x86_64-w64-mingw32-g++ installed, e.g. the g++-mingw-w64-x86-64 package; MINGW64=... picks another),
# and 32-bit ones with i686-w64-mingw32-g++ (g++-mingw-w64-i686; MINGW32=...). Linked statically: a
# Wine prefix has no MinGW runtime DLLs. Not in test-plugins/: the Linux tests list that folder ---
MINGW64  ?= $(shell command -v x86_64-w64-mingw32-g++ 2>/dev/null)
MINGW32  ?= $(shell command -v i686-w64-mingw32-g++ 2>/dev/null)
WIN_DIR := $(BUILD)/win-test-plugins
WIN_FLAGS := -std=c++17 -O2 -static -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Isrc
WIN_TEST_PLUGINS := $(WIN_DIR)/TinyTestFx.vst3 $(WIN_DIR)/TinyTestFx.moduleinfo.json \
                    $(WIN_DIR)/TinyTestSynth.dll $(WIN_DIR)/TinyTestInstaller.exe
WIN32_TEST_PLUGINS := $(WIN_DIR)/TinyTestFx32.vst3 $(WIN_DIR)/TinyTestSynth32.dll
ifneq ($(MINGW64),)
  ALL_WIN_TEST := $(WIN_TEST_PLUGINS)
  ifneq ($(MINGW32),)
    ALL_WIN_TEST += $(WIN32_TEST_PLUGINS)
  endif
endif

.PHONY: all clean test plugins winehost win-plugins
all: $(TARGET) $(ALL_WINE)

winehost: $(WINE_HOST) $(WINE_HOST32)

$(WINE_HOST32): $(WINE32_OBJS)
	$(WINEGCC) -m32 -mconsole -o $@ $^ -lstdc++ -lm -lpthread -lole32 -luser32 -ladvapi32 -lkernel32 -Wl,-z,defs

build/winehost32/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(WINE32_FLAGS) -c $< -o $@

$(WINE_HOST): $(WINE_OBJS)
	$(WINEGCC) -mconsole -o $@ $^ -lstdc++ -lm -lpthread -lole32 -luser32 -ladvapi32 -lkernel32 -Wl,-z,defs

build/winehost/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(WINE_FLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(LDOPT) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/tests/%: tests/%.cpp $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LDOPT) -o $@ $< $(LIB_OBJS) $(LDLIBS)

plugins: $(TEST_PLUGINS)

$(BUILD)/test-plugins/TinyTestSynth.so: tests/plugins/test_vst2_synth.cpp src/plugins/vst2.h
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -fPIC -shared -Isrc -o $@ $< -lX11

$(BUILD)/test-plugins/TinyTestFx.vst3/Contents/x86_64-linux/TinyTestFx.so: tests/plugins/test_vst3_fx.cpp src/plugins/vst3_api.h tests/plugins/TinyTestFx.moduleinfo.json
	@mkdir -p $(dir $@) $(BUILD)/test-plugins/TinyTestFx.vst3/Contents/Resources
	$(CXX) -std=c++17 -O2 -fPIC -shared -Isrc -o $@ $< -lX11
	cp tests/plugins/TinyTestFx.moduleinfo.json $(BUILD)/test-plugins/TinyTestFx.vst3/Contents/Resources/moduleinfo.json

win-plugins: $(ALL_WIN_TEST)

$(WIN_DIR)/TinyTestFx.vst3: tests/plugins/test_vst3_fx.cpp tests/plugins/win_dnd.h src/plugins/vst3_api.h
	@mkdir -p $(dir $@)
	$(MINGW64) $(WIN_FLAGS) -shared -o $@ $< -lgdi32 -luser32 -lole32 -lshell32 -luuid

$(WIN_DIR)/TinyTestFx.moduleinfo.json: tests/plugins/TinyTestFx.moduleinfo.json
	@mkdir -p $(dir $@)
	cp $< $@

$(WIN_DIR)/TinyTestSynth.dll: tests/plugins/test_vst2_synth.cpp tests/plugins/win_dnd.h src/plugins/vst2.h
	@mkdir -p $(dir $@)
	$(MINGW64) $(WIN_FLAGS) -shared -o $@ $< -lgdi32 -luser32 -lole32 -lshell32 -luuid

# (32-bit: --kill-at gives GetPluginFactory, a __stdcall function, its plain name, as Windows hosts look for it)
$(WIN_DIR)/TinyTestFx32.vst3: tests/plugins/test_vst3_fx.cpp tests/plugins/win_dnd.h src/plugins/vst3_api.h
	@mkdir -p $(dir $@)
	$(MINGW32) $(WIN_FLAGS) -shared -Wl,--kill-at -o $@ $< -lgdi32 -luser32 -lole32 -lshell32 -luuid

$(WIN_DIR)/TinyTestSynth32.dll: tests/plugins/test_vst2_synth.cpp tests/plugins/win_dnd.h src/plugins/vst2.h
	@mkdir -p $(dir $@)
	$(MINGW32) $(WIN_FLAGS) -shared -Wl,--kill-at -o $@ $< -lgdi32 -luser32 -lole32 -lshell32 -luuid

$(WIN_DIR)/TinyTestInstaller.exe: tests/plugins/test_installer.cpp
	@mkdir -p $(dir $@)
	$(MINGW64) $(WIN_FLAGS) -o $@ $<

test: $(TEST_BINS) $(TEST_PLUGINS) $(ALL_WINE) $(ALL_WIN_TEST)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; TINYDAW_TEST_PLUGINS=$(BUILD)/test-plugins TINYDAW_WIN_TEST_PLUGINS=$(WIN_DIR) $$t; done

clean:
	rm -rf build build-debug tinydaw tinydaw-debug $(WINE_HOST) $(WINE_HOST).so $(WINE_HOST32) $(WINE_HOST32).so

-include $(OBJS:.o=.d) $(TEST_BINS:%=%.d) $(WINE_OBJS:.o=.d) $(WINE32_OBJS:.o=.d)
