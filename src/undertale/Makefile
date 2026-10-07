# undertale — openfpgaOS custom core
#
# Butterscotch (open-source GameMaker: Studio runner) built against the
# openfpgaOS SDK, with its software renderer drawing into the 320x240
# RGB555 framebuffer. You supply your own Undertale data.win.
#
# Targets:
#   make              Build the custom core
#   make debug        Build, push via UART, stream console
#   make copy         Copy this custom core to Pocket SD
#   make package      Package this custom core into a ZIP
#   make test         Test on desktop (SDL2) -> ./undertale_pc
#   make clean        Remove build artifacts
#

APP       = undertale
# Platform target: pocket (APF SD tree under build/pocket/<app>/) or
# mister (disk-image bundle under build/mister/<app>/). Overridable per
# invocation: `make TARGET=mister`, or from the SDK root Makefile.
TARGET   ?= pocket
# MiSTer network target for `make copy TARGET=mister` (override: MISTER_IP=...).
MISTER_IP ?= mister.local
SDK_DIR   = ../sdk
ROOT      = $(realpath $(CURDIR)/../..)
OBJ_DIR   = $(ROOT)/.obj/$(APP)
BUILD_DIR = $(OBJ_DIR)
DIST      = $(ROOT)/dist/$(APP)
OUT       = $(ROOT)/build/$(TARGET)/$(APP)
RUNTIME   = $(ROOT)/runtime

# ── Butterscotch sources ─────────────────────────────────────────────
BS        = butterscotch
BS_SRC    = $(BS)/src
BS_VENDOR = $(BS)/vendor

