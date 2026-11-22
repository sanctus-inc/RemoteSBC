# RemoteSBC

A keyboard, video and mouse console for a single-board computer, run from a desktop machine. The viewer is a
window on your desktop: it shows the board's screen and sends your keyboard and mouse to it, with VirtualBox-style
capture (focus the window to send the keyboard, click to send the mouse too, press Right Ctrl alone to release
both). It was built for an Orange Pi Zero 3 and runs on any Linux board with an X11 desktop.

The picture comes from one of two sources:

- **The network** (`--net`). A small server on the board sends its screen as changed 64x64 tiles. It reads the
  screen from an X11 desktop or straight from the display controller's framebuffer (DRM), which also covers the text
  console and Wayland desktops. Key press to pixels received on the Orange Pi Zero 3 is about 20 ms for a small
  change and 47 ms for a full 1920x1080 redraw from X11, 29 ms and 132 ms from DRM (measurements below).
- **An HDMI capture card** on the desktop machine. Slower (the capture chain adds 50 to 80 ms), but it shows
  everything the board's HDMI port shows, including the bootloader, kernel messages and a panic.

Input reaches the board as a real input device through the kernel's `uinput`, so every program on the board sees
ordinary key presses and mouse movement.

Nothing here comes from VNC or another remote-desktop code base; the parts are written against X11, the kernel and
SDL directly.

## Parts

| Path | Runs on | What it is |
|---|---|---|
| `viewer/remote.c` | your desktop | The SDL window: picture in, keyboard and mouse out, status bar |
| `board/uinputd.py` | the board | Receives input events over TCP (port 5900) and injects them through `/dev/uinput`. Standard library only |
| `board/screenserve.c` | the board | Sends the screen as changed tiles over TCP (port 5901) |
| `board/capture.h`, `board/capture_x11.c`, `board/capture_drm.c` | the board | Where `screenserve` reads the screen (see "Screen sources") |
| `board/install.sh` | the board | Builds `screenserve` and installs both as systemd units |
| `board/20-shadowfb.conf`, `board/hdmi-keepalive.service`, `board/make_edid.py` | the board | Optional, board-specific fixes (see "Board notes") |
| `probes/` | both | Measurement and correctness probes (see "Measurements") |

## Build and install

On the desktop machine (needs SDL2 and LZ4 development files):

```sh
make                      # builds ./remotesbc
```

On the board, as root, from a copy of this repository (needs a C compiler, make, Python 3, LZ4's development files,
and the X11, Xext, Xdamage and Xfixes or libdrm development files for the screen sources you want; the Makefile
builds each source whose libraries it finds):

```sh
board/install.sh                         # X11 desktop on :0, listening on 127.0.0.1 only; see "Security"
board/install.sh 127.0.0.1 /dev/dri/card0  # read the framebuffer instead: console, Wayland, or no desktop
```

`install.sh` finds LightDM's X authority file by itself; with another display manager, set `XAUTHORITY` to the X
server's authority file before running it.

## Use

```sh
ssh -N -L 5900:127.0.0.1:5900 -L 5901:127.0.0.1:5901 root@BOARD &   # see "Security"
./remotesbc --net                  # the board's screen over the network
./remotesbc                        # the board's HDMI through a capture card (found by name; a device path can be given)
```

Arguments: `remotesbc [--net] [HOST] [INPUT_PORT] [CAPTURE_DEVICE]`. HOST defaults to 127.0.0.1, INPUT_PORT to
5900, and the screen port is INPUT_PORT + 1.

Focusing the window sends the keyboard to the board, including shortcuts your desktop would otherwise take; a click
sends the mouse as well. Right Ctrl, pressed and released alone, gives both back and is never sent to the board. The
status bar says which machine has the keyboard and mouse (grey: yours; blue: the board) and warns when the input
link is down. When control is released, or the viewer exits, every key and button is lifted on the board, so
nothing stays held down.

