# Butterscotch for openfpgaOS (Analogue Pocket / MiSTer), packaged as one
# core with an entry per game (Undertale, Deltarune).
#
# Built against the openfpgaOS SDK, which is not part of this repository:
# point SDK_ROOT at a checkout of https://github.com/openfpgaOS/openfpgaSDK
# (default: one next to this repository). The software renderer draws into
# the SDK's RGB555 framebuffer. You supply your own game data (data.win).
#
# Everything the build writes goes under out/ here; the SDK checkout is only
# read. The SD card tree ends up in out/build/pocket/butterscotch/.
#
# Targets:
#   make              Build the custom core
#   make debug        Build, push via UART, stream console
#   make copy         Copy this custom core to Pocket SD
#   make package      Package this custom core into a ZIP
#   make test         Test on desktop (SDL2) -> ./<game>_pc
#   make clean        Remove build artifacts
#

# GAME picks which game's program is built. The core is one, with an entry
# per game on the Pocket; each game has its own program, folder of game
# data and save file:
#   undertale (default)  games/undertale/                                    WAD 16
#   deltarune            games/deltarune/ (see tools/deltarune-setup.sh)     WAD 17
# The SD tree is assembled from every game built so far (tools/mkcard.sh),
# so `make && make GAME=deltarune` gives a card with both.
GAME     ?= undertale
ifeq ($(GAME),undertale)
G         = games/undertale/
WADS     ?= 16
GAME_DEFS = -DGAME_UNDERTALE
# Desktop save folder for `make import-save` (macOS).
SAVE_DIR ?= $(HOME)/Library/Application Support/com.tobyfox.undertale
# Undertale's data lived beside this Makefile until 2026-10-13.
ifneq ($(wildcard data.win),)
ifeq ($(wildcard $(G)data.win),)
$(error Undertale's data now goes in $(G): move data.win, and music/, textures.bin and music.bin if you have them, from here into $(G))
endif
endif
else ifeq ($(GAME),deltarune)
G         = games/deltarune/
WADS     ?= 17
# Sounds of audio group 1 live in their own file, and the game reads its
# text from lang_en.json; both go into the audio pack.
MUSIC_EXTRAS = group:1:$(G)audiogroup1.dat file:$(G)lang_en.json
GAME_DEFS = -DGAME_DELTARUNE
else
$(error GAME=$(GAME) is not known; use undertale or deltarune)
endif
APP       = $(GAME)
# Platform target: pocket (APF SD tree under build/pocket/<app>/) or
# mister (disk-image bundle under build/mister/<app>/). Overridable per
# invocation: `make TARGET=mister`, or from the SDK root Makefile.
TARGET   ?= pocket
# MiSTer network target for `make copy TARGET=mister` (override: MISTER_IP=...).
MISTER_IP ?= mister.local
SDK_ROOT ?= $(abspath $(CURDIR)/../../../openfpgaSDK)
SDK_DIR   = $(SDK_ROOT)/src/sdk
ifeq ($(wildcard $(SDK_DIR)/sdk.mk),)
$(error No openfpgaOS SDK at $(SDK_ROOT). Clone https://github.com/openfpgaOS/openfpgaSDK there, or pass SDK_ROOT=<its path>)
endif
# The SDK's scripts take a root that holds runtime/ and receive build/; out/
# plays that part, with runtime/ linked to the SDK's.
ROOT      = $(CURDIR)/out
OBJ_DIR   = $(ROOT)/.obj/$(APP)
BUILD_DIR = $(OBJ_DIR)
DIST      = $(CURDIR)/dist
CARD      = butterscotch
OUT       = $(ROOT)/build/$(TARGET)/$(CARD)
RUNTIME   = $(SDK_ROOT)/runtime

# ── Butterscotch sources ─────────────────────────────────────────────
# Butterscotch is this repository. Absolute, so that object paths stay
# inside the object directory.
BS        = $(abspath $(CURDIR)/../..)
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
                 $(BS_VENDOR)/base64/base64.c \
                 $(BS_VENDOR)/miniz/miniz.c $(BS_VENDOR)/miniz/miniz_tdef.c $(BS_VENDOR)/miniz/miniz_tinfl.c \
                 $(BS_VENDOR)/zstd/zstd.c
# Box2D physics is left out (ENABLE_PHYSICS off): Undertale does not use it.
BS_PHYSICS_SRCS = $(BS_SRC)/physics/physics.c $(BS_SRC)/physics/disabled/physics_disabled.c

SRCS = main.c platform/game_$(GAME).c platform/of_platform.c platform/of_diag.c platform/of_log.c \
       platform/of_perf.c platform/of_audio_system.c platform/of_pack_read.c platform/of_bench.c \
       platform/of_strings.c platform/of_buttons.c platform/of_gamepad.c platform/of_save_fs.c platform/of_native.c platform/of_focus.c platform/of_focus_stats.c \
       platform/of_font.c platform/of_menu.c platform/of_state_requests.c \
       platform/of_snapshot_envelope.c \
       $(BS_CORE_SRCS) $(BS_SW_SRCS) $(BS_IMAGE_SRCS) $(BS_FONT_SRCS) \
       $(BS_VIDEO_SRCS) $(BS_STB_SRCS) $(BS_VENDOR_SRCS) $(BS_PHYSICS_SRCS)
SRCS_CXX =

BS_INCLUDES = -Iplatform -I$(BS_SRC) -I$(BS_SRC)/sw -I$(BS_SRC)/image -I$(BS_SRC)/video \
              -I$(BS_SRC)/debug_font \
              -I$(BS_VENDOR)/stb/ds -I$(BS_VENDOR)/stb/image -I$(BS_VENDOR)/stb/vorbis \
              -I$(BS_VENDOR)/bzip2 -I$(BS_VENDOR)/sha1 -I$(BS_VENDOR)/md5 \
              -I$(BS_VENDOR)/base64 -I$(BS_VENDOR)/miniz -I$(BS_VENDOR)/zstd

# Undertale v1.08 is WAD 16; building only that version shrinks the
# interpreter hot path. The CPU's FPU is single-precision, so GML reals are
# floats (same trade-off the PS2 port makes).
# Override to test other games on desktop, e.g. `make test WADS="14 16"`.
SAVE_SLOT_FILE = $(APP)_0.sav
LOG_SLOT_FILE = $(APP)_1.sav
ifeq ($(FOCUS_BENCH),1)
ifneq ($(GAME),deltarune)
$(error FOCUS_BENCH supports Deltarune Chapter 1 only)
endif
SAVE_SLOT_FILE = deltarune_tiles_0.sav
LOG_SLOT_FILE = deltarune_tiles_1.sav
FOCUS_BUILD_ID ?= unversioned
FOCUS_DEFS = -DOF_FOCUSED_BENCH -DOF_FOCUS_BUILD_ID='"$(FOCUS_BUILD_ID)"'
endif

BS_DEFS = $(FOCUS_DEFS) $(GAME_DEFS) -DOF_GAME_NAME='"$(APP)"' -DOF_SAVE_SLOT_FILE='"$(SAVE_SLOT_FILE)"' -DOF_LOG_SLOT_FILE='"$(LOG_SLOT_FILE)"' -DOF_FAST_LOOP $(foreach w,$(WADS),-DENABLE_WAD$(w)) -DENABLE_SW_RENDERER -DPIXEL_SIZE=16 \
          -DUSE_FLOAT_REALS -DNO_RVALUE_INT64 -DSW_DRAW_PROFILE -DPLATFORM_BUSY_TICK -DPLATFORM_PACK_READ -DSW_TEXTURE_PACK_HOST='"music.bin"' -DENABLE_VM_GML_PROFILER \
          -DBUTTERSCOTCH_VIDEO_NULL -DUSE_PLATFORM_AUDIO -DUSE_PLATFORM_FILE_SYSTEM -DSW_PLATFORM_FRAMEBUFFER -DSW_PLATFORM_BUTTON_GLYPHS -DDATAWIN_LOG_CHUNKS \
          -DBUTTERSCOTCH_COMMIT_HASH='"openfpga"' -DBUTTERSCOTCH_COMMIT_DATE='"unknown"' -DUSE_STB_IMGDEC

# Decoded texture pages are cached up to TEXTURE_CACHE_MB, but on the device
# the real limit is the heap (about 51 MB for everything): after each load
# the cache drops old pages until TEXTURE_RESERVE_MB can still be allocated.
TEXTURE_CACHE_MB ?= 40
TEXTURE_RESERVE_MB ?= 4
BS_DEFS += -DTEXTURE_CACHE_BYTES='($(TEXTURE_CACHE_MB)u*1024u*1024u)' \
           -DTEXTURE_CACHE_RESERVE_BYTES='($(TEXTURE_RESERVE_MB)u*1024u*1024u)'

ifeq ($(TRACE),1)
BS_DEFS += -DOF_TRACE_FRAMES
endif

ifeq ($(SAVESTATES),1)
ifneq ($(TARGET),pocket)
$(error Native save-state builds support Pocket only)
endif
BS_DEFS += -DOF_NATIVE_SAVESTATES
SDK_CONTAINER = $(CURDIR)/tools/native-sdk-container.sh
export SDK_ROOT
endif

BS_WARN = -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
          -Wno-sign-compare -Wno-missing-field-initializers

# OF_OPT: extra optimisation flags for the device build, after the SDK's -O2
# (the last -O wins), named in a benchmark report's title. Build with some
# into an OBJ_DIR of their own and hand mkcompare.sh "<name>:<app.elf>" to
# get a benchmark entry for it. Tried on the Pocket, 2026-10-09: -O3 and
# -O3 -flto change nothing; -Os is 16 to 22% slower; taking off the SDK's
# 64-byte alignment of functions and loops is 2 to 4% slower.
# Experiments ride the same way as defines. Tried so and dropped: a pool for
# script call frames in place of calloc and free (0.2 to 0.3 ms of a 46 ms
# undrawn battle frame, within run-to-run variation).
# UT_OPT is the name it had until 2026-10-13. rename-ut: keep
OF_OPT ?= $(UT_OPT)
CFLAGS = $(BS_DEFS) $(BS_INCLUDES) $(BS_WARN) $(OF_OPT) -DOF_BUILD_NOTE='"$(OF_OPT)"'
LDFLAGS += $(OF_OPT)

# Data files bundled into the per-target deliverable (override per core).
# data.win is your own copy of the game data (gitignored); when present it is
# placed next to the ELF in the SD tree, together with the texture pack
# generated from it.
#
# music/ is a directory (or symlink) holding the game's .ogg files; when
# present they are converted into music.bin.
GAME_PACKS = $(if $(wildcard $(G)data.win),$(G)textures.bin) $(if $(wildcard $(G)music/*.ogg),$(G)music.bin)
MISTER_ASSETS ?= $(wildcard $(G)data.win) $(GAME_PACKS)

# Neither sdk.mk's rules nor the desktop ones track headers, and objects built
# against an old struct layout link without complaint and then crash (the
# device build did, twice). So a change to any header empties the object
# directories this run builds into. Focus mode and build identity also change
# compiled code and save filenames, so they must invalidate the same objects.
# (Before sdk.mk, which looks at what is
# in them; PC_OBJ_DIR is only set this early when a sub-make passes it.)
HEADER_SUM := $(shell { cat $(BS_SRC)/*.h $(BS_SRC)/*/*.h platform/*.h 2>/dev/null; printf '%s' '$(SAVESTATES) $(GAME) $(WADS) $(FOCUS_BENCH) $(FOCUS_BUILD_ID)'; } | cksum | cut -d' ' -f1)
$(foreach d,$(OBJ_DIR) $(or $(PC_OBJ_DIR),$(ROOT)/.obj/$(APP)-pc),$(shell [ "$$(cat $(d)/.headers 2>/dev/null)" = "$(HEADER_SUM)" ] || { rm -rf $(d); mkdir -p $(d); echo $(HEADER_SUM) > $(d)/.headers; }))

# Declare default target before sdk.mk's rules
.DEFAULT_GOAL := all

include $(SDK_DIR)/sdk.mk

APP_SIZE = $(SIZE) $(OBJ_DIR)/app.elf
ifeq ($(SAVESTATES)-$(SDK_DELEGATE),1-1)
APP_SIZE = bash $(SDK_CONTAINER) make -f $(firstword $(MAKEFILE_LIST)) native-size
endif
native-size:
	@$(SIZE) $(OBJ_DIR)/app.elf

# Host tool: converts data.win's texture pages to the renderer's 16-bit
# format so the device does not have to decode PNGs.
MKTEXPACK = $(ROOT)/.obj/$(APP)-tools/mktexpack
MKTEXPACK_SRCS = tools/mktexpack.c platform/of_log.c $(BS_SRC)/log.c $(BS_SRC)/gettime.c \
                 $(BS_IMAGE_SRCS) $(BS_STB_SRCS) $(wildcard $(BS_VENDOR)/bzip2/*.c)

# The header sets the tile size, so a change there rebuilds the tool and the packs.
$(MKTEXPACK): $(MKTEXPACK_SRCS) $(BS_SRC)/sw/sw_texture_pack.h
	@mkdir -p $(dir $@)
	$(PC_CC) -O2 -DOF_HOST_TOOL $(BS_DEFS) $(BS_INCLUDES) $(BS_WARN) $(MKTEXPACK_SRCS) -lm -o $@

$(G)textures.bin: $(G)data.win $(MKTEXPACK)
	$(MKTEXPACK) $(G)data.win $@

# Host tool: converts the game's .ogg music and the sounds embedded in
# data.win to mono ADPCM so the device does not have to decode Vorbis.
MKMUSIC = $(ROOT)/.obj/$(APP)-tools/mkmusic
MUSIC_RATE ?= 32000

$(MKMUSIC): tools/mkmusic.c platform/of_music_pack.h
	@mkdir -p $(dir $@)
	$(PC_CC) -O2 -w tools/mkmusic.c $(BS_VENDOR)/stb/vorbis/stb_vorbis.c -lm -o $@

# The extra inputs are "group:<n>:<file>" and "file:<file>"; a change in one
# of those files rebuilds the pack too.
MUSIC_EXTRA_FILES = $(wildcard $(foreach e,$(MUSIC_EXTRAS),$(lastword $(subst :, ,$(e)))))

$(G)music.bin: $(MKMUSIC) $(G)data.win $(wildcard $(G)music/*.ogg) $(MUSIC_EXTRA_FILES)
	$(MKMUSIC) $(G)data.win $(G)music $@ $(MUSIC_RATE) $(MUSIC_EXTRAS)

# Host tool: packs a desktop save folder into the save slot file and back.
# It knows Undertale's save files only (file0..9, undertale.ini and the like).
#   make import-save SAVE_DIR=<desktop save folder>   -> <game>_0.sav
#   make export-save EXPORT_DIR=<folder to write>     <- <game>_0.sav
# The slot file belongs at Saves/butterscotch/common/ on the SD card. It is not
# part of the SD tree, so copying a build never replaces progress on the card.
MKSAVE = $(ROOT)/.obj/$(APP)-tools/mksave
SAVE_FILE ?= $(APP)_0.sav

$(MKSAVE): tools/mksave.c platform/of_save_format.h
	@mkdir -p $(dir $@)
	$(PC_CC) -O2 -Wall -Wextra tools/mksave.c -o $@

.PHONY: import-save export-save
import-save: $(MKSAVE)
	@[ -n "$(SAVE_DIR)" ] || { echo "import-save: give SAVE_DIR=<desktop save folder>"; exit 1; }
	$(MKSAVE) pack "$(SAVE_DIR)" $(SAVE_FILE)

export-save: $(MKSAVE)
	@[ -n "$(EXPORT_DIR)" ] || { echo "export-save: give EXPORT_DIR=<folder to write the save files into>"; exit 1; }
	$(MKSAVE) unpack $(SAVE_FILE) "$(EXPORT_DIR)"

# The program alone. It needs no game data, so it is what a machine without
# any (the CI) can build: `make elf && make GAME=deltarune elf`.
elf: $(OBJ_DIR)/app.elf
	@$(APP_SIZE)

# Build ELF, then assemble SD card image in build/<app>/
all: $(OBJ_DIR)/app.elf $(GAME_PACKS) release
	@$(APP_SIZE)

# Assemble the per-target deliverable.  Each platform owns its layout
# (pocket = APF tree, mister = disk image); the rule names no target —
# adding a target = add platforms/<target>/image.sh.
# Re-pointed on every build: a link left by an earlier SDK_ROOT would put that
# SDK's OS and bitstream on the card next to a program built against this one.
$(ROOT)/runtime: FORCE
	@mkdir -p $(ROOT)
	@[ "$$(readlink $@)" = "$(SDK_ROOT)/runtime" ] || ln -sfn $(SDK_ROOT)/runtime $@

FORCE:

ifeq ($(TARGET),pocket)
release: $(OBJ_DIR)/app.elf $(GAME_PACKS) | $(ROOT)/runtime
	@SAVESTATES=$(SAVESTATES) tools/mkcard.sh "$(SDK_DIR)/platforms/pocket/image.sh" "$(ROOT)" "$(DIST)" "$(CURDIR)"
else
release: $(OBJ_DIR)/app.elf $(GAME_PACKS) | $(ROOT)/runtime
	@$(SDK_DIR)/platforms/$(TARGET)/image.sh "$(APP)" "$(OBJ_DIR)/app.elf" "$(ROOT)" "$(DIST)" $(MISTER_ASSETS)
endif

debug: all
	@$(SDK_ROOT)/scripts/debug.sh $$(ls -d $(OUT)/Assets/*/common 2>/dev/null)/$(APP).elf

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
SDK09   ?= $(abspath $(CURDIR)/../../../Diablo)
OBJ09    = $(ROOT)/.obj/$(APP)-v09

compare: all
	$(MAKE) SDK_DIR=$(SDK09)/src/sdk OBJ_DIR=$(OBJ09) BUILD_DIR=$(OBJ09) $(OBJ09)/app.elf
	@tools/mkcompare.sh "$(OUT)" "$(SDK09)/runtime/pocket" "$(OBJ09)/app.elf"

# Like `copy`, but without re-running the assembly step, which would remove
# the comparison cores again.
compare-copy: compare
	@$(COPY_CMD)

package: all
	@$(SDK_ROOT)/scripts/package.sh $(APP)

# Desktop build. sdk.mk's app_pc rule assumes a Linux host (of_init.c needs
# <sys/auxv.h>, and sdl2-config omits the SDL2 parent include dir on
# Homebrew), so this core builds its own per-object desktop binary that
# works on macOS too. of_init.c only fetches the kernel's caps/services
# tables, which the PC shim does not use.
PC_OBJ_DIR = $(ROOT)/.obj/$(APP)-pc
ifeq ($(OF_SDK_IN_CONTAINER),1)
PC_SDL_CFLAGS =
PC_SDL_LIBS =
else
PC_SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags)
PC_SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs)
endif
PC_SRCS = $(SRCS) platform/of_pc_audio.c
PC_SDL_CFLAGS += -isystem $(SDK_DIR)/pc -DOF_PC_SDL_SOURCE='"of_sdl2.c"'
PC_OBJS = $(patsubst %.c,$(PC_OBJ_DIR)/%.o,$(subst ../,up/,$(PC_SRCS)))

$(PC_OBJ_DIR)/up/%.o: ../%.c
	@mkdir -p $(dir $@)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) -c -o $@ $<

