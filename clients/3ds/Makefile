#---------------------------------------------------------------------------------
# clients/3ds/Makefile
#
# Based on devkitPro's application template
# (/opt/devkitpro/examples/3ds/templates/application/Makefile in the pinned
# devkitARM image, scripts/toolchains.env DEVKITARM), the same skeleton every
# references/3ds/*/Makefile carries. Two changes from the stock template:
#
#   - SOURCES also points at ../../core/src, so libapad's codec/session/
#     seq/hmac/self-test are compiled straight from core/, never copied
#     (docs/CONVENTIONS.md: clients call libapad, they don't reimplement it).
#   - SOURCES also points at ../../shim, for the SHARED shim/net_bsd.c
#     (docs/DESIGN.md S3: "net_bsd.c -- 3DS, PSP, Vita, Switch, Linux, macOS,
#     Android"; commit 975d01b added the <arpa/inet.h> include libctru
#     needs). shim/time_posix.c is deliberately excluded below -- a
#     monotonic clock genuinely is per-platform, and clients/3ds/source/
#     time_3ds.c (osGetTime()) is this platform's copy of it, the same way
#     shim/time_posix.c is Linux's.
#   - SOURCES also points at ../../clients/vendor/quirc/lib, the vendored
#     QR recognizer (clients/vendor/quirc/README.md), for the camera QR
#     pairing screen (docs/PROTOCOL.md S10.3). Those four .c files are
#     THIRD-PARTY and get their own warning flags -- see QUIRC_OFILES near
#     the bottom of this file.
#
#   - SOURCES also points at ../../clients/common, for the SHARED client
#     session engine apad_client.c -- the S8 handshake, S9 ACKs, S6.6 PING
#     both ways and the INPUT_STATE pacing that source/main.c used to
#     hand-drive. Compiled straight from clients/common/ for exactly the
#     reason core/src is: one copy, or the next protocol fix has to be made
#     once per client. Android links the same file through its CMakeLists.
#   - INCLUDES also points at ../../core/include for atticpad/*.h and at
#     ../../clients/common for apad_client.h.
#
#   - The DEVKITARM requirement and the include of $(DEVKITARM)/3ds_rules
#     below are skipped when `cia` is the only goal. .cia packaging
#     (docs/DESIGN.md S8.3, see the `cia:` rule further down) runs makerom against
#     an already-built $(OUTPUT).elf and needs no ARM cross-compiler at
#     all -- requiring devkitARM just to run makerom would force
#     clients/3ds/build.sh's `cia` action to launch the whole devkitARM
#     container a second time for a step that is really just a host binary
#     copying bytes around. `make all` and every other target still require
#     it exactly as before.
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(filter cia,$(MAKECMDGOALS)),cia)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules
endif

#---------------------------------------------------------------------------------
TARGET		:=	atticpad-3ds
BUILD		:=	build
SOURCES		:=	source ../../core/src ../../shim ../../clients/common \
			../../clients/vendor/quirc/lib
DATA		:=	data
INCLUDES	:=	../../core/include ../../clients/common \
			../../clients/vendor/quirc/lib
GRAPHICS	:=	gfx
GFXBUILD	:=	$(BUILD)

APP_TITLE		:=	AtticPad
APP_DESCRIPTION	:=	Use this 3DS as a PC gamepad over Wi-Fi
APP_AUTHOR		:=	AtticPad

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -Wextra -O2 -mword-relocations \
			-ffunction-sections \
			-std=c99 \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# citro2d/citro3d: the UI pass replaced consoleInit()+printf with rendered
