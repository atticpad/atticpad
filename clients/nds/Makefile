# clients/nds/Makefile -- AtticPad Nintendo DS / DSi client.
#
# Shape mirrored from references/nds/examples/dswifi/get_website/Makefile:
# a handful of variables, then `include $(BLOCKSDS)/sys/default_makefiles/
# rom_arm9/Makefile`, which is the SDK's own build and finds the compiler
# through $(WONDERFUL_TOOLCHAIN) rather than PATH.
#
# BLOCKSDS default: the SDK makefile's own default is /opt/blocksds/core, but
# the PINNED IMAGE puts it at /opt/wonderful/thirdparty/blocksds/core and sets
# $BLOCKSDS in the environment. `?=` therefore picks up the image's value; the
# fallback below only matters for a host install that is neither.
BLOCKSDS	?= /opt/wonderful/thirdparty/blocksds/core

NAME		:= atticpad-nds

GAME_TITLE	:= AtticPad
GAME_SUBTITLE	:= LAN gamepad client
GAME_AUTHOR	:= atticpad

# The launcher icon. Without this the SDK falls back to $(BLOCKSDS)/sys/icon.gif,
# the BlocksDS placeholder -- so the ROM showed a generic icon in every
# homebrew menu while every other AtticPad artifact carries the gamepad mark.
# meta/icon.png is that mark reduced to the DS banner's 32x32, 16-colour form
# (make-assets on the 3DS side owns the master; this is derived from
# clients/3ds/meta/icon.png). ndstool accepts a PNG directly (-b ... file.png).
GAME_ICON	:= $(CURDIR)/meta/icon.png

# Libraries
# ---------
#
# ARM7ELF is NOT optional here. The default makefile's ARM7 core is
# arm7_maxmod.elf, which contains no DSWiFi ARM7 half at all -- the ROM links
# and boots and Wifi_InitDefault() then fails at runtime with nothing on
# screen to say why. get_website's Makefile is the reference for this.
ARM7ELF		:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi.elf
LIBS		:= -ldswifi9 -lnds9
LIBDIRS		:= $(BLOCKSDS)/libs/dswifi \
		   $(BLOCKSDS)/libs/libnds

# Source code paths
# -----------------
#
# SOURCEDIRS stays `source` AND NOTHING ELSE. The SDK makefile globs it with
# `find -L`, so `SOURCEDIRS += ../../shim` would drag in net_winsock.c and
# time_win32.c, and `+= ../../core/src` would put every object at
# build/atticpad-nds/../../core/src/*.o -- i.e. inside core/src/, outside this
# project, where `make clean` cannot reach it and where a second platform's
# build would collide with it.
#
# The shared translation units are therefore pulled in by ONE-LINE WRAPPER TUs
# in source/ (source/sh_*.c), each of which #includes the real file. The
# wrapper lives inside SOURCEDIRS, so its object lands in build/ where it
# belongs, and the real source is still compiled exactly once, from where it
# lives, with no copy in this directory. See source/sh_codec.c for the note.
SOURCEDIRS	:= source
# ../../clients/vendor/quirc/lib: the DSi QR pairing screen's decoder
# (screen_qrscan.c -> apad_qr.c -> quirc.h). Mirrors clients/3ds/Makefile's
# own INCLUDES line for the same vendored library.
INCLUDEDIRS	:= ../../core/include ../../clients/common \
		   ../../clients/vendor/quirc/lib

# Compiler flags
# --------------
#
# The SDK makefile builds its CFLAGS as `CFLAGS := -Wall <includes> <defines>
# <arch> -O2 ... $(CFLAGS)`, appending whatever we set here LAST -- so these
# win over its defaults rather than being replaced by them.
#
# NO -std HERE, DELIBERATELY. <nds.h> CANNOT BE COMPILED AS C99 -- twice over,
# and both failures are in the SDK's headers before a line of ours is reached:
#
#   1. <nds/ndstypes.h> defines
#        #define COMPILER_MEMORY_BARRIER() asm volatile("" ::: "memory")
#      which every inline vramSetBank*/glFlush in <nds/arm9/video.h> expands.
#      In strict C99 `asm` is not a keyword (only `__asm__` is), so it parses
#      as an undeclared identifier -- ~200 errors. -std=gnu99 fixes this one.
#   2. <nds/cothread.h> and <nds/device_list.h> use the ONE-ARGUMENT form
#        static_assert(sizeof(DeviceList) == 0x400);
#      which is C23. C11's static_assert needs two arguments and gnu99 has no
#      static_assert at all, so -std=gnu99 still fails here.
#
# Leaving -std unset is therefore not laziness: it is what the SDK's own
# default makefile and every vendored example do, and gcc 16's default is
# gnu23, which is the only dialect libnds actually compiles in.
#
# docs/CONVENTIONS.md's "C99 only" rule is about the SHARED code in core/ and shim/, and
# that rule is kept exactly -- see the per-file -std=c99 rule after the
# include, which builds every sh_*.c wrapper as strict C99. The three files
# that include <nds.h> are platform code and follow the platform's dialect.
#
# -pedantic is not used for the same reason. The shared sources were verified
# separately at -std=c99 -pedantic -Wall -Wextra -Werror on this exact
# toolchain and pass clean; it is only <nds.h> that cannot take either flag.
CFLAGS		:= -Wextra -Werror

