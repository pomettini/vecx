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

PLAYDATE_GAMES ?= /Volumes/PLAYDATE/Games

.PHONY: install _push

install: all
	@test -d "$(PLAYDATE_GAMES)" || (echo "Playdate volume not mounted at $(PLAYDATE_GAMES)" && exit 1)
	$(RM) -rf "$(PLAYDATE_GAMES)/$(PRODUCT)"
	cp -R "$(PRODUCT)" "$(PLAYDATE_GAMES)/"

_push: install