# screens. Both libraries ship inside the pinned devkitARM image
# (/opt/devkitpro/libctru/lib, verified against the digest in
# scripts/toolchains.env), so this adds NO new
# dependency to fetch or pin -- it is the same $(CTRULIB) directory -lctru
# already comes from. Link order matters: citro2d calls into citro3d, which
# calls into ctru.
LIBS	:= -lcitro2d -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
LIBDIRS	:= $(CTRULIB)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
# ../../shim is globbed, so every platform's shim lands here and the ones
# that are not this platform's must be named out. shim/time_posix.c is the
# Linux monotonic clock and this platform's is clients/3ds/source/time_3ds.c
# (osGetTime()); shim/net_winsock.c and shim/time_win32.c are Windows' and
# arrived with M4. Only shim/net_bsd.c is wanted from ../../shim -- see the
# SOURCES comment above.
#
# Those two Windows files also guard their whole contents behind
# `#if defined(_WIN32)`, so a glob that misses them here compiles them to
# empty objects rather than failing on <winsock2.h>. Belt and braces on
# purpose: this Makefile is not the only thing that globs shim/, and a blind
# platform discovering the problem on hardware is the expensive way to find
# out.
#
# ../../clients/common is globbed the same way. Its second file, apad_qr.c
# (the QR-decode adapter around the vendored quirc), used to be named out
# here because this build had neither quirc's include path nor a camera
# screen to use it from. It now has both -- SOURCES and INCLUDES above pick
# up clients/vendor/quirc/lib, exactly as that comment said the fix would be
# -- so apad_qr.c compiles here, from the same file Android compiles, and is
# no longer filtered.
#
# apad_ui.c and apad_ui_strings.c (the shared customer-facing message
# catalog and the apad_client_stats -> message-id precedence, both consumed
# by every screen_*.c through app.h's apad_ui.h include) compile the same
# way: no separate SOURCES/INCLUDES entry needed, because ../../clients/common
# was already globbed and included above for apad_client.c and apad_qr.c.
CFILES		:=	$(filter-out time_posix.c net_winsock.c time_win32.c,$(CFILES))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
ifeq ($(GFXBUILD),$(BUILD))
#---------------------------------------------------------------------------------
export T3XFILES :=  $(GFXFILES:.t3s=.t3x)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
export ROMFS_T3XFILES	:=	$(patsubst %.t3s, $(GFXBUILD)/%.t3x, $(GFXFILES))
export T3XHFILES		:=	$(patsubst %.t3s, $(BUILD)/%.h, $(GFXFILES))
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES)) \
			$(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o) \
			$(addsuffix .o,$(T3XFILES))

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
			$(addsuffix .h,$(subst .,_,$(BINFILES))) \
			$(GFXFILES:.t3s=.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean cia

#---------------------------------------------------------------------------------
# .cia packaging (docs/DESIGN.md §8.3). Runs entirely in the top-level Makefile
# context (this branch, not the recursive $(BUILD)-dir invocation below) --
# same as `all` and `clean` above -- because it needs no devkitARM/3ds_rules
# machinery, only $(OUTPUT).elf (already built by `all`) plus makerom and
# the committed assets in meta/.
#
# makerom is fetched separately (clients/3ds/build.sh's `cia` action
# locally, a tag-gated CI step in .github/workflows/ci.yml) and expected on
# PATH, or its path passed via MAKEROM=. Not devkitARM/DEVKITARM-gated: unlike
# `all`, this target compiles nothing 3DS-specific -- makerom is an
# ordinary host binary operating on an already-built ELF.
#
# bannertool is NOT invoked here on purpose -- see scripts/cia-tools.env
# and meta/app.rsf's header comment for why (docs/DESIGN.md §8.3 / risk 11:
# bannertool is archived/gone upstream). meta/banner.bnr and meta/icon.icn
# are committed prebuilt outputs, fed to makerom directly.
#---------------------------------------------------------------------------------
MAKEROM      ?= makerom

# CIA title version, packed the way makerom's -ver expects:
# major<<10 | minor<<4 | micro, each read from core/include/atticpad/version.h
# so bumping the product version bumps the title version with it. The HOME
# menu caches the grid icon PER TITLE VERSION — shipping new icon.icn at the
# same version leaves the old icon on screen (observed on hardware,
# 2026-08-12: banner refreshed, icon did not). That day carried a +1 micro
# here as a cache-evicting bump; the 0.4.0 version bump subsumes it (64 > the
# 49 that shipped), so this is now a plain reading of version.h again.
CIA_VER := $(shell awk '/APAD_VERSION_MAJOR/{ma=$$3} /APAD_VERSION_MINOR/{mi=$$3} /APAD_VERSION_PATCH/{pa=$$3} END{print ma*1024+mi*16+pa}' ../../core/include/atticpad/version.h)
CIA_RSF      := meta/app.rsf
CIA_BANNER   := meta/banner.bnr
CIA_ICON     := meta/icon.icn

cia: $(OUTPUT).elf $(CIA_RSF) $(CIA_BANNER) $(CIA_ICON)
	@command -v $(MAKEROM) >/dev/null 2>&1 || { \
		echo "clients/3ds/Makefile: '$(MAKEROM)' not found on PATH."; \
		echo "  Local build: clients/3ds/build.sh cia (fetches the pinned release from scripts/cia-tools.env)."; \
		echo "  Or pass MAKEROM=/path/to/makerom explicitly."; \
		exit 1; \
	}
	$(MAKEROM) -f cia -o $(OUTPUT).cia -rsf $(CIA_RSF) -target t -exefslogo -ver $(CIA_VER) \
		-elf $(OUTPUT).elf -icon $(CIA_ICON) -banner $(CIA_BANNER) \
		-DAPP_ENCRYPTED=false
	@echo "built $(OUTPUT).cia"

#---------------------------------------------------------------------------------
all: $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

ifneq ($(GFXBUILD),$(BUILD))
$(GFXBUILD):
	@mkdir -p $@
endif

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(TARGET).cia $(GFXBUILD)

#---------------------------------------------------------------------------------
$(GFXBUILD)/%.t3x	$(BUILD)/%.h	:	%.t3s
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@tex3ds -i $< -H $(BUILD)/$*.h -d $(DEPSDIR)/$*.d -o $(GFXBUILD)/$*.t3x

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
# quirc: third-party source, third-party warning rules.
#
# clients/vendor/quirc/README.md's "Build notes" are explicit that lib/*.c
# does not compile clean under this project's warning flags (-Wsign-compare
# three times, -Wunused-parameter once, all pre-existing upstream code) and
# that "not one byte of vendored source has been modified" -- so it cannot be
# patched quiet, and dropping -Wall/-Wextra globally to accommodate it would
# hide warnings in code this project DID write. The Android build solves this
# with a separate CMake OBJECT library; the make equivalent is a
# target-specific variable, which applies only to these four objects and
# leaves every other .o compiled exactly as before.
#
# -w rather than a list of -Wno-*: the list would need revisiting at every
# re-pin of the vendored commit, which is precisely the maintenance this
# project does not want to owe a dependency it does not own.
#---------------------------------------------------------------------------------
QUIRC_OFILES	:=	quirc.o identify.o decode.o version_db.o

$(QUIRC_OFILES) : CFLAGS += -w

#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
.PRECIOUS	:	%.t3x %.shbin
#---------------------------------------------------------------------------------
%.t3x.o	%_t3x.h :	%.t3x
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

#---------------------------------------------------------------------------------
%.shbin.o %_shbin.h : %.shbin
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
