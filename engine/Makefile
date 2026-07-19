# ============================================================
#  engine/Makefile — portable C build for the HX-421 audio core
#
#  Builds every engine source into objects, links each portable
#  test suite against them, and runs them. Host-independent: no
#  OS/VM/hardware sources here (see README.md / PROVENANCE.md).
#
#  Usage:
#    make          # build engine objects + all test binaries
#    make test     # build + run every test suite
#    make clean
#
#  A C11 compiler is required (some sources use <stdatomic.h>).
#  Override CC if gcc is not on PATH, e.g.:
#    make CC=/c/msys64/mingw64/bin/gcc
# ============================================================

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -I. -Itests/include
LDFLAGS ?=

BUILD := build

ifeq ($(OS),Windows_NT)
  EXE := .exe
else
  EXE :=
endif

# ---- engine sources (order-independent; each is a TU) ----
ENGINE_SRC := \
  containers/ring_buffer.c \
  containers/bitset.c \
  audio/audio_mixer.c \
  audio/music_player.c \
  audio/audio_pool.c \
  audio/audio_arbiter.c \
  audio/audio_pool_stream.c \
  audio/audio_ring_stream.c \
  audio/audio_fft.c \
  audio/audio_fft_kernel.c \
  audio/audio_wav_read.c \
  audio/audio_file_stream.c \
  audio/audio_sink_wav.c \
  service.c

ENGINE_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(ENGINE_SRC))

# ---- portable test suites (each has its own main) ----
TESTS := \
  test_audio_mixer \
  test_audio_fft \
  test_audio_pool \
  test_audio_arbiter \
  test_audio_pool_stream \
  test_audio_pool_stream_integration \
  test_audio_wav_read \
  test_audio_file_stream \
  test_audio_sink_wav \
  test_music_player

TEST_BIN := $(patsubst %,$(BUILD)/%$(EXE),$(TESTS))

INCLUDE_DIR := ../include
RUNTIME_DIR := ../runtime
TOOLS_DIR   := ../tools

.PHONY: all test clean demo dll dlltest
all: $(TEST_BIN)

# engine object compile
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# test link: each tests/<name>.c against the whole engine
$(BUILD)/test_%$(EXE): tests/test_%.c $(ENGINE_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(ENGINE_OBJ) -o $@ $(LDFLAGS)

# build + run every suite; non-zero exit if any suite fails
test: $(TEST_BIN)
	@fail=0; \
	for t in $(TEST_BIN); do \
	  echo "==== $$t ===="; \
	  "./$$t" || fail=1; \
	done; \
	if [ $$fail -eq 0 ]; then echo "ALL SUITES PASSED"; else echo "SOME SUITES FAILED"; fi; \
	exit $$fail

# ---- demo: the end-to-end player (proves the service renders) ----
# Links demo/player.c against the whole engine (which now includes
# service.o). Needs the math lib for the procedural sine/click.
$(BUILD)/player$(EXE): demo/player.c $(ENGINE_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(ENGINE_OBJ) -o $@ $(LDFLAGS) -lm

# Build + run the player, producing out.wav in $(BUILD).
demo: $(BUILD)/player$(EXE)
	cd $(BUILD) && ./player$(EXE)

# ---- hx421.dll: the coprocessor runtime (engine + the hx421.h ABI shim) ----
# The runtime shim (../runtime) implements include/hx421.h over the hxa_* API.
$(BUILD)/hx421_runtime.o: $(RUNTIME_DIR)/hx421_runtime.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -DHX421_BUILDING_DLL -c $< -o $@

# The DLL's own WASAPI live-audio sink (Windows only; empty TU elsewhere).
$(BUILD)/hx421_wasapi.o: $(RUNTIME_DIR)/hx421_wasapi.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -DHX421_BUILDING_DLL -c $< -o $@

$(BUILD)/hx421.dll: $(ENGINE_OBJ) $(BUILD)/hx421_runtime.o $(BUILD)/hx421_wasapi.o
	$(CC) -shared -o $@ $(ENGINE_OBJ) $(BUILD)/hx421_runtime.o $(BUILD)/hx421_wasapi.o -lm -lole32 $(LDFLAGS)

dll: $(BUILD)/hx421.dll

# Host harness: LoadLibrary the DLL, pull audio through the ABI, write dll_out.wav.
$(BUILD)/hx421_host_test$(EXE): $(TOOLS_DIR)/hx421_host_test.c $(BUILD)/hx421.dll
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) $< -o $@ $(LDFLAGS) -lm

dlltest: $(BUILD)/hx421_host_test$(EXE)
	cd $(BUILD) && ./hx421_host_test$(EXE)

clean:
	rm -rf $(BUILD)