$(PC_OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) -c -o $@ $<

PC_BIN ?= $(APP)_pc

$(PC_BIN): $(PC_OBJS)
	$(PC_CC) $(PC_OBJS) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $@

test: $(PC_BIN) $(GAME_PACKS)

# Asset-free clipped sprite pixel and memory-safety regression check.
.PHONY: sprite-clipping-fixture
sprite-clipping-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/sprite-clipping.c $(filter-out $(PC_OBJ_DIR)/main.o,$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_sprite_clipping_fixture

.PHONY: solid-fill-fixture
solid-fill-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/solid-fill.c $(filter-out $(PC_OBJ_DIR)/main.o $(filter %/sw_drawing.o,$(PC_OBJS)),$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_solid_fill_fixture

# Asset-free doubled-font sampling and filter-isolation regression check.
.PHONY: font-sampling-fixture
font-sampling-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/font-sampling.c $(filter-out $(PC_OBJ_DIR)/main.o,$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_font_sampling_fixture

# Asset-free menu layout, descriptions and navigation.
.PHONY: menu-fixture
menu-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/menu.c $(filter-out $(PC_OBJ_DIR)/main.o,$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_menu_fixture

# Shared enemy speech, battle labels/buttons, and filter isolation.
.PHONY: battle-contrast-fixture
battle-contrast-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/battle-contrast.c $(filter-out $(PC_OBJ_DIR)/main.o,$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_battle_contrast_fixture

# Asset-free VM/profiler accounting and between-execution toggle regression check.
.PHONY: profiler-fixture
profiler-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/profiler.c $(filter-out $(PC_OBJ_DIR)/main.o,$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_profiler_fixture

# Asset-free CALL/CALLV argument bounds, ownership and nested execution checks.
.PHONY: vm-call-args-fixture
vm-call-args-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/vm-call-args.c $(filter-out $(PC_OBJ_DIR)/main.o,$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_vm_call_args_fixture

# Deferred WAD17 callbacks use the real dispatcher and VM with no game assets.
.PHONY: call-later-fixture
call-later-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) $(BS)/tests/call_later_test.c $(filter-out $(PC_OBJ_DIR)/main.o $(filter %/runner.o,$(PC_OBJS)),$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_call_later_fixture

# Collision callbacks preserve self/other and inherited event context.
.PHONY: physics-contact-fixture
physics-contact-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) $(BS)/tests/physics_contact_test.c $(filter-out $(PC_OBJ_DIR)/main.o $(filter %/physics/physics.o,$(PC_OBJS)),$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_physics_contact_fixture

# Asset-free builtin ownership and audio compatibility checks.
.PHONY: builtin-compat-fixture
builtin-compat-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/builtin-compat.c $(filter-out $(PC_OBJ_DIR)/main.o,$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_builtin_compat_fixture
# Asset-free string read behavior, result ownership and input-copy regression.
.PHONY: string-reads-fixture
string-reads-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/string-reads.c $(filter-out $(PC_OBJ_DIR)/main.o $(filter %/vm_builtins.o,$(PC_OBJS)),$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_string_reads_fixture

# Asset-free collision dispatch ordering and optional work-count checks.
# Header-only precise-pair regression; instrumentation exists in this test TU only.
.PHONY: collision-precise-fixture
collision-precise-fixture:
	$(PC_CC) $(ALL_PC_CFLAGS) -finstrument-functions -fno-inline-functions tests/collision-precise.c -lm -o $(PC_BIN)_collision_precise_fixture

.PHONY: collision-work-fixture
collision-work-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/collision-work.c $(filter-out $(PC_OBJ_DIR)/main.o $(PC_OBJ_DIR)/up/runner.o $(filter %/src/runner.o,$(PC_OBJS)),$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_collision_work_fixture

# Asset-free spatial membership/recovery and bounded-update-work checks.
.PHONY: spatial-grid-fixture
spatial-grid-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/spatial-grid.c $(filter-out $(PC_OBJ_DIR)/main.o $(filter %/spatial_grid.o,$(PC_OBJS)),$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_spatial_grid_fixture

# Asset-free layer lookup order, mutation recovery and bounded-work checks.
.PHONY: layer-lookup-fixture
layer-lookup-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/layer-lookup.c $(filter-out $(PC_OBJ_DIR)/main.o $(PC_OBJ_DIR)/up/runner.o $(filter %/src/runner.o,$(PC_OBJS)),$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_layer_lookup_fixture

# Real-asset oracle for the guarded tile-shift native; pass data.win to the binary.
.PHONY: tile-shift-fixture
tile-shift-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) -finstrument-functions -fno-inline-functions tests/tile-shift.c $(filter-out $(PC_OBJ_DIR)/main.o $(PC_OBJ_DIR)/up/vm.o $(filter %/src/vm.o,$(PC_OBJS)),$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_tile_shift_fixture

# Real-asset oracle for the guarded active Forest sentry event.
.PHONY: forest-sentry-fixture
forest-sentry-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) -finstrument-functions -fno-inline-functions tests/forest-sentry.c $(filter-out $(PC_OBJ_DIR)/main.o $(PC_OBJ_DIR)/up/vm.o $(filter %/src/vm.o,$(PC_OBJS)),$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_forest_sentry_fixture

# Real-asset oracle for live background lookup and its guarded native path.
.PHONY: background-lookup-fixture
background-lookup-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) -finstrument-functions -fno-inline-functions tests/background-lookup.c $(filter-out $(PC_OBJ_DIR)/main.o $(PC_OBJ_DIR)/up/vm.o $(filter %/src/vm.o,$(PC_OBJS)),$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_background_lookup_fixture

# Asset-free same-frame wall/flip accounting and frame distribution checks.
.PHONY: focus-stats-fixture
focus-stats-fixture:
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/focus-stats.c platform/of_focus_stats.c $(PC_EXTRA_CFLAGS) -o $(PC_BIN)_focus_stats_fixture

# Asset-free tile composition and copy-call regression oracle.
.PHONY: tile-stripes-fixture
tile-stripes-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/tile-stripes.c $(filter-out $(PC_OBJ_DIR)/main.o $(filter %/sw_renderer.o,$(PC_OBJS)),$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_tile_stripes_fixture

# Real-asset state/RNG/command oracle for the guarded battle writer.
.PHONY: writer-draw-fixture
writer-draw-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) -finstrument-functions -fno-inline-functions tests/writer-draw.c $(filter-out $(PC_OBJ_DIR)/main.o $(PC_OBJ_DIR)/up/vm.o $(filter %/src/vm.o,$(PC_OBJS)),$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_writer_draw_fixture

# Requires an SDK staged with the matching save-state-enabled os25 runtime.
# Keep this output separate from the ordinary two-game core. Both games share
# the one core, and the Pocket's sleep switch is per core, so every game built
# here answers save and load requests (refusing what it cannot restore).
native-saves:
	$(MAKE) GAME=undertale SAVESTATES=1 ROOT=$(CURDIR)/out/native-saves all
	@if [ -f games/deltarune/data.win ]; then \
	    $(MAKE) GAME=deltarune SAVESTATES=1 ROOT=$(CURDIR)/out/native-saves all; \
	else echo "native-saves: no games/deltarune/data.win, the card has Undertale only"; fi

save-states: native-saves

snapshot-tests:
	@SDK_ROOT="$(SDK_ROOT)" tools/test-snapshots.sh

# The desktop build again, with AddressSanitizer and UndefinedBehaviorSanitizer,
# as <game>_pc_asan. Run it through the same frame scripts as <game>_pc:
# it stops with a report on an out-of-bounds access, a use after free or
# undefined arithmetic, which on the device would be garbage or a hang.
# Undefined-behaviour findings are printed once per place and the run goes on
# (vendored code has a few); a memory error stops it.
# On macOS, run it with DYLD_FALLBACK_LIBRARY_PATH=$(brew --prefix)/lib: under
# the sanitizer sdl2-compat cannot find SDL3 by itself and blocks on a dialog.
# -fno-sanitize=alignment,function: Butterscotch reads unaligned fields out of
# data.win and calls builtins through a differently typed pointer by design.
ASAN_FLAGS = -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined \
             -fno-sanitize=alignment,function
asan:
	$(MAKE) PC_BIN=$(APP)_pc_asan PC_OBJ_DIR=$(ROOT)/.obj/$(APP)-pc-asan \
	        PC_EXTRA_CFLAGS="$(ASAN_FLAGS)" $(APP)_pc_asan

# The desktop build again, counting every bytecode instruction the game runs,
# as <game>_pc_ops. Run it with BS_EXIT_FRAME=<n>: it prints the ranking
# when it leaves the main loop. A separate binary because the count is a
# compile-time option of Butterscotch's and costs time on every instruction.
ops:
	$(MAKE) PC_BIN=$(APP)_pc_ops PC_OBJ_DIR=$(ROOT)/.obj/$(APP)-pc-ops \
	        PC_EXTRA_CFLAGS="-DENABLE_VM_OPCODE_PROFILER" $(APP)_pc_ops

# Strict warnings for the code this port owns (not Butterscotch or the SDK,
# whose headers are treated as system headers so they stay quiet). Compiles
# nothing; fails if anything is reported.
LINT_SRCS = main.c $(wildcard platform/*.c) $(wildcard tools/*.c)
LINT_WARN = -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Wdouble-promotion \
            -Wstrict-prototypes -Wmissing-prototypes -Wundef -Wformat=2 -Werror
LINT_INCLUDES = -Iplatform -I. $(patsubst -I%,-isystem %,$(filter-out -Iplatform,$(BS_INCLUDES))) \
                -isystem $(SDK_DIR)/include
lint:
	@for f in $(LINT_SRCS); do \
	    $(PC_CC) -fsyntax-only -DOF_PC $(BS_DEFS) $(LINT_INCLUDES) $(PC_SDL_CFLAGS) $(LINT_WARN) $$f || failed=1; \
	done; [ -z "$$failed" ] && echo "lint: clean"

clean: sdk-clean
	rm -rf $(PC_OBJ_DIR) $(ROOT)/.obj/$(APP)-pc-asan $(ROOT)/.obj/$(APP)-pc-ops $(APP)_pc $(APP)_pc_asan $(APP)_pc_ops
	rm -rf $(ROOT)/build/pocket/$(CARD) $(ROOT)/build/mister/$(APP)

.PHONY: all elf release debug copy compare compare-copy package test asan ops lint clean native-saves save-states native-size snapshot-tests FORCE

# Asset-free translated tile picture reuse, composition and allocation regression.
.PHONY: tile-motion-fixture
tile-motion-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/tile-motion.c $(filter-out $(PC_OBJ_DIR)/main.o $(filter %/sw/sw_renderer.o,$(PC_OBJS)),$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_tile_motion_fixture


# Asset-free prompt replacement, font metrics, and positional icon rendering.
.PHONY: button-prompts-fixture
button-prompts-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/button-prompts.c $(filter-out $(PC_OBJ_DIR)/main.o $(PC_OBJ_DIR)/platform/game_$(GAME).o,$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_button_prompts_fixture

# Deltarune generated labels and interpreter fallbacks, without game assets.
.PHONY: deltarune-button-prompts-fixture
deltarune-button-prompts-fixture:
	$(MAKE) GAME=deltarune deltarune-button-prompts-fixture-build

.PHONY: deltarune-button-prompts-fixture-build
deltarune-button-prompts-fixture-build: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/deltarune-button-prompts.c $(filter-out $(PC_OBJ_DIR)/main.o $(PC_OBJ_DIR)/platform/game_deltarune.o,$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o deltarune_pc_button_names_fixture

.PHONY: gamepad-input-fixture
gamepad-input-fixture: $(PC_OBJS)
	$(PC_CC) $(ALL_PC_CFLAGS) $(PC_SDL_CFLAGS) tests/gamepad-input.c $(filter-out $(PC_OBJ_DIR)/main.o,$(PC_OBJS)) $(PC_SDL_LIBS) $(PC_EXTRA_CFLAGS) -lm -o $(PC_BIN)_gamepad_input_fixture
