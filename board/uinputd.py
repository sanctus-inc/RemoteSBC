#!/usr/bin/env python3
"""Inject remote mouse/keyboard events into /dev/uinput.

Listens on TCP for a tiny binary protocol and replays each event through the
kernel's uinput interface, so X11 sees a genuine keyboard and mouse rather
than synthetic X events.  Written against ctypes so it needs no packages:
this board's userspace is fragile and every pacman transaction risks a
glibc-symbol mismatch.

Protocol: 8-byte little-endian records, <BHiB> = (kind, code, value, pad).
  kind 1 = EV_KEY   code = linux keycode, value 1 down / 0 up
  kind 2 = EV_REL   code = 0 (X) or 1 (Y), value = delta
  kind 3 = EV_ABS   code = 0 (X) or 1 (Y), value = absolute 0..32767
  kind 4 = sync only
"""
import ctypes, fcntl, os, select, socket, struct, sys, signal

EV_SYN, EV_KEY, EV_REL, EV_ABS = 0x00, 0x01, 0x02, 0x03
SYN_REPORT = 0
REL_X, REL_Y, REL_WHEEL = 0x00, 0x01, 0x08
ABS_X, ABS_Y = 0x00, 0x01

UI_SET_EVBIT, UI_SET_KEYBIT = 0x40045564, 0x40045565
UI_SET_RELBIT, UI_SET_ABSBIT = 0x40045566, 0x40045567
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x5501, 0x5502

BUS_USB = 0x03


class InputId(ctypes.Structure):
    _fields_ = [("bustype", ctypes.c_uint16), ("vendor", ctypes.c_uint16),
                ("product", ctypes.c_uint16), ("version", ctypes.c_uint16)]


class UinputSetup(ctypes.Structure):
    _fields_ = [("id", InputId), ("name", ctypes.c_char * 80),
                ("ff_effects_max", ctypes.c_uint32)]


class AbsInfo(ctypes.Structure):
    _fields_ = [("value", ctypes.c_int32), ("minimum", ctypes.c_int32),
                ("maximum", ctypes.c_int32), ("fuzz", ctypes.c_int32),
                ("flat", ctypes.c_int32), ("resolution", ctypes.c_int32)]


class UinputAbsSetup(ctypes.Structure):
    _fields_ = [("code", ctypes.c_uint16), ("absinfo", AbsInfo)]


UI_ABS_SETUP = 0x401c5504
ABS_MAX = 32767


def create_device():
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)

    for ev in (EV_KEY, EV_REL, EV_ABS):
        fcntl.ioctl(fd, UI_SET_EVBIT, ev)

    # every key we might send: 1..255 covers the standard keyboard range,
    # plus the three mouse buttons at 0x110..0x112
    for code in list(range(1, 256)) + [0x110, 0x111, 0x112]:
        try:
            fcntl.ioctl(fd, UI_SET_KEYBIT, code)
        except OSError:
            pass

    for rel in (REL_X, REL_Y, REL_WHEEL):
        fcntl.ioctl(fd, UI_SET_RELBIT, rel)
    for axis in (ABS_X, ABS_Y):
        fcntl.ioctl(fd, UI_SET_ABSBIT, axis)
        s = UinputAbsSetup()
        s.code = axis
        s.absinfo = AbsInfo(0, 0, ABS_MAX, 0, 0, 0)
        fcntl.ioctl(fd, UI_ABS_SETUP, s)

    setup = UinputSetup()
    setup.id = InputId(BUS_USB, 0x1209, 0x4b52, 1)
    setup.name = b"remotesbc"
    setup.ff_effects_max = 0
    fcntl.ioctl(fd, UI_DEV_SETUP, setup)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    return fd


def emit(fd, etype, code, value):
    # struct input_event: timeval{sec,usec} then type, code, value
    os.write(fd, struct.pack("@llHHi", 0, 0, etype, code, value))


def sync(fd):
    emit(fd, EV_SYN, SYN_REPORT, 0)


def serve(addr, port, fd):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((addr, port))
    srv.listen(1)
    print(f"uinputd listening on {addr}:{port}", flush=True)

    REC = struct.calcsize("<BHiB")
    while True:
        conn, addr = srv.accept()
        print(f"client {addr[0]}", flush=True)
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        buf = b""
        try:
            while True:
                # a new connection replaces this one: the newest viewer is the one a person is using,
                # and a hung viewer must not leave the next one's keys going nowhere
                ready, _, _ = select.select([conn, srv], [], [])
                if srv in ready:
                    print("a new client replaces the current one", flush=True)
                    break
                chunk = conn.recv(4096)
                if not chunk:
                    break
                buf += chunk
                while len(buf) >= REC:
                    kind, code, value, _ = struct.unpack("<BHiB", buf[:REC])
                    buf = buf[REC:]
                    if kind == 1:
                        emit(fd, EV_KEY, code, value); sync(fd)
                    elif kind == 2:
                        emit(fd, EV_REL, code, value); sync(fd)
                    elif kind == 3:
                        emit(fd, EV_ABS, code, value); sync(fd)
                    elif kind == 4:
                        sync(fd)
        except OSError as e:
            print(f"client error: {e}", flush=True)
        finally:
            conn.close()
            print("client gone; releasing all keys", flush=True)
            # never leave a key stuck down if the client vanishes mid-press
            for code in list(range(1, 256)) + [0x110, 0x111, 0x112]:
                emit(fd, EV_KEY, code, 0)
            sync(fd)


def main():
    # The protocol has no authentication: whoever reaches this port types as root. Bind it to one address, by
    # default loopback (reach it through `ssh -L`), or to a point-to-point cable whose other end you trust.
    addr = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5900
    fd = create_device()
    print("uinput device created: remotesbc", flush=True)

    def cleanup(*_):
        try:
            fcntl.ioctl(fd, UI_DEV_DESTROY)
            os.close(fd)
        except OSError:
            pass
        sys.exit(0)

    signal.signal(signal.SIGTERM, cleanup)
    signal.signal(signal.SIGINT, cleanup)
    try:
        serve(addr, port, fd)
    finally:
        cleanup()


if __name__ == "__main__":
    main()
