# clients/psp/Makefile -- AtticPad PSP client.
#
# Mirrors pspsdk's own samples (references/psp/) rather than the 3DS Makefile:
# build.mak has no wildcard globbing, so every object is named here. That is
# an advantage -- clients/3ds/Makefile has to $(filter-out) the shim files
# belonging to other platforms after globbing them in, and this simply never
# lists them.
TARGET = atticpad-psp

# vpath finds the SHARED sources where they live. Nothing is copied into this
# directory, for the same reason clients/3ds/Makefile points SOURCES at
# ../../core/src: one copy of the protocol, compiled per platform.
vpath %.c source:../../core/src:../../shim:../../clients/common

OBJS = main.o ui.o time_psp.o net_psp.o \
       screen_connect.o screen_session.o screen_selftest.o screen_fatal.o \
       numpad.o devlog.o config_psp.o power_psp.o \
       codec.o seq.o session.o hmac_sha256.o selftest.o \
       net_bsd.o \
       apad_client.o

INCDIR   = source ../../core/include ../../clients/common
CFLAGS   = -O2 -G0 -Wall -Wextra -std=gnu99
# Dev hook: `make APAD_SMOKE_HOST=192.168.1.118` builds the phase-1 socket
# check, which sends through shim/net_bsd.c to that address. Takes a BARE
# address -- the quoting lives here, in single quotes, because passing
# -DAPAD_SMOKE_HOST='"1.2.3.4"' through shell -> docker -> make loses a layer
# every time and the preprocessor ends up seeing a bare 1.2.3.4 ("too many
# decimal points in number").
#
# Note this APPENDS. Setting CFLAGS on the command line would replace the line
# above and take build.mak's -I paths with it ("pspkernel.h: No such file").
# Dev hook: `make APAD_PSP_SHOTS=1` walks the screens and dumps each to the
# memory stick. Never enabled in a shipped build -- it writes files.
ifneq ($(APAD_PSP_SHOTS),)
CFLAGS  += -DAPAD_PSP_SHOTS=1
endif

# Dev hook: `make APAD_AUTO_HOST=192.168.1.118 APAD_AUTO_PORT=21167` pre-fills
# the address and connects on its own, so the handshake can be exercised in a
# headless emulator run where no button can be pressed. Never in a shipped
# build: an address baked into a binary is exactly what docs/CONVENTIONS.md S2 forbids.
# Dev hook: `make APAD_PSP_DEVLOG=1` writes ms0:/atticpad.log.
ifneq ($(APAD_PSP_DEVLOG),)
CFLAGS  += -DAPAD_PSP_DEVLOG=1
endif
# Dev hook: `make APAD_PSP_FAKE_RESUME=20` injects one synthetic
# suspend+resume 20 s after boot, so the power-callback path can be RUN in an
# emulator that cannot suspend a PSP (PPSSPP cannot). It fakes only the two
# event counters; everything downstream is the shipping path. Never in a
# shipped build.
ifneq ($(APAD_PSP_FAKE_RESUME),)
CFLAGS  += -DAPAD_PSP_FAKE_RESUME=$(APAD_PSP_FAKE_RESUME)
endif
# Dev hook: `make APAD_PSP_STDOUT=1` mirrors apad_devlog to stdout, which
# PSPLINK relays to pspsh over USB -- live diagnostics on a transport that
# survives the Wi-Fi drop. Never in a shipped build.
ifneq ($(APAD_PSP_STDOUT),)
CFLAGS  += -DAPAD_PSP_STDOUT=1
endif

ifneq ($(APAD_AUTO_HOST),)
CFLAGS  += -DAPAD_AUTO_HOST='"$(APAD_AUTO_HOST)"'
endif
ifneq ($(APAD_AUTO_PORT),)
CFLAGS  += -DAPAD_AUTO_PORT='"$(APAD_AUTO_PORT)"'
endif

ifneq ($(APAD_SMOKE_HOST),)
CFLAGS  += -DAPAD_SMOKE_HOST='"$(APAD_SMOKE_HOST)"'
endif
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)
LIBDIR   =
LDFLAGS  =

# THE LIBS LINE IS ONE WORD, AND STAYS ONE WORD.
#
# The spike that opened this port found that a long LIBS list made sendto()
# return 142760828 -- an address, not a byte count -- and silently send
# nothing, while socket() still returned a plausible 4. pspsdk's net sample
# declares no LIBS at all and lets build.mak supply the defaults that map
# POSIX socket()/sendto() onto sceNetInet; overriding that is what broke it.
#
# The one exception is battery. build.mak's default list is
#   -lpspdebug -lpspdisplay -lpspge -lpspctrl -lpspnet -lpspnet_apctl
# and contains no -lpsppower, so scePowerGetBatteryLifePercent() cannot link
# without it. pspsdk's own samples/power/Makefile declares exactly
# `LIBS = -lpsppower`, and per docs/CONVENTIONS.md's blind-platform rule the sample
# wins. build.mak does `LIBS := $(LIBS) ...`, so this PREPENDS rather than
# replaces.
#
# DO NOT EXTEND THIS LINE without re-running the UDP smoke test against a
# host listener. "It compiled" is not evidence of working sockets here.
LIBS = -lpsppower

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = AtticPad
# XMB art: build.mak's own variables (references/psp/build.mak). ICON0 is the
# 144x80 tile in the game list, PIC1 the 480x272 backdrop behind it. Both
# are written by clients/3ds/meta/make-assets.py --psp; without them the XMB
# shows a blank tile.
PSP_EBOOT_ICON  = meta/ICON0.PNG
PSP_EBOOT_PIC1  = meta/PIC1.PNG

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
