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

SRCS  := $(shell find src -name '*.cpp' | sort)
OBJS  := $(SRCS:%.cpp=$(BUILD)/%.o)
LIB_OBJS := $(filter-out $(BUILD)/src/main.o,$(OBJS))

TEST_SRCS := $(wildcard tests/*.cpp)
TEST_BINS := $(TEST_SRCS:tests/%.cpp=$(BUILD)/tests/%)

# test plugins (built from tests/plugins/: a VST2 synth and a VST3 effect, on Tiny DAW's own headers)
TEST_PLUGINS := $(BUILD)/test-plugins/TinyTestSynth.so \
                $(BUILD)/test-plugins/TinyTestFx.vst3/Contents/x86_64-linux/TinyTestFx.so

.PHONY: all clean test plugins
all: $(TARGET)

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

test: $(TEST_BINS) $(TEST_PLUGINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; TINYDAW_TEST_PLUGINS=$(BUILD)/test-plugins $$t; done

clean:
	rm -rf build build-debug tinydaw tinydaw-debug

-include $(OBJS:.o=.d) $(TEST_BINS:%=%.d)
