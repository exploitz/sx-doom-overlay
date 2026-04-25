# =============================================================================
# Makefile for sx-doom-overlay
#
# Builds the .ovl binary that loads Doom inside Ultrahand. doomgeneric is
# included from lib/doomgeneric (submodule) with the patches/ overlay applied
# at build time. The libtesla / libultra layer comes from lib/libultrahand.
#
# Targets:
#   make           - build out/sx-doom-overlay.ovl
#   make patches   - apply all patches/*.patch to lib/doomgeneric (idempotent)
#   make clean     - remove build/ and out/
#   make dist      - assemble dist/sx-doom-overlay-<version>.zip
#
# Licensed under GPLv2.
# =============================================================================

.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Install devkitPro and `export DEVKITPRO=/opt/devkitpro`. See README.md "Build prerequisites".)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

# -----------------------------------------------------------------------------
# Project metadata
# -----------------------------------------------------------------------------
APP_TITLE   := Doom Overlay
APP_AUTHOR  := chase
APP_VERSION := 0.0.1-bootstrap
TARGET      := sx-doom-overlay
BUILD       := build
SOURCES     := source
INCLUDES    := source include
NO_ICON     := 1

# Pull in the libultrahand build rules. This sets up libtesla, libultra,
# and a chain of include/lib paths that we depend on.
include $(TOPDIR)/lib/libultrahand/ultrahand.mk

# -----------------------------------------------------------------------------
# Compiler / linker flags
# -----------------------------------------------------------------------------
ARCH    := -march=armv8-a+simd+crc+cryp -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS  := -g -Wall -Wextra -Werror -O2 -ffunction-sections \
           $(ARCH) $(DEFINES)
CFLAGS  += $(INCLUDE) -D__SWITCH__
# Project-wide defines:
#   CMAP256                   - doomgeneric writes 8-bit indexed pixels
#   DOOMGENERIC_RESX/RESY     - 320x200 Doom internal resolution
#   USE_EXCEPTION_WRAP        - libultrahand's exception-stub mechanism (overlays
#                               are built with -fno-exceptions for size; this
#                               wraps any STL throw paths into trap codes)
CFLAGS  += -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
           -DUSE_EXCEPTION_WRAP=1

CXXFLAGS:= $(CFLAGS) -fno-rtti -fno-exceptions -std=c++26 -flto=6
ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
           -Wl,-Map,$(notdir $*.map) \
           -Wl,-wrap,__cxa_throw

LIBS    := -lcurl -lz -lmbedtls -lmbedx509 -lmbedcrypto -lnx

# libultrahand's ultrahand.mk already extends LIBDIRS / INCLUDE; we add libnx.
LIBDIRS := $(PORTLIBS) $(LIBNX) $(LIBDIRS)

export OUTPUT := $(CURDIR)/out/$(TARGET)

# -----------------------------------------------------------------------------
# Patch handling — MUST fail loud, never silent.
# -----------------------------------------------------------------------------
PATCH_SENTINEL := lib/doomgeneric/.patched
PATCH_FILES    := $(sort $(wildcard patches/*.patch))

.PHONY: patches
patches: $(PATCH_SENTINEL)

$(PATCH_SENTINEL): $(PATCH_FILES) | lib/doomgeneric
	@if [ -z "$(PATCH_FILES)" ]; then \
		echo "[patches] no patches in patches/ — skipping"; \
	else \
		set -e; \
		cd lib/doomgeneric && \
		for p in $(addprefix ../../,$(PATCH_FILES)); do \
			echo "[patches] applying $$p"; \
			git apply --check "$$p" || { \
				echo "ERROR: patch $$p does not apply cleanly."; \
				echo "       Re-roll the patch against the current submodule HEAD before continuing."; \
				exit 1; \
			}; \
			git apply "$$p" || { \
				echo "ERROR: git apply failed for $$p (check passed but apply failed — this should not happen)"; \
				exit 1; \
			}; \
		done; \
	fi
	@touch $(PATCH_SENTINEL)

# -----------------------------------------------------------------------------
# Build flow
# -----------------------------------------------------------------------------
.PHONY: all clean dist

all: $(BUILD) $(PATCH_SENTINEL)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD) $(OUTPUT).ovl $(OUTPUT).elf $(OUTPUT).nacp $(PATCH_SENTINEL)
	@echo "[clean] removed build artifacts (patch sentinel cleared)"

dist: all
	@scripts/dist.sh "$(APP_VERSION)"

# Standard devkitPro magic happens via switch_rules + ultrahand.mk above.