One viewer is served at a time, and a new one replaces the current one. If the board does not answer within 3 s,
the viewer shows an error box and exits.

## Security

Neither service authenticates or encrypts anything. Whoever can connect to port 5900 types on the board as root,
and whoever can connect to 5901 sees its screen. For that reason `install.sh` binds both to 127.0.0.1 by default,
and the viewer defaults to 127.0.0.1, the near end of an SSH tunnel (the `ssh -L` line above). The tunnel provides
authentication and encryption; recent OpenSSH negotiates a post-quantum hybrid key exchange (`mlkem768x25519-sha256`
or `sntrup761x25519-sha512`) by default.

`install.sh ADDR` binds both services to another address instead. Use that only for an address no one else can
reach, such as the board's end of a cable that runs straight to your machine.

## How the network picture works

1. The screen source reports which 64x64 tiles changed (see "Screen sources").
2. At most every 8 ms, the bounding box of the changed tiles is read.
3. Each changed tile is compared with what the viewer already holds. Only tiles whose pixels differ are compressed
   with LZ4 and sent. The comparison and compression are split across the board's cores, one run of tiles per
   worker, and joined in order.
4. The pointer's position and shape are sent separately when the source knows them.

## Screen sources

`screenserve SOURCE ADDRESS PORT` takes its pixels from one of two sources behind one interface (`board/capture.h`):

| Source | SOURCE | Finds changes by | Pointer | Needs |
|---|---|---|---|---|
| X11 | an X display, `:0` | X's change reports (DAMAGE); reads through shared memory (MIT-SHM) | position and image (XFIXES), drawn by the viewer | an X11 session and its authority file |
| DRM | a DRM device, `/dev/dri/card0` | sampling: every 16 ms, 4 rows of each band of tiles are compared with the last read, at an offset that moves each time, so every row is checked within 256 ms and a change taller than 16 rows is found at once | part of the image when the desktop draws it in software; a hardware cursor plane is not shown | root, and a linear 32-bit framebuffer |

The DRM source works with no desktop at all (the text console), and under any display server, Wayland included,
because it reads the buffer the display controller shows. It refuses, with a message, a framebuffer that is tiled or
compressed, which some GPUs scan out; it has been tested only on the Orange Pi Zero 3, whose buffer is linear. If the
screen size changes, `screenserve` exits so its service restarts it with the new size.

The viewer keeps a copy of the screen and uploads only the box that changed to the GPU. The message format is
described at the top of `board/screenserve.c`; the input format (8-byte records) is at the top of
`board/uinputd.py`.

LZ4 rather than zstd: on the Orange Pi Zero 3's Cortex-A53 cores, zstd's fastest level took 170 to 320 ms for a
full screen against LZ4's 65 to 80 ms on one core.

## Measurements

`probes/latency.py` runs on the desktop machine. It sends a key press to `uinputd`, then times how long until a
frame arrives carrying tiles from the screen area it changed. The target is `probes/flip.c` on the board: a
window that changes colour on each key press, or with `--animate` draws a new colour every frame. The number covers
input to pixels received; the viewer's drawing and the monitor come after it.

Orange Pi Zero 3 (Allwinner H618, four Cortex-A53 cores, software rendering), 1920x1080, gigabit cable:

| Change on the board's screen | Key press to pixels received, median | Frames per second, continuous change |
|---|---|---|
| one 64x64 tile | 19.5 ms (10 to 20) | 51 to 60 |
| full screen, as first built | 156 ms | not measured cleanly |
| full screen, with X's shadow framebuffer | 80 ms | 17.5 |
| full screen, with the shadow framebuffer and tiles split across four cores | 46 to 47 ms | 21 |
| one 64x64 tile, DRM source | 29 ms | 46 |
| full screen, DRM source | 132 ms | 6.9 |

The DRM source reads the display buffer itself, which is uncached on this board (88 ms for a full screen), and finds
changes by sampling, which costs about 2 ms every 16 ms while the screen is still. The probe waits until no frame
has arrived for 200 ms before each key press, because the DRM source can send one change in several frames.

