# =============================================================================
# Makefile for sx-doom-overlay
#
# Two-pass devkitPro homebrew structure (standard pattern):
#   - First pass (from project root): set up vars, ensure build-<platform>
#     exists, then recurse into build-<platform>/ with this Makefile.
#   - Second pass (from inside build-<platform>/): the actual compile rules
#     driven by libnx's switch_rules.
#
# build-<platform>/ and out-<platform>/ are auto-namespaced per host so
# the same checkout can be built from WSL ('linux') and PowerShell ('win')
# without the .d cache from one breaking the other.
#
# Targets (top level):
#   make           build out-<platform>/sx-doom-overlay.ovl
#   make patches   apply patches/*.patch to lib/doomgeneric (idempotent)
#   make clean     remove ALL build-* and out-* dirs
#   make dist      assemble dist/sx-doom-overlay-<version>.zip
#
# Licensed under GPLv2.
# =============================================================================

.SUFFIXES:
.DEFAULT_GOAL := all

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Install devkitPro for your OS and re-run: \
  Linux/WSL → `sudo ./scripts/install-devkitpro.sh` \
  Windows native → run the official installer from https://github.com/devkitPro/installer/releases, then build from the "devkitPro MSys2" shell. \
  See README.md "Build from source" for the full cross-platform guide.)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

# -----------------------------------------------------------------------------
# Project metadata (used by switch_rules for .nacp generation)
# -----------------------------------------------------------------------------
APP_AUTHOR  := chase
APP_VERSION := 0.1.0
GIT_BRANCH  := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
GIT_HASH    := $(shell git rev-parse --short HEAD 2>/dev/null || echo nogit)
GIT_DIRTY   := $(shell git diff --quiet 2>/dev/null || echo +)
BUILD_ID    := $(GIT_BRANCH)@$(GIT_HASH)$(GIT_DIRTY)
# Title shown in the libtesla overlay picker. Includes the branch so non-main
# builds (OGG, feature branches, etc) are visually distinct in the launcher.
ifeq ($(GIT_BRANCH),main)
APP_TITLE   := Doom Overlay
else
APP_TITLE   := Doom ($(GIT_BRANCH))
endif
TARGET      := sx-doom-overlay

# Platform-namespaced build + output dirs. The intermediate .d files emitted
# by gcc contain absolute source paths (/mnt/c/... under WSL, C:/... under
# native Windows MSys2), so a build/ dir produced from one shell breaks
# 'make' from another shell against the same checkout. Namespacing both
# dirs lets contributors who alternate between WSL and native Windows
# keep two valid build caches side by side without 'make clean' between
# every platform switch.
UNAME_S := $(shell uname -s 2>/dev/null)
ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
PLATFORM := win
else ifeq ($(UNAME_S),Darwin)
PLATFORM := mac
else ifeq ($(UNAME_S),Linux)
PLATFORM := linux
else
PLATFORM := unknown
endif

BUILD       := build-$(PLATFORM)
OUT         := out-$(PLATFORM)
SOURCES     := source source/opl
INCLUDES    := source source/opl include
NO_ICON     := 1

# Pull in the libultrahand build rules. This appends to SOURCES and INCLUDES
# so libtesla + libultra get compiled into our overlay.
include $(TOPDIR)/lib/libultrahand/ultrahand.mk

