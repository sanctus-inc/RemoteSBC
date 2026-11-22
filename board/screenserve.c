/*
 * screenserve: stream a screen to one client as changed tiles.
 *
 * Built from first principles for a desktop, which changes little between frames:
 *   - a source (capture.h: an X11 display, or a DRM framebuffer for the console and any display server) says which
 *     64x64 tiles changed, so nothing is read while the screen is still;
 *   - only the bounding box of the changed tiles is read, so the cost follows the size of the change;
 *   - a tile is sent only when its pixels differ from what the client already has (change reports are coarse: a
 *     blinking cursor damages a rectangle larger than what changed);
 *   - each sent tile is compressed with LZ4, which on small ARM cores is several times faster than zstd's fastest
 *     level (zstd took 170 to 320 ms for a full screen), at a lower ratio the network absorbs; comparison and
 *     compression are split across the cores;
 *   - the pointer's position and image go separately when the source knows them (X draws the pointer outside the
 *     screen image).
 * Input does not go through here: the client sends it to uinputd (port 5900), which injects it as a real device.
 *
 * Wire format, little endian, server to client only:
 *   'H' u32 magic "TWR2", u16 width, u16 height                         once, on connect
 *   'F' u32 seq, u16 ntiles, then per tile: u16 x, y, w, h, u32 clen, clen bytes = LZ4 block of w*h BGRX pixels
 *   'P' i16 x, i16 y                                                   pointer moved
 *   'C' u16 w, h, xhot, yhot, then w*h u32 ARGB (premultiplied)       pointer shape changed
 *
 *   screenserve [SOURCE] [ADDRESS] [PORT]
 *     SOURCE: an X display (":0", the default; set XAUTHORITY) or a DRM device ("/dev/dri/card0", as root)
 *     ADDRESS: where to listen, default 127.0.0.1; PORT default 5901
 */
#include "capture.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <lz4.h>

#define TICK_MS 8	/* the most often a frame is sent: 120 per second at most */

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* Output buffer for one message, flushed with a single write so a frame is one TCP send. */
static unsigned char *out;
static size_t out_len, out_cap;

static void put(const void *p, size_t n)
{
	if (out_len + n > out_cap) {
		out_cap = (out_len + n) * 2;
		out = realloc(out, out_cap);
	}
	memcpy(out + out_len, p, n);
	out_len += n;
}
static void put8(uint8_t v) { put(&v, 1); }
static void put16(uint16_t v) { put(&v, 2); }
static void put32(uint32_t v) { put(&v, 4); }

/* Tile encoding, split across cores. Each worker compares, copies and compresses a contiguous run of the frame's
 * dirty tiles into its own buffer; the runs are joined in order, so the frame is the same as one core would make.
 * Tiles are disjoint, so the workers write disjoint parts of `sent`. On this board one core took 30 to 90 ms for a
 * full screen of tiles. */
#define MAX_WORKERS 8
struct enc {
	const int *list;		/* tile indices, ty * TX + tx */
	int from, to;
	const unsigned char *box;	/* the read-back box */
	int bpl, bx, by, W, H, TX;
	unsigned char *sent;
	unsigned char tile[TILE * TILE * 4];
	char *cbuf;
	int cbound;
	unsigned char *buf;		/* this worker's encoded tiles */
	size_t len, cap;
	uint16_t n;
};

static void enc_put(struct enc *e, const void *p, size_t n)
{
	if (e->len + n > e->cap) {
		e->cap = (e->len + n) * 2;
		e->buf = realloc(e->buf, e->cap);
	}
	memcpy(e->buf + e->len, p, n);
	e->len += n;
}

