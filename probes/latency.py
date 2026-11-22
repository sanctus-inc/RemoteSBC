#!/usr/bin/env python3
"""End-to-end latency and throughput of the network picture, measured from the workstation.

On the board, screen/probes/flip.c shows a window at a known place that changes colour on each key press (or flips
continuously with --animate). This script takes over screenserve (5901) and uinputd (5900), then:
  latency: sends a key press, and times until a frame arrives carrying a tile inside the window (the change),
           repeated N times; this is input to pixels received, before the viewer draws them.
  stream:  with the window animating, counts frames, tiles and bytes received per second.
Prints JSON. Usage: latency.py HOST latency X Y W H [N] | latency.py HOST stream SECONDS"""
import json, socket, struct, sys, time


def read_exact(s, n):
    b = bytearray()
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return bytes(b)


class Picture:
    def __init__(self, host):
        self.s = socket.create_connection((host, 5901))
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        k, magic, self.W, self.H = struct.unpack("<cIHH", read_exact(self.s, 9))
        assert k == b"H" and magic == 0x32525754

    def message(self):
        """('F', [(x, y, w, h, bytes)], total_bytes) | ('P', None, 5) | ('C', None, n)"""
        k = read_exact(self.s, 1)
        if k == b"P":
            read_exact(self.s, 4)
            return "P", None, 5
        if k == b"C":
            w, h, _, _ = struct.unpack("<HHHH", read_exact(self.s, 8))
            read_exact(self.s, w * h * 4)
            return "C", None, 9 + w * h * 4
        if k == b"F":
            _, count = struct.unpack("<IH", read_exact(self.s, 6))
            tiles, total = [], 7
            for _ in range(count):
                x, y, w, h, clen = struct.unpack("<HHHHI", read_exact(self.s, 12))
                read_exact(self.s, clen)
                tiles.append((x, y, w, h, clen))
                total += 12 + clen
            return "F", tiles, total
        raise ValueError(f"unknown message {k!r}")


def main():
    host, mode = sys.argv[1], sys.argv[2]
    pic = Picture(host)
    pic.s.settimeout(0.5)
    t_end = time.monotonic() + 2.0          # let the first full frame pass
    while time.monotonic() < t_end:
        try:
            pic.message()
        except socket.timeout:
            pass
    if mode == "latency":
        X, Y, W, H = map(int, sys.argv[3:7])
        n = int(sys.argv[7]) if len(sys.argv) > 7 else 30
        inp = socket.create_connection((host, 5900))
        inp.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        pic.s.settimeout(3.0)
        lags = []
        warm = 3   # presses not counted: the target's keyboard grab and first redraws can lag its start
        for i in range(n + warm):
            # wait until no frame has arrived for 200 ms: a source that finds a change in pieces (the drm source's
            # sampling) sends several frames per press, and a late one would be counted against the next press
            pic.s.settimeout(0.2)
            while True:
                try:
                    pic.message()
                except socket.timeout:
                    break
            pic.s.settimeout(3.0)
            t0 = time.monotonic()
            inp.sendall(struct.pack("<BHiB", 1, 88, 1, 0) + struct.pack("<BHiB", 1, 88, 0, 0))   # F12 down, up
            while True:
                try:
                    kind, tiles, _ = pic.message()
                except socket.timeout:
                    lags.append(None)
                    break
                if kind == "F" and any(X <= x < X + W and Y <= y < Y + H for x, y, *_ in tiles):
                    lags.append((time.monotonic() - t0) * 1000)
                    break
        lags = lags[warm:]
        got = sorted(l for l in lags if l is not None)
        print(json.dumps({"mode": "latency", "window": [X, Y, W, H], "n": n, "missed": lags.count(None),
                          "ms_min": round(got[0], 1), "ms_median": round(got[len(got) // 2], 1),
                          "ms_p90": round(got[int(len(got) * 0.9)], 1), "ms_max": round(got[-1], 1)}))
    elif mode == "stream":
        secs = float(sys.argv[3])
        frames = tiles = nbytes = 0
        t0 = time.monotonic()
        pic.s.settimeout(1.0)
        while time.monotonic() - t0 < secs:
            try:
                kind, ts, b = pic.message()
            except socket.timeout:
                continue
            nbytes += b
            if kind == "F":
                frames += 1
                tiles += len(ts)
        dt = time.monotonic() - t0
        print(json.dumps({"mode": "stream", "seconds": round(dt, 1), "frames_per_s": round(frames / dt, 1),
                          "tiles_per_frame": round(tiles / max(frames, 1), 1),
                          "bytes_per_frame": round(nbytes / max(frames, 1)), "mbit_per_s": round(nbytes * 8 / dt / 1e6, 1),
                          "raw_frame_bytes": pic.W * pic.H * 4}))


if __name__ == "__main__":
    main()
