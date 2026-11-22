#!/bin/bash
# Install the board side, as root, from a checkout on the board:
#
#   board/install.sh [ADDR] [SOURCE]
#
# ADDR is the one address both services listen on (default 127.0.0.1). The protocol has no authentication or
# encryption: whoever reaches these ports sees the screen and types as root. Keep the default and reach the board
# through an SSH tunnel (README, "Security"), or give the board's end of a point-to-point cable you trust.
#
# SOURCE is where screenserve reads the screen: an X display (":0", the default) or a DRM device ("/dev/dri/card0"),
# which also shows the text console and works under any display server (README, "Screen sources").
#
# Installs: uinputd (input, port 5900) and screenserve (screen, port 5901) as systemd units. Builds screenserve here,
# so the board needs a C compiler, LZ4's headers, and the X11 (X11, Xext, Xdamage, Xfixes) or libdrm headers for the
# sources it should have.
#
# Optional, separately (both board-specific; README, "Board notes"):
#   board/20-shadowfb.conf      -> /etc/X11/xorg.conf.d/, when X draws straight into an uncached display buffer
#   board/hdmi-keepalive.service, when the board runs with no monitor attached
set -euo pipefail
ADDR=${1:-127.0.0.1}
SOURCE=${2:-:0}
HERE=$(cd "$(dirname "$0")" && pwd)
LIB=/usr/local/lib/remotesbc
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }

# For an X display, the X server's authority file, which screenserve needs to connect as a client. LightDM's is found;
# for another display manager set XAUTHORITY before running this.
if [ "${SOURCE#/dev/}" = "$SOURCE" ]; then
	XAUTH=${XAUTHORITY:-}
	[ -z "$XAUTH" ] && [ -e /run/lightdm/root/:0 ] && XAUTH=/run/lightdm/root/:0
	[ -n "$XAUTH" ] || { echo "set XAUTHORITY to the X server's authority file" >&2; exit 1; }
	SS_ENV="Environment=XAUTHORITY=$XAUTH"
	SS_AFTER=display-manager.service
	SS_WANTED=graphical.target
else
	SS_ENV=
	SS_AFTER=systemd-modules-load.service
	SS_WANTED=multi-user.target
fi

install -d "$LIB"
install -m 644 "$HERE/uinputd.py" "$LIB/uinputd.py"
make -C "$HERE/.." board/screenserve
install -m 755 "$HERE/screenserve" /usr/local/bin/screenserve

cat > /etc/systemd/system/uinputd.service <<UNIT
[Unit]
Description=Keyboard and mouse from the viewer, injected through /dev/uinput
# not network-online.target: on boards whose wait-online waits for every interface that holds the start for
# minutes; if ADDR is not up yet the bind fails and Restart retries
After=network.target

[Service]
ExecStartPre=/sbin/modprobe uinput
ExecStart=/usr/bin/python3 $LIB/uinputd.py $ADDR 5900
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
UNIT

cat > /etc/systemd/system/screenserve.service <<UNIT
[Unit]
Description=This board's screen as changed tiles, for the viewer's --net source ($SOURCE)
After=$SS_AFTER

[Service]
$SS_ENV
ExecStart=/usr/local/bin/screenserve $SOURCE $ADDR 5901
Restart=always
RestartSec=3

[Install]
WantedBy=$SS_WANTED
UNIT

systemctl daemon-reload
systemctl enable uinputd screenserve >/dev/null 2>&1
systemctl restart uinputd screenserve
sleep 2
for s in uinputd screenserve; do echo "$s: $(systemctl is-active $s)"; done
ss -tln | grep -E "$ADDR:590[01]" || echo "warning: nothing listening on $ADDR:5900/5901 yet (is the address up?)"
