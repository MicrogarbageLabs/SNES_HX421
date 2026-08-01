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

# Metatile map expansion: reference implementation AND the spec the RTL block
# is written from, so it stays dependency-free and integer-only.
$(BUILD)/hx421_metatile.o: $(RUNTIME_DIR)/hx421_metatile.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -DHX421_BUILDING_DLL -c $< -o $@

# TBDR rasteriser and the 3D object registry. Same rule as the metatile block:
# integer-only reference implementations that double as the RTL spec.
$(BUILD)/hx421_raster.o: $(RUNTIME_DIR)/hx421_raster.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -DHX421_BUILDING_DLL -c $< -o $@

$(BUILD)/hx421_scene3d.o: $(RUNTIME_DIR)/hx421_scene3d.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -DHX421_BUILDING_DLL -c $< -o $@

$(BUILD)/hx421_collide.o: $(RUNTIME_DIR)/hx421_collide.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -DHX421_BUILDING_DLL -c $< -o $@

$(BUILD)/hx421_mask.o: $(RUNTIME_DIR)/hx421_mask.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -DHX421_BUILDING_DLL -c $< -o $@

DLL_OBJ = $(BUILD)/hx421_runtime.o $(BUILD)/hx421_wasapi.o $(BUILD)/hx421_metatile.o \
          $(BUILD)/hx421_raster.o $(BUILD)/hx421_scene3d.o \
          $(BUILD)/hx421_collide.o $(BUILD)/hx421_mask.o

$(BUILD)/hx421.dll: $(ENGINE_OBJ) $(DLL_OBJ)
	$(CC) -shared -o $@ $(ENGINE_OBJ) $(DLL_OBJ) -lm -lole32 $(LDFLAGS)

dll: $(BUILD)/hx421.dll

# Host harness: LoadLibrary the DLL, pull audio through the ABI, write dll_out.wav.
$(BUILD)/hx421_host_test$(EXE): $(TOOLS_DIR)/hx421_host_test.c $(BUILD)/hx421.dll
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) $< -o $@ $(LDFLAGS) -lm

dlltest: $(BUILD)/hx421_host_test$(EXE)
	cd $(BUILD) && ./hx421_host_test$(EXE)

# ---- runtime suites: the integer reference blocks that double as RTL spec ----
# These live in ../tools and were previously built by hand, which meant nothing
# noticed when one stopped compiling. Each is self-contained: its own main, and
# it links only the runtime TUs it exercises.
$(BUILD)/hx421_metatile_test$(EXE): $(TOOLS_DIR)/hx421_metatile_test.c $(RUNTIME_DIR)/hx421_metatile.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(RUNTIME_DIR) $^ -o $@ $(LDFLAGS)

$(BUILD)/hx421_raster_test$(EXE): $(TOOLS_DIR)/hx421_raster_test.c $(RUNTIME_DIR)/hx421_raster.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(RUNTIME_DIR) $^ -o $@ $(LDFLAGS)

$(BUILD)/hx421_scene3d_test$(EXE): $(TOOLS_DIR)/hx421_scene3d_test.c \
                                   $(RUNTIME_DIR)/hx421_scene3d.c $(RUNTIME_DIR)/hx421_raster.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(RUNTIME_DIR) $^ -o $@ $(LDFLAGS)

$(BUILD)/hx421_collide_test$(EXE): $(TOOLS_DIR)/hx421_collide_test.c \
                                   $(RUNTIME_DIR)/hx421_collide.c $(RUNTIME_DIR)/hx421_scene3d.c \
                                   $(RUNTIME_DIR)/hx421_mask.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(RUNTIME_DIR) $^ -o $@ $(LDFLAGS)

$(BUILD)/hx421_mask_test$(EXE): $(TOOLS_DIR)/hx421_mask_test.c $(RUNTIME_DIR)/hx421_mask.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(RUNTIME_DIR) $^ -o $@ $(LDFLAGS)