# Dev hooks
# ---------
#
# All three are DEV ONLY and must never appear in a shipped build: a baked-in
# address is exactly what docs/CONVENTIONS.md forbids. They exist because a headless
# emulator run has nobody to press a button.
#
# Quoting: pass a BARE value on the command line and let the single quotes
# live HERE. Writing -DAPAD_AUTO_HOST='"1.2.3.4"' at the shell loses a quoting
# layer for every hop through shell -> docker -> make, and the preprocessor
# ends up seeing a bare 1.2.3.4 ("too many decimal points in number"). This is
# the same trap clients/psp/Makefile documents.
#
#   make APAD_AUTO_HOST=192.168.1.118 APAD_AUTO_PORT=21177 APAD_AUTO_AP=melonAP
#
# build.sh keeps a .hooks stamp and forces a clean when any of them changes,
# because make cannot see that CFLAGS changed and would happily relink stale
# objects that still contain the previous address.
ifneq ($(APAD_AUTO_HOST),)
CFLAGS		+= -DAPAD_AUTO_HOST='"$(APAD_AUTO_HOST)"'
endif
ifneq ($(APAD_AUTO_PORT),)
CFLAGS		+= -DAPAD_AUTO_PORT='"$(APAD_AUTO_PORT)"'
endif
ifneq ($(APAD_AUTO_AP),)
CFLAGS		+= -DAPAD_AUTO_AP='"$(APAD_AUTO_AP)"'
endif
# make APAD_AUTO_BCAST=1: one S7 tier-2 broadcast DISCOVER before the unicast
# probe, purely to record what a broadcast does on a given network. Off by
# default because it costs 1.5 s of dead time at startup.
ifneq ($(APAD_AUTO_BCAST),)
CFLAGS		+= -DAPAD_AUTO_BCAST=1
endif

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile

# docs/CONVENTIONS.md's C99 rule, enforced where it applies.
#
# Every source/sh_*.c is a one-line wrapper around a file in core/, shim/ or
# clients/common/, and those must be strict C99 -- they also build for a 67 MHz
# ARM9 with no MMU on toolchains far older than this one. Nearly all of them
# include no platform header, so none of THOSE hits the two C23-isms described
# above; net_bsd.c reaches only DSWiFi's
# <sys/socket.h>/<netinet/in.h>/<arpa/inet.h>, which are clean at c99.
#
# sh_net_nds.c IS THE ONE EXCEPTION and is EXCLUDED below. It wraps
# shim/net_nds.c (references/nds/README.md "Outcome B"), a platform shim that
# needs cothread_yield_irq()/IRQ_VBLANK from <nds.h> to poll around DSWiFi's
# broken recv timeout -- so unlike every other wrapper here, this one hits the
# same C23-isms as time_nds.c/wifi_nds.c/kbd_nds.c/ui.c and must build at the
# SDK's own default dialect (gnu23), not c99. It is still a one-line wrapper
# (the reason wrappers exist at all -- see shared-source-build-traps.md --
# is to keep the shared file's object inside $(BUILDDIR), which has nothing
# to do with dialect), just not a c99 one.
#
# A pattern-specific variable, so it applies to the wrappers and to nothing
# else; it must come after the include because $(BUILDDIR) is defined there.
# If this ever starts failing, the right answer is to find out what changed in
# the shared file, not to widen the dialect.
#
# sh_quirc_*.c ARE ALSO EXCLUDED, alongside sh_net_nds.c, but for a different
# reason: they wrap vendored, third-party quirc sources
# (clients/vendor/quirc/lib/*.c), not this project's own C99 shared code, and
# this project does not own quirc's dialect any more than it owns its
# warnings (see the -w rule just below). Forcing -std=c99 on code this repo
# did not write and has never needed to build at that dialect before
# (clients/3ds/Makefile builds it at devkitARM's own default, unconstrained)
# would be a NEW constraint invented for this port rather than one carried
# over from anywhere it has already been tried.
QUIRC_SH_SRCS := source/sh_quirc_quirc.c source/sh_quirc_identify.c \
		 source/sh_quirc_decode.c source/sh_quirc_version_db.c
SH_C99_SRCS := $(filter-out source/sh_net_nds.c $(QUIRC_SH_SRCS), \
			$(wildcard source/sh_*.c))
$(SH_C99_SRCS:source/%.c=$(BUILDDIR)/source/%.c.o): CFLAGS += -std=c99

# quirc: third-party source, third-party warning rules -- same reasoning and
# the same four files (by upstream name) as clients/3ds/Makefile's own
# QUIRC_OFILES rule, which see. "Not one byte of vendored source has been
# modified" (clients/vendor/quirc/README.md) is what makes `-w` the right
# tool instead of patching the warnings quiet.
$(QUIRC_SH_SRCS:source/%.c=$(BUILDDIR)/source/%.c.o): CFLAGS += -w

# NOT -marm FOR ui.c, AND THAT IS A MEASUREMENT, NOT AN OVERSIGHT.
#
# The obvious optimisation for a software rasteriser on this CPU is to build
# it in ARM mode instead of the SDK's -mthumb. It was tried, on the running
# console, read off the frame-time figure the diagnostics overlay shows
# (app_frame_ms()): 31 ms/frame in Thumb, 31 ms/frame in ARM, and 1.6 KB more
# ROM. Removing the two full-screen clears entirely did not move it either.
# So the client's ~31 ms frame (a 32 Hz send rate, since the engine sends one
# INPUT_STATE per pump and there is one pump per frame) is NOT the rasteriser,
# and this flag would have been cost with no benefit. Left out deliberately;
# the cause is recorded as unresolved in the platform notes.

# Extra target, after the include so $(ELF) is defined: `make size` is what
# the report quotes. Not part of `all` -- it is diagnostics, not a build step.
.PHONY: size
size: $(ELF)
	$(V)$(PREFIX)size $(ELF)