static void *encode_run(void *arg)
{
	struct enc *e = arg;
	e->len = 0;
	e->n = 0;
	for (int i = e->from; i < e->to; i++) {
		int tx = e->list[i] % e->TX, ty = e->list[i] / e->TX;
		int x = tx * TILE, y = ty * TILE;
		int w = x + TILE > e->W ? e->W - x : TILE, h = y + TILE > e->H ? e->H - y : TILE;
		int same = 1;
		for (int r = 0; r < h; r++) {
			const unsigned char *src = e->box + (size_t)(y - e->by + r) * e->bpl + (size_t)(x - e->bx) * 4;
			if (same && memcmp(src, e->sent + ((size_t)(y + r) * e->W + x) * 4, (size_t)w * 4))
				same = 0;
			memcpy(e->tile + (size_t)r * w * 4, src, (size_t)w * 4);
		}
		if (same)
			continue;
		for (int r = 0; r < h; r++)
			memcpy(e->sent + ((size_t)(y + r) * e->W + x) * 4, e->tile + (size_t)r * w * 4, (size_t)w * 4);
		int c = LZ4_compress_default((const char *)e->tile, e->cbuf, w * h * 4, e->cbound);
		uint16_t hdr[4] = { (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h };
		uint32_t clen = (uint32_t)c;
		enc_put(e, hdr, 8);
		enc_put(e, &clen, 4);
		enc_put(e, e->cbuf, (size_t)c);
		e->n++;
	}
	return NULL;
}

static int flush(int fd)
{
	size_t done = 0;
	while (done < out_len) {
		ssize_t n = write(fd, out + done, out_len - done);
		if (n <= 0)
			return -1;
		done += (size_t)n;
	}
	out_len = 0;
	return 0;
}

int main(int argc, char **argv)
{
	const char *source = argc > 1 ? argv[1] : ":0";
	const char *bind_addr = argc > 2 ? argv[2] : "127.0.0.1";
	int port = argc > 3 ? atoi(argv[3]) : 5901;
	signal(SIGPIPE, SIG_IGN);
	int timing = getenv("SCREENSERVE_TIMING") != NULL;	/* print each frame's read and compress times */

	struct capture *cap = NULL;
	if (!strncmp(source, "/dev/", 5)) {
#ifdef HAVE_DRM
		cap = capture_drm(source);
#else
		fprintf(stderr, "built without the drm source (libdrm was missing)\n");
#endif
	} else {
#ifdef HAVE_X11
		cap = capture_x11(source);
#else
		fprintf(stderr, "built without the x11 source (the X11 libraries were missing)\n");
#endif
	}
	if (!cap)
		return 1;
	int W = cap->W, H = cap->H;

	int TX = (W + TILE - 1) / TILE, TY = (H + TILE - 1) / TILE;
	unsigned char *sent = calloc((size_t)W * H, 4);	/* what the client holds */
	unsigned char *dirty = calloc((size_t)TX * TY, 1);
	int cbound = LZ4_compressBound(TILE * TILE * 4);
	long cores = sysconf(_SC_NPROCESSORS_ONLN);
	int workers = cores < 1 ? 1 : cores > MAX_WORKERS ? MAX_WORKERS : (int)cores;
	struct enc *enc = calloc((size_t)workers, sizeof *enc);
	for (int i = 0; i < workers; i++) {
		enc[i].cbuf = malloc((size_t)cbound);
		enc[i].cbound = cbound;
	}
	int *list = malloc((size_t)TX * TY * sizeof *list);

	int srv = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
	inet_pton(AF_INET, bind_addr, &sa.sin_addr);
	if (bind(srv, (struct sockaddr *)&sa, sizeof sa) || listen(srv, 1)) { perror("bind/listen"); return 1; }
	fprintf(stderr, "screenserve: %s, %dx%d, on %s:%d\n", source, W, H, bind_addr, port);

	for (;;) {
		int fd = accept(srv, NULL, NULL);
		if (fd < 0)
			continue;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		/* a viewer that stops reading is dropped after 2 s instead of blocking every later one */
		struct timeval snd = { 2, 0 };
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
		fprintf(stderr, "client connected\n");
		put8('H'); put32(0x32525754); put16((uint16_t)W); put16((uint16_t)H);
		memset(dirty, 1, (size_t)TX * TY);	/* a new client needs every tile */
		memset(sent, 0xA5, (size_t)W * H * 4);	/* compare against nothing it could hold */
		int cursor_due = 1, px = -1, py = -1;
		unsigned seq = 0;
		double last = 0;
		int alive = flush(fd) == 0;
		while (alive) {
			/* gather what the source reports: changed tiles and pointer image changes */
			if (cap->changes(cap, dirty, TX, TY, &cursor_due)) {
				fprintf(stderr, "the source is gone or changed size; exiting for a restart\n");
				return 3;
			}
			/* Wait for the source, the client and the listening socket together. The client never sends anything, so
			 * its socket turning readable means it closed; a write alone noticed that only when the screen
			 * next changed, and a still screen kept a departed viewer's session open while new viewers
			 * waited unanswered in the backlog with no window. A new connection replaces the current one:
			 * the newest viewer is the one a person is looking at. */
			double t = now_ms();
			int wait = t - last < TICK_MS ? (int)(TICK_MS - (t - last)) + 1 : 0;
			struct pollfd p[3] = { { cap->fd, POLLIN, 0 }, { fd, POLLIN, 0 }, { srv, POLLIN, 0 } };	/* fd -1 is skipped */
			poll(p, 3, wait);
			if (p[1].revents) {
				char b[64];
				ssize_t r = recv(fd, b, sizeof b, MSG_DONTWAIT);
				if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
					break;
			}
			if (p[2].revents & POLLIN) {
				fprintf(stderr, "a new client replaces the current one\n");
				break;
			}
			if (wait)
				continue;
			last = t;

			/* the pointer: its position every tick it moved, its shape when it changed */
			int rx, ry;
			if (cap->pointer(cap, &rx, &ry) && (rx != px || ry != py)) {
				px = rx; py = ry;
				put8('P'); put16((uint16_t)(int16_t)px); put16((uint16_t)(int16_t)py);
			}
			if (cursor_due) {
				int cw, ch, cxh, cyh;
				uint32_t *ci = cap->cursor(cap, &cw, &ch, &cxh, &cyh);
				if (ci) {
					put8('C'); put16((uint16_t)cw); put16((uint16_t)ch); put16((uint16_t)cxh); put16((uint16_t)cyh);
					put(ci, (size_t)cw * ch * 4);
					free(ci);
				}
				cursor_due = 0;
			}

			/* the screen: read the band of rows holding dirty tiles, send the tiles whose pixels changed */
			int ymin = TY, ymax = -1, xmin = TX, xmax = -1;
			for (int i = 0; i < TX * TY; i++)
				if (dirty[i]) {
					int ty = i / TX, tx = i % TX;
					if (ty < ymin) ymin = ty;
					if (ty > ymax) ymax = ty;
					if (tx < xmin) xmin = tx;
					if (tx > xmax) xmax = tx;
				}
			if (ymax >= 0) {
				cap->rearm(cap);
				/* read only the dirty tiles' bounding box */
				int bx = xmin * TILE, by = ymin * TILE;
				int bw = (xmax + 1) * TILE > W ? W - bx : (xmax + 1 - xmin) * TILE;
				int bh = (ymax + 1) * TILE > H ? H - by : (ymax + 1 - ymin) * TILE;
				int bpl;
				double g0 = now_ms();
				const unsigned char *box = cap->read(cap, bx, by, bw, bh, &bpl);
				double g1 = now_ms();
				if (!box) {
					fprintf(stderr, "reading the screen failed; exiting for a restart\n");
					return 3;
				}
				size_t bytes_at = out_len;
				size_t count_at = 0;
				uint16_t ntiles = 0;
				put8('F'); put32(seq++);
				count_at = out_len;
				put16(0);
				int nd = 0;
				for (int ty = ymin; ty <= ymax; ty++)
					for (int tx = 0; tx < TX; tx++)
						if (dirty[ty * TX + tx]) {
							dirty[ty * TX + tx] = 0;
							list[nd++] = ty * TX + tx;
						}
				/* a few tiles are not worth a thread each: one worker below 16 */
				int use = nd < 16 ? 1 : workers;
				pthread_t th[MAX_WORKERS];
				for (int i = 0; i < use; i++) {
					struct enc *e = &enc[i];
					e->list = list; e->from = nd * i / use; e->to = nd * (i + 1) / use;
					e->box = box; e->bpl = bpl;
					e->bx = bx; e->by = by; e->W = W; e->H = H; e->TX = TX; e->sent = sent;
					if (i == 0 || pthread_create(&th[i], NULL, encode_run, e))
						th[i] = 0;
				}
				encode_run(&enc[0]);
				for (int i = 1; i < use; i++) {
					if (th[i])
						pthread_join(th[i], NULL);
					else
						encode_run(&enc[i]);	/* the thread could not start: do its run here */
				}
				for (int i = 0; i < use; i++) {
					put(enc[i].buf, enc[i].len);
					ntiles += enc[i].n;
				}
				memcpy(out + count_at, &ntiles, 2);
				if (timing && ntiles)
					fprintf(stderr, "frame %u: box %dx%d read %.1f ms, %u tiles %.1f ms, %zu bytes\n", seq - 1,
						bw, bh, g1 - g0, ntiles, now_ms() - g1, out_len - bytes_at);
				if (!ntiles)
					out_len = count_at - 5;	/* nothing changed after all: drop the empty frame */
			}
			if (out_len && flush(fd))
				alive = 0;
		}
		close(fd);
		out_len = 0;
		fprintf(stderr, "client gone\n");
	}
}
