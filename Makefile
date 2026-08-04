HEAP_SIZE  = 8388208
STACK_SIZE = 61800

# NB: bundleID in Source/pdxinfo stays com.pomettini.vecx on purpose -- it keys
# the on-device data folder (settings.cfg), so changing it would orphan existing
# installs. Only the .pdx filename carries the CrankTrex name.
PRODUCT = CrankTrex.pdx

# Locate the SDK.
SDK = ${PLAYDATE_SDK_PATH}
ifeq ($(SDK),)
	SDK = $(shell egrep '^\s*SDKRoot' ~/.Playdate/config | head -n 1 | cut -c9-)
endif

ifeq ($(SDK),)
$(error SDK path not found; set ENV value PLAYDATE_SDK_PATH)
endif

VPATH += .

# LINK ORDER IS LOAD-BEARING (whole-binary I-cache packing, see PLAYDATE_ITCM_GUIDE.md).
# The CPU hot path (e6809.o + vecx.o) is acutely position-sensitive: moving it FIRST
# measured ~10% WORSE (Jun 10 2026), proving hand-tuning the layout is a dead end.
# render.c / jit.c stay LAST (don't shift the core); this order is the better-measured
# one of the two samples. The real fix is Tier 1 (hot path fully in DTCM -> packing-
# independent), after which this order stops mattering.
# rom_picker_unit.c #includes the pd-rom-picker submodule source via UINCDIR.
SRC = playdate_main.c e6809.c e8910.c vecx.c render.c jit.c rom_picker_unit.c

UINCDIR = pd-rom-picker/src
UASRC =
UDEFS =
UADEFS =
ULIBDIR =
ULIBS =

include $(SDK)/C_API/buildsupport/common.mk

OPT = -O3 -falign-functions=32 -fomit-frame-pointer
CPFLAGS += -flto
LDFLAGS += -flto

# The SDK's setup.c overrides newlib's allocator hooks (_malloc_r etc -> the
# Playdate's system->realloc). Under -flto those overrides are compiled to slim
# IR and the plugin DROPS them: their only consumer is newlib's libc.a, which is
# not an LTO unit, so LTO sees them as unreferenced. The link then fails with
# "undefined reference to _malloc_r" as soon as anything heap-allocates (the
# pd-rom-picker file list does). -u forces the linker to keep them.
LDFLAGS += -Wl,-u,_malloc_r -Wl,-u,_free_r -Wl,-u,_realloc_r

# build 92: .itcm section for runtime relocation of the hot core into fast TCM.
override LDSCRIPT = ./link_map.ld
# e6809.c -mlong-calls (relocated hot core's calls reach the originals) + -fno-lto.
# e6809.c: -mlong-calls so the relocated hot core's calls reach the PSRAM originals.
# (Tier 1 helper-relocation was tried + REVERTED: mechanism worked, but hot_core
# 2968B + ea_indexed 1416B = 4.4KB overruns the ~3.6KB safe DTCM pool. See NOTES.md.)
$(OBJDIR)/e6809.o: CPFLAGS += -mlong-calls -fno-lto

PLAYDATE_GAMES ?= /Volumes/PLAYDATE/Games

# Device-only project: never build the simulator (.dylib). `make` and `make
# install` target the device; everything is tested on hardware.
.DEFAULT_GOAL := device

.PHONY: install _push

install: device
	@test -d "$(PLAYDATE_GAMES)" || (echo "Playdate volume not mounted at $(PLAYDATE_GAMES)" && exit 1)
	$(RM) -rf "$(PLAYDATE_GAMES)/$(PRODUCT)"
	COPYFILE_DISABLE=1 cp -R "$(PRODUCT)" "$(PLAYDATE_GAMES)/"
	-dot_clean -m "$(PLAYDATE_GAMES)/$(PRODUCT)"

_push: install