BS_CORE_SRCS   = $(wildcard $(BS_SRC)/*.c)
BS_SW_SRCS     = $(wildcard $(BS_SRC)/sw/*.c)
BS_IMAGE_SRCS  = $(wildcard $(BS_SRC)/image/*.c)
BS_FONT_SRCS   = $(wildcard $(BS_SRC)/debug_font/*.c)
BS_VIDEO_SRCS  = $(BS_SRC)/video/null_video.c
BS_STB_SRCS    = $(BS_SRC)/embedded/stb_impl.c
BS_VENDOR_SRCS = $(wildcard $(BS_VENDOR)/bzip2/*.c) \
                 $(BS_VENDOR)/sha1/sha1.c $(BS_VENDOR)/md5/md5.c \
                 $(BS_VENDOR)/base64/base64.c

SRCS = main.c platform/of_platform.c platform/of_diag.c platform/of_log.c \
       platform/of_perf.c platform/of_audio_system.c platform/ut_bench.c \
       platform/ut_strings.c platform/ut_save_fs.c \
       $(BS_CORE_SRCS) $(BS_SW_SRCS) $(BS_IMAGE_SRCS) $(BS_FONT_SRCS) \
       $(BS_VIDEO_SRCS) $(BS_STB_SRCS) $(BS_VENDOR_SRCS)
SRCS_CXX =

BS_INCLUDES = -Iplatform -I$(BS_SRC) -I$(BS_SRC)/sw -I$(BS_SRC)/image -I$(BS_SRC)/video \
              -I$(BS_SRC)/debug_font \
              -I$(BS_VENDOR)/stb/ds -I$(BS_VENDOR)/stb/image -I$(BS_VENDOR)/stb/vorbis \
              -I$(BS_VENDOR)/bzip2 -I$(BS_VENDOR)/sha1 -I$(BS_VENDOR)/md5 \
              -I$(BS_VENDOR)/base64

# Undertale v1.08 is WAD 16; building only that version shrinks the
# interpreter hot path. The CPU's FPU is single-precision, so GML reals are
# floats (same trade-off the PS2 port makes).
# Override to test other games on desktop, e.g. `make test WADS="14 16"`.
WADS ?= 16
BS_DEFS = $(foreach w,$(WADS),-DENABLE_WAD$(w)) -DENABLE_SW_RENDERER -DPIXEL_SIZE=16 \
          -DUSE_FLOAT_REALS -DNO_RVALUE_INT64 -DSW_DRAW_PROFILE \
          -DBUTTERSCOTCH_VIDEO_NULL -DUSE_PLATFORM_AUDIO -DUSE_PLATFORM_FILE_SYSTEM -DSW_PLATFORM_FRAMEBUFFER -DDATAWIN_LOG_CHUNKS \
          -DBUTTERSCOTCH_COMMIT_HASH='"openfpga"' -DBUTTERSCOTCH_COMMIT_DATE='"unknown"'

# Decoded texture pages are cached up to TEXTURE_CACHE_MB, but on the device
# the real limit is the heap (about 51 MB for everything): after each load
# the cache drops old pages until TEXTURE_RESERVE_MB can still be allocated.
TEXTURE_CACHE_MB ?= 40
TEXTURE_RESERVE_MB ?= 4
BS_DEFS += -DTEXTURE_CACHE_BYTES='($(TEXTURE_CACHE_MB)u*1024u*1024u)' \
           -DTEXTURE_CACHE_RESERVE_BYTES='($(TEXTURE_RESERVE_MB)u*1024u*1024u)'

ifeq ($(TRACE),1)
BS_DEFS += -DUT_TRACE_FRAMES
endif

BS_WARN = -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
          -Wno-sign-compare -Wno-missing-field-initializers

CFLAGS = $(BS_DEFS) $(BS_INCLUDES) $(BS_WARN)

# Data files bundled into the per-target deliverable (override per core).
# data.win is your own copy of the game data (gitignored); when present it is
# placed next to the ELF in the SD tree, together with the texture pack
# generated from it.
#
# music/ is a directory (or symlink) holding the game's .ogg files; when
# present they are converted into music.bin.
GAME_PACKS = $(if $(wildcard data.win),textures.bin) $(if $(wildcard music/*.ogg),music.bin)
MISTER_ASSETS ?= $(wildcard data.win) $(GAME_PACKS)

# Declare default target before sdk.mk's rules
.DEFAULT_GOAL := all

include $(SDK_DIR)/sdk.mk

# Host tool: converts data.win's texture pages to the renderer's 16-bit
# format so the device does not have to decode PNGs.
MKTEXPACK = $(ROOT)/.obj/$(APP)-tools/mktexpack
MKTEXPACK_SRCS = tools/mktexpack.c platform/of_log.c $(BS_SRC)/log.c $(BS_SRC)/gettime.c \
                 $(BS_IMAGE_SRCS) $(BS_STB_SRCS) $(wildcard $(BS_VENDOR)/bzip2/*.c)

$(MKTEXPACK): $(MKTEXPACK_SRCS)
	@mkdir -p $(dir $@)
	$(PC_CC) -O2 -DUT_HOST_TOOL $(BS_DEFS) $(BS_INCLUDES) $(BS_WARN) $(MKTEXPACK_SRCS) -lm -o $@

textures.bin: data.win $(MKTEXPACK)
	$(MKTEXPACK) data.win $@

# Host tool: converts the game's .ogg music and the sounds embedded in
# data.win to mono ADPCM so the device does not have to decode Vorbis.
MKMUSIC = $(ROOT)/.obj/$(APP)-tools/mkmusic
MUSIC_RATE ?= 32000

$(MKMUSIC): tools/mkmusic.c platform/ut_music_pack.h
	@mkdir -p $(dir $@)
	$(PC_CC) -O2 -w tools/mkmusic.c $(BS_VENDOR)/stb/vorbis/stb_vorbis.c -lm -o $@

music.bin: $(MKMUSIC) data.win $(wildcard music/*.ogg)
	$(MKMUSIC) data.win music $@ $(MUSIC_RATE)

# Host tool: packs a desktop save folder into the save slot file and back.
#   make import-save SAVE_DIR=<desktop save folder>   -> undertale_0.sav
#   make export-save EXPORT_DIR=<folder to write>     <- undertale_0.sav
# The slot file belongs at Saves/undertale/common/ on the SD card. It is not
# part of the SD tree, so copying a build never replaces progress on the card.
MKSAVE = $(ROOT)/.obj/$(APP)-tools/mksave
SAVE_DIR ?= $(HOME)/Library/Application Support/com.tobyfox.undertale
SAVE_FILE ?= undertale_0.sav

$(MKSAVE): tools/mksave.c platform/ut_save_format.h
	@mkdir -p $(dir $@)
	$(PC_CC) -O2 -Wall -Wextra tools/mksave.c -o $@

.PHONY: import-save export-save
import-save: $(MKSAVE)
	$(MKSAVE) pack "$(SAVE_DIR)" $(SAVE_FILE)

export-save: $(MKSAVE)
	@[ -n "$(EXPORT_DIR)" ] || { echo "export-save: give EXPORT_DIR=<folder to write the save files into>"; exit 1; }
	$(MKSAVE) unpack $(SAVE_FILE) "$(EXPORT_DIR)"

# Build ELF, then assemble SD card image in build/<app>/
all: $(OBJ_DIR)/app.elf $(GAME_PACKS) release
	@$(SIZE) $<

# Assemble the per-target deliverable.  Each platform owns its layout
# (pocket = APF tree, mister = disk image); the rule names no target —
# adding a target = add platforms/<target>/image.sh.
release: $(OBJ_DIR)/app.elf $(GAME_PACKS)
	@$(SDK_DIR)/platforms/$(TARGET)/image.sh "$(APP)" "$(OBJ_DIR)/app.elf" "$(ROOT)" "$(DIST)" $(MISTER_ASSETS)

debug: all
	@$(ROOT)/scripts/debug.sh $$(ls -d $(OUT)/Assets/*/common 2>/dev/null)/$(APP).elf

