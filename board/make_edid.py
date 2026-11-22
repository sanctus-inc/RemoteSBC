#!/usr/bin/env python3
"""Write a 128-byte EDID 1.3 block describing one 1920x1080 60 Hz mode (the CEA-861 timing).

The board's HDMI output is forced on when nothing is plugged in (or when a capture card holds
hot-plug down while it is not capturing), so that its display pipe keeps running: with no active
CRTC the X server paces Present clients (xfwm4, GTK) with a 1-second timer, and every click or
key shows on screen up to a second late. A forced connector with no EDID gets 1024x768; this
block makes it 1920x1080, the size screenserve streams.  Usage: make_edid.py OUT"""
import struct, sys


def edid_1080p60():
    e = bytearray(128)
    e[0:8] = b"\x00\xff\xff\xff\xff\xff\xff\x00"
    e[8:10] = struct.pack(">H", (ord("O") - 64) << 10 | (ord("P") - 64) << 5 | (ord("Z") - 64))  # maker "OPZ"
    e[10:12] = struct.pack("<H", 0x1080)          # product code
    e[16], e[17] = 1, 2026 - 1990                 # week, year
    e[18], e[19] = 1, 3                           # EDID 1.3
    e[20] = 0x80                                  # digital input
    e[21], e[22] = 53, 30                         # 53x30 cm
    e[23] = 120                                   # gamma 2.2
    e[24] = 0x0E                                  # RGB, sRGB, first detailed timing is preferred
    e[25:35] = bytes([0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54])  # sRGB chromaticity
    e[38:54] = b"\x01\x01" * 8                     # no standard timings
    # detailed timing: 148.5 MHz, 1920 + 280 blank (88 front, 44 sync), 1080 + 45 blank (4 front, 5 sync)
    ha, hb, va, vb, hfp, hsw, vfp, vsw = 1920, 280, 1080, 45, 88, 44, 4, 5
    d = bytearray(18)
    d[0:2] = struct.pack("<H", 14850)
    d[2], d[3], d[4] = ha & 0xFF, hb & 0xFF, (ha >> 8) << 4 | hb >> 8
    d[5], d[6], d[7] = va & 0xFF, vb & 0xFF, (va >> 8) << 4 | vb >> 8
    d[8], d[9] = hfp & 0xFF, hsw & 0xFF
    d[10] = (vfp & 0xF) << 4 | vsw & 0xF
    d[11] = (hfp >> 8) << 6 | (hsw >> 8) << 4 | (vfp >> 4) << 2 | vsw >> 4
    d[12], d[13], d[14] = 530 & 0xFF, 300 & 0xFF, (530 >> 8) << 4 | 300 >> 8
    d[17] = 0x1E                                  # digital separate sync, both positive
    e[54:72] = d
    e[72:90] = bytes([0, 0, 0, 0xFC, 0]) + b"SBC 1080p\n".ljust(13, b" ")    # monitor name
    e[90:108] = bytes([0, 0, 0, 0xFD, 0, 56, 76, 30, 81, 15, 0, 10]) + b" " * 6  # range limits
    e[108:126] = bytes([0, 0, 0, 0x10, 0]) + bytes(13)                        # dummy
    e[127] = (-sum(e[:127])) & 0xFF
    return bytes(e)


if __name__ == "__main__":
    blob = edid_1080p60()
    assert len(blob) == 128 and sum(blob) % 256 == 0
    open(sys.argv[1], "wb").write(blob)
