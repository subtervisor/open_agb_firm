#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

APP_NAME        := open_agb_firm
APP_C2D         := 1
AGGRESSIVE_SIZE := 1

APP_DIR   := $(abspath $(CURDIR)/..)
CITRINE3D := $(APP_DIR)/libraries/citrine3d
LIBN3DS   := $(APP_DIR)/libraries/libn3ds

# Source roots in addition to $(APP_DIR)/source (which app.mk auto-includes
# and which holds shared C files like oaf_error_codes.c).
APP_EXTRA_SOURCES := \
	$(APP_DIR)/source/arm11 \
	$(APP_DIR)/source/arm11/ui

APP_EXTRA_INCLUDES := \
	$(APP_DIR)/include \
	$(APP_DIR)/libraries \
	$(APP_DIR)/libraries/imgui

APP_EXTRA_DEFINES := \
	-fno-unwind-tables \
	-fno-asynchronous-unwind-tables \
	-fdata-sections \
	-fstack-usage \
	-Wframe-larger-than=512 \
	-DLIBN3DS_LEGACY=1 \
	-DVERS_STRING=\"$(VERS_STRING)\" \
	-DVERS_MAJOR=$(VERS_MAJOR) \
	-DVERS_MINOR=$(VERS_MINOR) \
	'-DIM_ASSERT(_EXPR)=((void)(_EXPR))' \
	-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS=1 \
	-DIMGUI_DISABLE_DEBUG_TOOLS=1 \
	-DIMGUI_DISABLE_DEFAULT_FONT=1 \
	-DIMGUI_DISABLE_FILE_FUNCTIONS=1 \
	-DIMGUI_DISABLE_DEFAULT_FORMAT_FUNCTIONS=1 \
	-DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS=1 \
	-DIMGUI_DISABLE_DEFAULT_ALLOCATORS=1 \
	-DIMGUI_DISABLE_STB_TRUETYPE_IMPLEMENTATION=1 \
	-DIMGUI_DISABLE_STB_RECT_PACK_IMPLEMENTATION=1 \
	-DIMGUI_DISABLE_DEMO_WINDOWS=1

ifneq ($(strip $(NO_DEBUG)),)
APP_EXTRA_DEFINES += -DNDEBUG
endif

APP_BUILD       := $(APP_DIR)/build
APP_ARM11_BUILD := $(APP_BUILD)/arm11
APP_ARM11_BIN   := $(APP_ARM11_BUILD)/$(APP_NAME)11.bin

NPROC := $(shell nproc 2>/dev/null || echo 1)

.PHONY: all libcitrine3d libcitrine2d arm11_app clean

all: $(APP_NAME)11.bin

# Build the citrine3d static lib.
libcitrine3d:
	@$(MAKE) -j$(NPROC) --no-print-directory -C $(CITRINE3D)/build/arm11 \
	    AGGRESSIVE_SIZE=$(AGGRESSIVE_SIZE) \
	    CITRINE3D=$(CITRINE3D) LIBN3DS=$(LIBN3DS)

# Build the citrine2d static lib (font / sprite helpers).
libcitrine2d:
	@$(MAKE) -j$(NPROC) --no-print-directory -C $(CITRINE3D)/citrine2d/build/arm11 \
	    AGGRESSIVE_SIZE=$(AGGRESSIVE_SIZE) \
	    CITRINE3D=$(CITRINE3D) LIBN3DS=$(LIBN3DS)

# Per-app ARM11 build via citrine3d's app.mk.
arm11_app: libcitrine3d libcitrine2d
	@mkdir -p $(APP_ARM11_BUILD)
	@$(MAKE) -j$(NPROC) --no-print-directory -C $(APP_ARM11_BUILD) \
	    -f $(CITRINE3D)/build/arm11/app.mk \
	    AGGRESSIVE_SIZE=$(AGGRESSIVE_SIZE) \
	    APP_DIR=$(APP_DIR) APP_NAME=$(APP_NAME) APP_C2D=$(APP_C2D) \
	    APP_EXTRA_SOURCES="$(APP_EXTRA_SOURCES)" \
	    APP_EXTRA_INCLUDES="$(APP_EXTRA_INCLUDES)" \
	    APP_EXTRA_DEFINES="$(APP_EXTRA_DEFINES)" \
	    CITRINE3D=$(CITRINE3D) LIBN3DS=$(LIBN3DS)

# The top-level Makefile expects arm11/open_agb_firm11.bin to exist.
$(APP_NAME)11.bin: arm11_app
	@cp $(APP_ARM11_BIN) $@

clean:
	@echo clean ...
	@rm -fr $(APP_BUILD) $(APP_NAME)11.bin $(APP_NAME)11.elf