# Deploy to the device.  MISTER_IP rides the environment so the same
# invocation serves every platform's copy.sh (pocket ignores it and
# auto-detects the SD card; mister uses it as the network host).
# On macOS the Pocket copy goes through tools/sdcopy.sh, which also works over
# SSH on a headless machine (waits for the card, mounts, verifies, ejects).
ifeq ($(shell uname -s)-$(TARGET),Darwin-pocket)
COPY_CMD = tools/sdcopy.sh "$(OUT)"
else
COPY_CMD = MISTER_IP="$(MISTER_IP)" $(SDK_DIR)/platforms/$(TARGET)/copy.sh "$(APP)" "$(OBJ_DIR)/app.elf"
endif

copy: all
	@$(COPY_CMD)

# Side-by-side comparison of OS versions and CPU variants on one SD card.
# SDK09 is a checkout carrying the v0.9 SDK and Pocket runtime (the Diablo
# port's tree works). Adds Benchmark instances and two v0.9 cores next to
# this one; see tools/mkcompare.sh.
SDK09   ?= $(ROOT)/../Diablo
OBJ09    = $(ROOT)/.obj/$(APP)-v09

compare: all
	$(MAKE) SDK_DIR=$(SDK09)/src/sdk OBJ_DIR=$(OBJ09) BUILD_DIR=$(OBJ09) $(OBJ09)/app.elf
	@tools/mkcompare.sh "$(OUT)" "$(SDK09)/runtime/pocket" "$(OBJ09)/app.elf"

# Like `copy`, but without re-running the assembly step, which would remove
# the comparison cores again.
compare-copy: compare
	@$(COPY_CMD)

package: all
	@$(ROOT)/scripts/package.sh $(APP)

# Desktop build. sdk.mk's app_pc rule assumes a Linux host (of_init.c needs
# <sys/auxv.h>, and sdl2-config omits the SDL2 parent include dir on
# Homebrew), so this core builds its own per-object desktop binary that
# works on macOS too. of_init.c only fetches the kernel's caps/services
# tables, which the PC shim does not use.
PC_OBJ_DIR = $(ROOT)/.obj/$(APP)-pc
PC_SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags)
PC_SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs)
PC_SRCS = $(SRCS) $(SDK_DIR)/pc/of_sdl2.c
PC_OBJS = $(patsubst %.c,$(PC_OBJ_DIR)/%.o,$(subst ../,up/,$(PC_SRCS)))

$(PC_OBJ_DIR)/up/%.o: ../%.c
	@mkdir -p $(dir $@)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) -c -o $@ $<

$(PC_OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) -c -o $@ $<

undertale_pc: $(PC_OBJS)
	$(PC_CC) $(PC_OBJS) $(PC_SDL_LIBS) -lm -o $@

test: undertale_pc $(GAME_PACKS)

clean: sdk-clean
	rm -rf $(PC_OBJ_DIR) undertale_pc
	rm -rf $(ROOT)/build/pocket/$(APP) $(ROOT)/build/mister/$(APP)

.PHONY: all release debug copy compare compare-copy package test clean
