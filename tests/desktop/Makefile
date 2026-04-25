# =============================================================================
# tests/desktop/Makefile — desktop engine smoke for sx-doom-overlay (Task 2)
#
# Builds doomgeneric on Linux (no SDL, no GPU, no audio) using our stub
# platform shim. Verifies that:
#   - The MIN_RAM=3 patch (patches/0001-lower-min-ram.patch) is applied
#   - The CMAP256 + 320x200 build configuration produces correct frames
#   - Freedoom 1 boots and runs without zone allocator failures
#
# Usage:
#   make smoke           - build + run for 200 ticks, dump 20 PPM frames
#   make smoke-valgrind  - same, under valgrind --leak-check=full
#   make clean
#
# Requires: gcc, libc, libm. NO devkitPro. NO SDL.
#
# Source of truth: tests/desktop/stub_platform.c
# =============================================================================

ROOT := ../..
DG_DIR := $(ROOT)/lib/doomgeneric/doomgeneric
PATCH_SENTINEL := $(ROOT)/lib/doomgeneric/.patched
WAD := $(ROOT)/data/freedoom1.wad

OUT_DIR := out
SMOKE_BIN := $(OUT_DIR)/stub_platform

# All doomgeneric sources EXCEPT the ones not in Makefile.soso's SRC_DOOM
# (the canonical "no SDL, no audio" reference build). Excluded files are:
#   - doomgeneric_*.c     (platform shims; ours is stub_platform.c)
#                         except doomgeneric_soso.c which we DO want? No —
#                         we replace the platform shim entirely with our stub.
#   - i_allegrosound.c    (Allegro audio backend, requires libAllegro)
#   - i_allegromusic.c
#   - i_sdlsound.c        (SDL audio backend, requires libSDL)
#   - i_sdlmusic.c
#   - mus2mid.c           (used only by SDL music)
#   - gusconf.c           (Gravis Ultrasound config; not used in any platform)
#   - icon.c              (window icon — needed only by SDL/X11/Win backends)
#
# We keep i_sound.c + s_sound.c + sounds.c (the upper audio framework).
# Without FEATURE_SOUND defined, sound_modules[] is just NULL — engine runs
# silent, no DG_sound_module symbol is referenced.
DG_SOURCES := $(filter-out \
    $(DG_DIR)/doomgeneric_allegro.c \
    $(DG_DIR)/doomgeneric_emscripten.c \
    $(DG_DIR)/doomgeneric_linuxvt.c \
    $(DG_DIR)/doomgeneric_sdl.c \
    $(DG_DIR)/doomgeneric_soso.c \
    $(DG_DIR)/doomgeneric_sosox.c \
    $(DG_DIR)/doomgeneric_win.c \
    $(DG_DIR)/doomgeneric_xlib.c \
    $(DG_DIR)/i_allegrosound.c \
    $(DG_DIR)/i_allegromusic.c \
    $(DG_DIR)/i_sdlsound.c \
    $(DG_DIR)/i_sdlmusic.c \
    $(DG_DIR)/mus2mid.c \
    $(DG_DIR)/gusconf.c \
    $(DG_DIR)/icon.c \
    , $(wildcard $(DG_DIR)/*.c))

DG_OBJECTS := $(patsubst $(DG_DIR)/%.c,$(OUT_DIR)/dg_%.o,$(DG_SOURCES))
STUB_OBJ   := $(OUT_DIR)/stub_platform.o

# Match the production build flags as closely as possible.
CC      := gcc
CFLAGS  := -O2 -g -Wall -Wno-unused-variable -Wno-unused-but-set-variable \
           -Wno-implicit-fallthrough -Wno-format-truncation \
           -Wno-pointer-sign -Wno-stringop-truncation \
           -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
           -DNORMALUNIX -DLINUX \
           -I$(DG_DIR)
LDFLAGS := -lm

.PHONY: all smoke smoke-valgrind smoke-asan clean check-wad check-patch test_blit

all: $(SMOKE_BIN)

# --- test_blit (Task 3 unit tests) ------------------------------------------
TEST_BLIT_BIN := $(OUT_DIR)/test_blit
test_blit: $(TEST_BLIT_BIN)
	@echo "[test_blit] running unit tests..."
	@$(TEST_BLIT_BIN)
	@echo "[test_blit] OK"

$(TEST_BLIT_BIN): test_blit.cpp $(ROOT)/source/blit.cpp $(ROOT)/source/blit.hpp | $(OUT_DIR)
	@echo "[compile] $(TEST_BLIT_BIN)"
	@g++ -std=c++17 -O2 -g -Wall -Wextra -Werror \
		-fsanitize=address,undefined \
		-I$(ROOT)/source \
		test_blit.cpp $(ROOT)/source/blit.cpp \
		-o $(TEST_BLIT_BIN)

# --- test_sound (Task 4 unit tests) -----------------------------------------
TEST_SOUND_BIN := $(OUT_DIR)/test_sound
test_sound: $(TEST_SOUND_BIN)
	@echo "[test_sound] running unit tests..."
	@$(TEST_SOUND_BIN)
	@echo "[test_sound] OK — wav at out/test_sound_roundtrip.wav"

$(TEST_SOUND_BIN): test_sound.c audio_backend_wav.c \
                   $(ROOT)/source/audio_mixer.c $(ROOT)/source/audio_mixer.h \
                   $(ROOT)/source/audio_backend.h | $(OUT_DIR)
	@echo "[compile] $(TEST_SOUND_BIN)"
	@gcc -std=c11 -O2 -g -Wall -Wextra -Werror \
		-fsanitize=address,undefined \
		-I$(ROOT)/source \
		test_sound.c audio_backend_wav.c $(ROOT)/source/audio_mixer.c \
		-o $(TEST_SOUND_BIN)

# --- Patch application -------------------------------------------------------
# We delegate to scripts/apply-patches.sh (the canonical applier). Idempotent
# — if patches are already in effect, it's a fast no-op.
check-patch:
	@bash $(ROOT)/scripts/apply-patches.sh
	@grep -q "DEFAULT_RAM 3" $(DG_DIR)/i_system.c || { \
		echo "ERROR: patches/0001-lower-min-ram.patch did not produce expected output."; \
		echo "       Check $(DG_DIR)/i_system.c lines 57-58."; \
		exit 1; \
	}
	@echo "[check-patch] OK — DEFAULT_RAM/MIN_RAM patched to 3"

# --- WAD sanity check --------------------------------------------------------
check-wad:
	@if [ ! -f "$(WAD)" ]; then \
		echo "ERROR: $(WAD) not found."; \
		echo "       Download freedoom1.wad from https://freedoom.github.io/"; \
		echo "       and place it at $(WAD) before running smoke."; \
		exit 1; \
	fi
	@echo "[check-wad] OK — $$(stat -c%s $(WAD) 2>/dev/null || stat -f%z $(WAD)) bytes"

# --- Build -------------------------------------------------------------------
$(OUT_DIR):
	@mkdir -p $@

$(OUT_DIR)/dg_%.o: $(DG_DIR)/%.c | $(OUT_DIR) check-patch
	@$(CC) $(CFLAGS) -c $< -o $@

$(STUB_OBJ): stub_platform.c | $(OUT_DIR)
	@$(CC) $(CFLAGS) -c $< -o $@

$(SMOKE_BIN): $(STUB_OBJ) $(DG_OBJECTS)
	@echo "[link] $(SMOKE_BIN)"
	@$(CC) -o $@ $^ $(LDFLAGS)

# --- Smoke run ---------------------------------------------------------------
smoke: $(SMOKE_BIN) check-wad
	@echo "[smoke] running engine for 200 ticks, dumping every 10th frame..."
	@cd . && $(SMOKE_BIN) $(WAD) 200 10
	@echo "[smoke] frames produced:"
	@ls $(OUT_DIR)/frame_*.ppm 2>/dev/null | wc -l
	@echo "[smoke] OK"

smoke-valgrind: $(SMOKE_BIN) check-wad
	@if ! command -v valgrind >/dev/null 2>&1; then \
		echo "ERROR: valgrind not installed. Install: sudo apt install -y valgrind"; \
		echo "       Or use \`make smoke-asan\` (no sudo required)."; \
		exit 1; \
	fi
	@echo "[smoke-valgrind] running under valgrind..."
	@valgrind --leak-check=full --error-exitcode=1 --quiet \
		$(SMOKE_BIN) $(WAD) 60 30

# AddressSanitizer alternative — catches use-after-free, buffer overruns,
# stack overflows. No sudo, no extra install — built into gcc.
smoke-asan: check-patch check-wad
	@echo "[smoke-asan] rebuilding with -fsanitize=address..."
	@$(MAKE) clean
	@$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address -fsanitize=undefined" \
		LDFLAGS="$(LDFLAGS) -fsanitize=address -fsanitize=undefined"
	@echo "[smoke-asan] running engine for 60 ticks under ASan..."
	@$(SMOKE_BIN) $(WAD) 60 30
	@echo "[smoke-asan] OK — no ASan/UBSan errors detected"

clean:
	@rm -rf $(OUT_DIR)
	@echo "[clean] cleared $(OUT_DIR)"
