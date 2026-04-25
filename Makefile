# =============================================================================
# Makefile for sx-doom-overlay
#
# Two-pass devkitPro homebrew structure (standard pattern):
#   - First pass (from project root): set up vars, ensure build dir, recurse
#     into build/ with itself as the Makefile.
#   - Second pass (from inside build/): the actual compile rules driven by
#     libnx's switch_rules.
#
# Targets (top level):
#   make           build out/sx-doom-overlay.ovl
#   make patches   apply patches/*.patch to lib/doomgeneric (idempotent)
#   make clean     remove build/ and out/
#   make dist      assemble dist/sx-doom-overlay-<version>.zip
#
# Licensed under GPLv2.
# =============================================================================

.SUFFIXES:
.DEFAULT_GOAL := all

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Install devkitPro and `export DEVKITPRO=/opt/devkitpro`. See README.md "Build prerequisites".)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

# -----------------------------------------------------------------------------
# Project metadata (used by switch_rules for .nacp generation)
# -----------------------------------------------------------------------------
APP_TITLE   := Doom Overlay
APP_AUTHOR  := chase
APP_VERSION := 0.0.1-bootstrap
TARGET      := sx-doom-overlay
BUILD       := build
SOURCES     := source
INCLUDES    := source include
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
CFLAGS  += -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
           -DUSE_EXCEPTION_WRAP=1 -DNORMALUNIX -DLINUX

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

export OUTPUT  := $(CURDIR)/out/$(TARGET)
export TOPDIR  := $(CURDIR)

# Doomgeneric source set — exclude platform shims and SDL/Allegro audio.
# We provide our own platform shim; libultrahand provides audio plumbing.
DG_DIR := lib/doomgeneric/doomgeneric
DG_EXCLUDE := \
    doomgeneric_allegro.c doomgeneric_emscripten.c doomgeneric_linuxvt.c \
    doomgeneric_sdl.c doomgeneric_soso.c doomgeneric_sosox.c \
    doomgeneric_win.c doomgeneric_xlib.c \
    i_allegrosound.c i_allegromusic.c i_sdlsound.c i_sdlmusic.c \
    mus2mid.c gusconf.c icon.c
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
	@[ -d out ] || mkdir -p out
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -rf $(BUILD) out $(PATCH_SENTINEL)
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
