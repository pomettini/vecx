HEAP_SIZE  = 8388208
STACK_SIZE = 61800

PRODUCT = vecx.pdx

# Locate the SDK.
SDK = ${PLAYDATE_SDK_PATH}
ifeq ($(SDK),)
	SDK = $(shell egrep '^\s*SDKRoot' ~/.Playdate/config | head -n 1 | cut -c9-)
endif

ifeq ($(SDK),)
$(error SDK path not found; set ENV value PLAYDATE_SDK_PATH)
endif

VPATH += .

# render.c / jit.c are LAST on purpose: objects linked after e6809.o/vecx.o don't
# shift them, so iterating on the renderer/JIT can't reshuffle the CPU hot path's
# I-cache packing (the 38.2-FPS layout). See PLAYDATE_ITCM_GUIDE.md.
SRC = playdate_main.c e6809.c e8910.c vecx.c render.c jit.c

UINCDIR =
UASRC =
UDEFS =
UADEFS =
ULIBDIR =
ULIBS =

include $(SDK)/C_API/buildsupport/common.mk

OPT = -O3 -falign-functions=32 -fomit-frame-pointer
CPFLAGS += -flto
LDFLAGS += -flto

# build 92: .itcm section for runtime relocation of the hot core into fast TCM.
override LDSCRIPT = ./link_map.ld
# e6809.c -mlong-calls (relocated hot core's calls reach the originals) + -fno-lto.
$(OBJDIR)/e6809.o: CPFLAGS += -mlong-calls -fno-lto

PLAYDATE_GAMES ?= /Volumes/PLAYDATE/Games

.PHONY: install _push

install: all
	@test -d "$(PLAYDATE_GAMES)" || (echo "Playdate volume not mounted at $(PLAYDATE_GAMES)" && exit 1)
	$(RM) -rf "$(PLAYDATE_GAMES)/$(PRODUCT)"
	COPYFILE_DISABLE=1 cp -R "$(PRODUCT)" "$(PLAYDATE_GAMES)/"
	-dot_clean -m "$(PLAYDATE_GAMES)/$(PRODUCT)"

_push: install
