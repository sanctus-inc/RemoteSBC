/*
 * Where screenserve's pixels come from. A source knows the screen's size, which tiles changed, how to read a box of
 * pixels as 32-bit BGRX, and, if it can, where the pointer is and what it looks like. Everything else (tiles, the
 * comparison with what the viewer holds, LZ4, the network) is the server's and does not depend on the source.
 *
 *   x11  an X11 display (":0"): the X server reports changes (DAMAGE) and the pointer (XFIXES)
 *   drm  a DRM device ("/dev/dri/card0"): the framebuffer on screen, read directly; works with no desktop (the text
 *        console) and under any display server, but the kernel reports no changes to a reader, so they are found by
 *        sampling rows; needs root, and a linear 32-bit framebuffer
 */
#ifndef CAPTURE_H
#define CAPTURE_H
#include <stdint.h>

#define TILE 64

struct capture {
	int W, H;
	/* a descriptor that turns readable when the source has news, or -1 when changes are found only by `changes` */
	int fd;
	/* Mark in dirty[ty * TX + tx] every tile that changed since the last call; set *cursor_changed when the pointer's
	 * image changed. Returns 0, or -1 when the source is gone or its size changed (the server then exits so its
	 * supervisor restarts it with the new size). */
	int (*changes)(struct capture *c, unsigned char *dirty, int TX, int TY, int *cursor_changed);
	/* Called before the dirty tiles' box is read, so changes made during the read are reported next time. */
	void (*rearm)(struct capture *c);
	/* Read the box at (x, y), w by h, as 32-bit BGRX. Returns its first pixel and sets *bpl to the bytes per line;
	 * valid until the next call. NULL when the read failed. */
	const unsigned char *(*read)(struct capture *c, int x, int y, int w, int h, int *bpl);
	/* The pointer's position: 1 when known, 0 when this source has no pointer. */
	int (*pointer)(struct capture *c, int *x, int *y);
	/* The pointer's image, w*h premultiplied ARGB, malloc'd; NULL when there is none. */
	uint32_t *(*cursor)(struct capture *c, int *w, int *h, int *xhot, int *yhot);
	void *priv;
};

/* Each returns NULL, with a message on stderr, when the source cannot be opened. */
struct capture *capture_x11(const char *display);
struct capture *capture_drm(const char *device);

/* Mark the tiles a rectangle touches. */
static inline void capture_mark(unsigned char *dirty, int TX, int TY, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	int x0 = x / TILE, y0 = y / TILE, x1 = (x + w - 1) / TILE, y1 = (y + h - 1) / TILE;
	for (int ty = y0 < 0 ? 0 : y0; ty <= y1 && ty < TY; ty++)
		for (int tx = x0 < 0 ? 0 : x0; tx <= x1 && tx < TX; tx++)
			dirty[ty * TX + tx] = 1;
}
#endif