# The dev-mode firmware<->game syscall boundary. Host-tested like the audio
# firmware modules; the ABI header's append-only _Static_asserts also compile
# here, so a reordered slot fails this build before any game is ever loaded.
FWDEV_DIR := ../firmware/dev
$(BUILD)/hx421_syscall_test$(EXE): $(TOOLS_DIR)/hx421_syscall_test.c $(FWDEV_DIR)/hx421_gamert.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(FWDEV_DIR) $^ -o $@ $(LDFLAGS)

# The dev-mode terminal: transport-agnostic logic (TX ring, RX line editor,
# ANSI). USB glue is a separate target-only shim; everything with logic is here.
$(BUILD)/hx421_term_test$(EXE): $(TOOLS_DIR)/hx421_term_test.c $(FWDEV_DIR)/hx421_term.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(FWDEV_DIR) $^ -o $@ $(LDFLAGS)

# The RAM loader's logic: header validation + placement (copy + bss zero) + all
# reject paths. The load-and-jump is proven under QEMU (real Thumb execution).
$(BUILD)/hx421_loader_test$(EXE): $(TOOLS_DIR)/hx421_loader_test.c $(FWDEV_DIR)/hx421_loader.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(FWDEV_DIR) $^ -o $@ $(LDFLAGS)

# The syscall-table builder's new logic: the game arena, the path sandbox, and
# the slot/handle wiring, against a mocked firmware backend.
$(BUILD)/hx421_sysimpl_test$(EXE): $(TOOLS_DIR)/hx421_sysimpl_test.c $(FWDEV_DIR)/hx421_sysimpl.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(FWDEV_DIR) $^ -o $@ $(LDFLAGS)

RUNTIME_TESTS := $(BUILD)/hx421_metatile_test$(EXE) $(BUILD)/hx421_raster_test$(EXE) \
                 $(BUILD)/hx421_scene3d_test$(EXE) $(BUILD)/hx421_collide_test$(EXE) \
                 $(BUILD)/hx421_mask_test$(EXE) $(BUILD)/hx421_syscall_test$(EXE) \
                 $(BUILD)/hx421_term_test$(EXE) $(BUILD)/hx421_loader_test$(EXE) \
                 $(BUILD)/hx421_sysimpl_test$(EXE)

.PHONY: runtimetest
runtimetest: $(RUNTIME_TESTS)
	@fail=0; \
	for t in $(RUNTIME_TESTS); do \
	  echo "==== $$t ===="; \
	  "./$$t" || fail=1; \
	done; \
	if [ $$fail -eq 0 ]; then echo "ALL RUNTIME SUITES PASSED"; else echo "SOME RUNTIME SUITES FAILED"; fi; \
	exit $$fail

# ---- armcheck: tier 2 (docs/dev-mode.md) — cross-compile the dev-mode firmware
# for the REAL Cortex-M4 target, so the append-only ABI _Static_asserts evaluate
# at sizeof(void*)==4. The host suites only prove the 64-bit layout; this proves
# the layout the cart sees. Needs arm-none-eabi-gcc (put msys mingw64/bin on PATH
# or cc1 fails silently).
ARM_CC    ?= arm-none-eabi-gcc
ARM_FLAGS := -c -Os -Wall -Wextra -std=c11 -mcpu=cortex-m4 -mthumb -ffreestanding -I$(FWDEV_DIR)
FWDEV_SRC := $(FWDEV_DIR)/hx421_gamert.c $(FWDEV_DIR)/hx421_term.c \
             $(FWDEV_DIR)/hx421_loader.c $(FWDEV_DIR)/hx421_sysimpl.c

.PHONY: armcheck
armcheck:
	@mkdir -p $(BUILD)
	@for s in $(FWDEV_SRC); do \
	  echo "arm: $$s"; \
	  $(ARM_CC) $(ARM_FLAGS) $$s -o $(BUILD)/`basename $$s .c`.arm.o || exit 1; \
	done
	@echo "ARM CROSS-COMPILE OK — target 32-bit ABI validated"

clean:
	rm -rf $(BUILD)