# -----------------------------------------------------------------------------
# Patch handling — must run BEFORE we let the build try to read i_system.c.
# Delegated to scripts/apply-patches.sh (idempotent, fail-loud).
# -----------------------------------------------------------------------------
PATCH_SENTINEL := lib/doomgeneric/.patched
PATCH_FILES    := $(sort $(wildcard patches/*.patch))

.PHONY: patches
patches: $(PATCH_SENTINEL)

$(PATCH_SENTINEL): $(PATCH_FILES) scripts/apply-patches.sh | lib/doomgeneric
	@bash scripts/apply-patches.sh

# -----------------------------------------------------------------------------
# Compiler / linker flags
# -----------------------------------------------------------------------------
ARCH    := -march=armv8-a+simd+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  := -g -Wall -O2 -ffunction-sections \
           $(ARCH) $(DEFINES)
CFLAGS  += $(INCLUDE) -D__SWITCH__
# Project-wide defines — see plan §"Foundation: Engine + Framework"
#   CMAP256                  : doomgeneric writes 8-bit indexed pixels
#   DOOMGENERIC_RESX/RESY    : 320x200 internal Doom resolution
#   USE_EXCEPTION_WRAP       : libultrahand's no-throw exception stub mechanism
#   NORMALUNIX, LINUX        : doomgeneric needs these for Linux-style build paths
#   SWITCH_SOUND             : enables our DG_sound_module / DG_music_module in
#                              i_sound.c without dragging FEATURE_SOUND's
#                              SDL_mixer.h include (see patches/0008)
#   USING_WIDGET_DIRECTIVE   : libtesla draws the user's Ultrahand-configured
#                              sysmon widget (clock / battery / CPU stats) in
#                              the top-right of our overlay. Without this,
#                              opening our Doom overlay makes the widget
#                              vanish (visual inconsistency with every other
#                              libtesla overlay). With it, our overlay matches
#                              the rest of the user's overlay collection.
CFLAGS  += -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
           -DUSE_EXCEPTION_WRAP=1 -DNORMALUNIX -DLINUX \
           -DSWITCH_SOUND -DUSING_WIDGET_DIRECTIVE \
           -DAPP_VERSION=\"$(APP_VERSION)\" -DBUILD_ID=\"$(BUILD_ID)\"

# Doomgeneric warnings we accept (pre-existing in upstream, not regressions
# we want to fail the build on)
CFLAGS  += -Wno-pointer-sign -Wno-unused-but-set-variable \
           -Wno-stringop-truncation -Wno-format-truncation \
           -Wno-implicit-fallthrough

CXXFLAGS:= $(CFLAGS) -fno-rtti -fno-exceptions -std=c++26
ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
           -Wl,-Map,$(notdir $*.map) \
           -Wl,-wrap,__cxa_throw

LIBS    := -lcurl -lz -lmbedtls -lmbedx509 -lmbedcrypto -lnx

# libultrahand's ultrahand.mk already extends LIBDIRS / INCLUDE; we add libnx.
LIBDIRS := $(PORTLIBS) $(LIBNX) $(LIBDIRS)

# -----------------------------------------------------------------------------
# Two-pass build: top vs inside build/
# -----------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

# === Top-level pass ===

# devkitPro's base_tools sets CC and CXX but NOT LD — every project Makefile
# has to declare its own. Use the C++ compiler as the linker driver so the
# `-march=` / `-mtp=soft` flags are translated correctly into the right linker
# emulation (otherwise system /usr/bin/ld gets invoked and chokes on `tp=soft`).
export LD := $(CXX)

export OUTPUT  := $(CURDIR)/$(OUT)/$(TARGET)
export TOPDIR  := $(CURDIR)

# Doomgeneric source set — exclude platform shims and SDL/Allegro audio.
# We provide our own platform shim; libultrahand provides audio plumbing.
DG_DIR := lib/doomgeneric/doomgeneric
DG_EXCLUDE := \
    doomgeneric_allegro.c doomgeneric_emscripten.c doomgeneric_linuxvt.c \
    doomgeneric_sdl.c doomgeneric_soso.c doomgeneric_sosox.c \
    doomgeneric_win.c doomgeneric_xlib.c \
    i_allegrosound.c i_allegromusic.c i_sdlsound.c i_sdlmusic.c \
    gusconf.c icon.c \
    memio.c
# mus2mid.c is INCLUDED — used by source/opl/i_oplmusic.c to convert MUS
# lumps to MIDI for the OPL2 music player. Was previously excluded when
# music was disabled.
# memio.c is EXCLUDED — replaced by source/opl/memio_malloc.c which uses
# plain malloc instead of Z_Malloc, so MIDI loading doesn't fragment
# Doom's zone allocator (was crashing libtesla's HID thread on level
# transitions when the zone got tight).
DG_CFILES := $(filter-out $(DG_EXCLUDE),$(notdir $(wildcard $(DG_DIR)/*.c)))

# Append doomgeneric to source set BEFORE we expand VPATH and the file lists,
# so the build can actually find am_map.c et al.
SOURCES  += $(DG_DIR)
INCLUDES += $(DG_DIR)

# VPATH must include EVERY directory the build searches for sources.
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                  $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

# Find every .c, .cpp, .s in SOURCES (now includes lib/doomgeneric)
CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

# Filter out the doomgeneric exclusions (platform shims etc.) from the auto-discovered list
CFILES   := $(filter-out $(DG_EXCLUDE),$(CFILES))

# Set up final include flag list
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export OFILES := $(addsuffix .o,$(SFILES)) $(CFILES:.c=.o) $(CPPFILES:.cpp=.o)
export HFILES := $(filter-out $(BUILD)/, $(wildcard $(BUILD)/$(notdir *.h)))

.PHONY: all clean dist $(BUILD)

all: $(BUILD)

$(BUILD): $(PATCH_SENTINEL)
	@[ -d $@ ] || mkdir -p $@
	@[ -d $(OUT) ] || mkdir -p $(OUT)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# clean wipes ALL platform variants, not just the current one — keeps things
# tidy if you previously built on a different platform against this checkout.
clean:
	@rm -rf build build-* out out-* $(PATCH_SENTINEL)
	@echo "[clean] removed build artifacts (patch sentinel cleared)"

dist: all
	@bash scripts/dist.sh "$(APP_VERSION)"

else

# === Inside-build/ pass — actual compile rules driven by libnx switch_rules ===

DEPENDS := $(OFILES:.o=.d)

# Inside-build default: produce the .ovl
all: $(OUTPUT).ovl

$(OUTPUT).ovl: $(OUTPUT).nro
	@cp $< $@
	@echo "[ovl] $(notdir $@)"

# Override switch_rules' default %.nro recipe so elf2nro embeds the .nacp.
# Without --nacp=, the resulting .nro has no title metadata and Ultrahand
# silently skips it from the overlay menu (no error, just doesn't appear).
# Both the .elf and .nacp must be built first, hence the explicit deps.
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
	@elf2nro $< $@ --nacp=$(OUTPUT).nacp $(NROFLAGS)
	@echo built ... $(notdir $@)

$(OUTPUT).elf: $(OFILES)

# Default rules supplied by switch_rules + base_rules cover .o, .elf, .nro,
# .nacp generation. The .ovl rule above is the libtesla/libultrahand
# convention for overlays — same bytes as a .nro, just a different extension
# so nx-ovlloader picks it up from /switch/.overlays/.

-include $(DEPENDS)

endif
