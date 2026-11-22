# The viewer runs on the machine you sit at; screenserve and the probes on the board (`make board`, run there).
CFLAGS ?= -O2 -Wall
BOARD_BINS = board/screenserve probes/flip probes/mirror_check probes/vblank_rate

remotesbc: viewer/remote.c viewer/font.h
	$(CC) $(CFLAGS) viewer/remote.c -o $@ $$(pkg-config --cflags --libs sdl2) -llz4

board: $(BOARD_BINS)

# screenserve's sources are built when their libraries are present: x11 (X11, Xext, Xdamage, Xfixes), drm (libdrm)
X11_PKGS = x11 xext xdamage xfixes
SS_SRC = board/screenserve.c
SS_FLAGS =
SS_LIBS = -llz4 -lpthread
ifeq ($(shell pkg-config --exists $(X11_PKGS) && echo y),y)
SS_SRC += board/capture_x11.c
SS_FLAGS += -DHAVE_X11 $(shell pkg-config --cflags $(X11_PKGS))
SS_LIBS += $(shell pkg-config --libs $(X11_PKGS))
endif
ifeq ($(shell pkg-config --exists libdrm && echo y),y)
SS_SRC += board/capture_drm.c
SS_FLAGS += -DHAVE_DRM $(shell pkg-config --cflags libdrm)
SS_LIBS += $(shell pkg-config --libs libdrm)
endif

board/screenserve: $(SS_SRC) board/capture.h
	$(CC) $(CFLAGS) $(SS_FLAGS) $(SS_SRC) -o $@ $(SS_LIBS)

probes/flip: probes/flip.c
	$(CC) $(CFLAGS) $< -o $@ -lX11

probes/mirror_check: probes/mirror_check.c
	$(CC) $(CFLAGS) $< -o $@ -lX11 -llz4

probes/vblank_rate: probes/vblank_rate.c
	$(CC) $(CFLAGS) $< -o $@ -lX11 -lXext -lXpresent

clean:
	$(RM) remotesbc $(BOARD_BINS)

.PHONY: board clean
