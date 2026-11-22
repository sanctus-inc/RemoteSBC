/* Correctness check for screenserve's frames: connects as a viewer, rebuilds the screen from the tiles it receives,
 * then, once the stream has been quiet for a second, reads the real screen with XGetImage and compares every pixel.
 * Prints the count of differing pixels and exits 0 only when there are none. Run on the board while the screen
 * changes (flip --animate, windows opening) and after it settles.
 *   cc -O2 mirror_check.c -o mirror_check -lX11 -llz4      ./mirror_check HOST PORT SECONDS */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <arpa/inet.h>
#include <lz4.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int rd(int fd, void *p, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, (char *)p + got, n - got);
		if (r <= 0) return -1;
		got += (size_t)r;
	}
	return 0;
}

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	if (argc < 4) return 2;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(atoi(argv[2])) };
	inet_pton(AF_INET, argv[1], &sa.sin_addr);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa)) { perror("connect"); return 2; }
	unsigned char k; uint32_t magic; uint16_t wh[2];
	if (rd(fd, &k, 1) || k != 'H' || rd(fd, &magic, 4) || rd(fd, wh, 4)) return 2;
	int W = wh[0], H = wh[1];
	unsigned char *fb = calloc((size_t)W * H, 4), tile[64 * 64 * 4];
	char *cb = malloc(LZ4_compressBound(64 * 64 * 4));
	double until = now() + atof(argv[3]), quiet_since = now();
	long frames = 0, tiles = 0;
	while (now() < until || now() - quiet_since < 1.0) {
		struct pollfd p = { fd, POLLIN, 0 };
		if (poll(&p, 1, 100) <= 0) continue;
		if (rd(fd, &k, 1)) return 2;
		if (k == 'P') { uint16_t xy[2]; if (rd(fd, xy, 4)) return 2; continue; }
		if (k == 'C') {
			uint16_t h[4]; if (rd(fd, h, 8)) return 2;
			size_t n = (size_t)h[0] * h[1] * 4; unsigned char *c = malloc(n ? n : 1);
			if (rd(fd, c, n)) return 2;
			free(c); continue;
		}
		if (k != 'F') { fprintf(stderr, "bad message %c\n", k); return 2; }
		uint32_t seq; uint16_t count;
		if (rd(fd, &seq, 4) || rd(fd, &count, 2)) return 2;
		for (int i = 0; i < count; i++) {
			uint16_t t[4]; uint32_t clen;
			if (rd(fd, t, 8) || rd(fd, &clen, 4) || rd(fd, cb, clen)) return 2;
			int x = t[0], y = t[1], w = t[2], h = t[3];
			if (LZ4_decompress_safe(cb, (char *)tile, (int)clen, w * h * 4) != w * h * 4) { fprintf(stderr, "bad tile\n"); return 2; }
			for (int r = 0; r < h; r++)
				memcpy(fb + ((size_t)(y + r) * W + x) * 4, tile + (size_t)r * w * 4, (size_t)w * 4);
		}
		frames++; tiles += count;
		quiet_since = now();
	}
	/* MIRROR_DUMP=FILE also writes the rebuilt screen as a PPM image, which is the only check where no X server runs
	 * (the drm source on the text console) */
	const char *dump = getenv("MIRROR_DUMP");
	if (dump) {
		FILE *f = fopen(dump, "wb");
		fprintf(f, "P6\n%d %d\n255\n", W, H);
		for (size_t i = 0; i < (size_t)W * H; i++) {
			unsigned char rgb[3] = { fb[i * 4 + 2], fb[i * 4 + 1], fb[i * 4] };
			fwrite(rgb, 1, 3, f);
		}
		fclose(f);
	}
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		printf("%ld frames, %ld tiles; no X server to compare with%s\n", frames, tiles, dump ? ", image written" : "");
		return dump ? 0 : 2;
	}
	XImage *img = XGetImage(dpy, DefaultRootWindow(dpy), 0, 0, W, H, AllPlanes, ZPixmap);
	long diff = 0;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			uint32_t a = *(uint32_t *)(fb + ((size_t)y * W + x) * 4) & 0xffffff;
			uint32_t b = (uint32_t)XGetPixel(img, x, y) & 0xffffff;
			diff += a != b;
		}
	printf("%ld frames, %ld tiles, %ld pixels differ of %d\n", frames, tiles, diff, W * H);
	return diff ? 1 : 0;
}