A full-screen frame of flat colour is 45 to 57 KB on the wire, against 8.3 MB of raw pixels (9.6 Mbit/s at 21
frames a second).

`probes/mirror_check.c` checks the stream pixel for pixel (both sources pass with no pixel different; with
`MIRROR_DUMP=FILE` it also writes the rebuilt screen as an image, the check used for the text console, where no X
server runs). It rebuilds the screen from the tiles it receives and,
once the stream has been quiet for a second, compares the result with the real screen. A test run should end on a
screen different from the one it started on. If the screen returns to its starting state, a tile that was never
sent still matches and the check cannot see it: a server that drops one tile per frame passed a run that ended back
at the desktop, and failed (4,086 pixels different) once the run ended on a new full-screen window.

`probes/vblank_rate.c` prints the interval between vblanks that the X server gives Present clients (see the next
section).

## Board notes

These were found on the Orange Pi Zero 3 with a mainline kernel and Xorg's modesetting driver. Other boards may
need the same fixes by other means.

**Keep the display pipe on.** With nothing on the HDMI port, the connector reads disconnected and the display
controller is switched off. Xorg then has no vblank to pace Present clients by (the window manager, GTK programs)
and uses a fallback timer that ticks once per second; `probes/vblank_rate.c` measured 1000 ms between vblanks,
against 16.7 ms with the pipe running. Clicks and keys reached the board at once, but their effect appeared up to a
second later, while the pointer, drawn separately, stayed smooth. Some HDMI capture cards hold hot-plug low while
nothing is capturing, which has the same effect. `board/hdmi-keepalive.service` writes a 1920x1080 60 Hz EDID
(generated by `board/make_edid.py`, which passes `edid-decode`'s conformity check) through debugfs and forces the
connector on before the display manager starts. Without an EDID, a forced connector comes up at 1024x768. To install it: `board/make_edid.py
/usr/local/lib/remotesbc/edid-1080p.bin`, copy the unit to `/etc/systemd/system/`, edit the connector name
(`HDMI-A-1`, `card0-HDMI-A-1`) and the debugfs path for your board, and enable it.

**Use X's shadow framebuffer when the display buffer is uncached.** When glamor is unavailable (no GPU driver, or
only software GL), the modesetting driver draws straight into the display buffer. On this board that memory is
uncached, so every read of the screen was slow, both `screenserve`'s read-back (85 ms for a full screen) and X's own
drawing whenever it reads back what it drew. `board/20-shadowfb.conf` (into `/etc/X11/xorg.conf.d/`) makes X draw
into a cached copy, and the read-back fell to 12 to 20 ms. The Xorg log reports the current setting as
`ShadowFB: preferred NO, enabled NO`.

**Raspberry Pi and other boards** (not tested here). `uinputd` and the viewer need nothing board-specific.
`screenserve` needs an X11 session; recent Raspberry Pi OS starts a Wayland session by default, which `raspi-config`
can switch to X11. On a Raspberry Pi, `hdmi_force_hotplug=1` in `config.txt` or a `video=` setting on the kernel
command line are the usual ways to keep HDMI on with no monitor. `screenserve` expects 32 bits per pixel and exits
with a message otherwise.

## Limits

- Wayland desktops are shown through the DRM source, without their pointer when it is a hardware cursor plane; no
  Wayland-native source yet.
- One viewer at a time.
- The keymap covers a standard keyboard, not the numeric keypad or media keys.
- The capture-card source uses YUYV 4:2:2, which halves colour resolution horizontally, so coloured text shows
  fringes.
- The status bar's font (`viewer/font.h`) is a bitmap rendered from DejaVu Sans by `viewer/make_font.py`.

## Licence

MIT, see `LICENSE`. The glyphs in `viewer/font.h` are rendered from DejaVu Sans, whose fonts are free under the
Bitstream Vera and DejaVu licence.
