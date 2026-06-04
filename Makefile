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

SRC = playdate_main.c e6809.c e8910.c vecx.c

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

# build 78: local linker script adds an .itcm section for runtime relocation
# of hot code into fast TCM-backed memory.
override LDSCRIPT = ./link_map.ld

# build 88: compile e6809.c -Os (so the relocated e6809_sstep is ~8.9KB and its
# pool sits above the 0x20003cc0 DTCM hole), -mlong-calls (calls reach the
# originals via absolute addresses after relocation) and -fno-lto.
$(OBJDIR)/e6809.o: OPT := -Os -falign-functions=32 -fomit-frame-pointer
$(OBJDIR)/e6809.o: CPFLAGS += -mlong-calls -fno-lto

PLAYDATE_GAMES ?= /Volumes/PLAYDATE/Games

.PHONY: install _push

install: all
	@test -d "$(PLAYDATE_GAMES)" || (echo "Playdate volume not mounted at $(PLAYDATE_GAMES)" && exit 1)
	$(RM) -rf "$(PLAYDATE_GAMES)/$(PRODUCT)"
	COPYFILE_DISABLE=1 cp -R "$(PRODUCT)" "$(PLAYDATE_GAMES)/"
	-dot_clean -m "$(PLAYDATE_GAMES)/$(PRODUCT)"

_push: install
