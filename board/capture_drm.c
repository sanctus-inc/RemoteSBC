/*
 * screenserve's DRM source: the framebuffer the display controller is showing, read directly. It needs no display
 * server, so it shows the text console and any desktop (X11 or Wayland) alike.
 *
 * The framebuffer on screen is found each tick (the active CRTC's buffer; a compositor that page-flips changes it
 * every frame) and mapped through a dma-buf (GETFB2 gives the buffer's handle to root only). It must be linear, 32 bits
 * per pixel (XRGB8888 or ARGB8888); a tiled or compressed buffer, which some GPUs scan out, is refused with a message.
 *
 * The kernel reports no changes to a reader, so they are found by sampling: each tick reads SAMPLE_ROWS rows of every
 * band of tiles, at an offset that advances each tick, and compares them with what was last read; a tile whose sample
 * differs is marked. Every row is sampled once in TILE / SAMPLE_ROWS ticks, so a change that touches only a few rows
 * is found within that many ticks; a change of more than TILE / SAMPLE_ROWS rows is found at once. Sampling runs at most every SAMPLE_MS (16 ms), so with SAMPLE_ROWS 4 every row is checked within
 * 256 ms. Display memory is often uncached, which is why it is sampled rather than read whole each tick.
 *
 * The pointer: a hardware cursor plane is not read; a pointer drawn into the framebuffer is part of the image.
 */
#include "capture.h"
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define SAMPLE_ROWS 4
#define SAMPLE_MS 16	/* sampling at most this often: the server's loop runs more often than its tick */

struct drm {
	int fd;
	uint32_t crtc, fb_id;
	int buf_fd;
	unsigned char *map;
	size_t map_len;
	int pitch, offset;
	unsigned char *seen;	/* the last pixels read, W*4 per line: what samples are compared against */
	int phase;
	int all_dirty;
	double last_sample;
};

static void drm_unmap(struct drm *d)
{
	if (d->map)
		munmap(d->map, d->map_len);
	if (d->buf_fd >= 0)
		close(d->buf_fd);
	d->map = NULL;
	d->buf_fd = -1;
	d->fb_id = 0;
}

static void sync_dmabuf(struct drm *d, uint64_t flags)
{
	struct dma_buf_sync s = { .flags = flags | DMA_BUF_SYNC_READ };
	ioctl(d->buf_fd, DMA_BUF_IOCTL_SYNC, &s);
}

/* Find the framebuffer on screen and map it if it is a new one. Returns 0, -1 on failure, -2 on a size change. */
static int drm_follow(struct capture *c)
{
	struct drm *d = c->priv;
	drmModeCrtc *crtc = NULL;
	if (d->crtc)
		crtc = drmModeGetCrtc(d->fd, d->crtc);
	if (!crtc || !crtc->buffer_id) {	/* first call, or the CRTC went off: take the first one showing a buffer */
		drmModeFreeCrtc(crtc);
		crtc = NULL;
		drmModeRes *res = drmModeGetResources(d->fd);
		for (int i = 0; res && i < res->count_crtcs && !crtc; i++) {
			drmModeCrtc *k = drmModeGetCrtc(d->fd, res->crtcs[i]);
			if (k && k->buffer_id)
				crtc = k;
			else
				drmModeFreeCrtc(k);
		}
		drmModeFreeResources(res);
		if (!crtc) { fprintf(stderr, "drm: no CRTC is showing a framebuffer\n"); return -1; }
		d->crtc = crtc->crtc_id;
	}
	uint32_t id = crtc->buffer_id;
	drmModeFreeCrtc(crtc);
	if (id == d->fb_id)
		return 0;
	drm_unmap(d);
	drmModeFB2 *fb = drmModeGetFB2(d->fd, id);
	if (!fb) { fprintf(stderr, "drm: GETFB2 failed: %s\n", strerror(errno)); return -1; }
	int ok = 1;
	if (fb->pixel_format != DRM_FORMAT_XRGB8888 && fb->pixel_format != DRM_FORMAT_ARGB8888) {
		fprintf(stderr, "drm: framebuffer format %.4s is not XRGB8888 or ARGB8888\n", (char *)&fb->pixel_format);
		ok = 0;
	} else if ((fb->flags & DRM_MODE_FB_MODIFIERS) && fb->modifier != DRM_FORMAT_MOD_LINEAR) {
		fprintf(stderr, "drm: framebuffer is not linear (modifier 0x%llx)\n", (unsigned long long)fb->modifier);
		ok = 0;
	} else if (!fb->handles[0]) {
		fprintf(stderr, "drm: no buffer handle (screenserve must run as root for the drm source)\n");
		ok = 0;
	}
	if (ok && (c->W && ((int)fb->width != c->W || (int)fb->height != c->H))) {
		fprintf(stderr, "drm: screen size changed to %ux%u\n", fb->width, fb->height);
		drmModeFreeFB2(fb);
		return -2;
	}
	if (ok && drmPrimeHandleToFD(d->fd, fb->handles[0], DRM_CLOEXEC, &d->buf_fd)) {
		fprintf(stderr, "drm: cannot export the framebuffer: %s\n", strerror(errno));
		ok = 0;
	}
	if (ok) {
		c->W = (int)fb->width;
		c->H = (int)fb->height;
		d->pitch = (int)fb->pitches[0];
		d->offset = (int)fb->offsets[0];
		d->map_len = (size_t)d->offset + (size_t)d->pitch * fb->height;
		d->map = mmap(NULL, d->map_len, PROT_READ, MAP_SHARED, d->buf_fd, 0);
		if (d->map == MAP_FAILED) {
			fprintf(stderr, "drm: cannot map the framebuffer: %s\n", strerror(errno));
			d->map = NULL;
			ok = 0;
		}
	}
	for (int i = 0; i < 4; i++)	/* GETFB2 opened a handle per plane; the dma-buf holds the buffer now */
		if (fb->handles[i] && (i == 0 || fb->handles[i] != fb->handles[0]))
			drmCloseBufferHandle(d->fd, fb->handles[i]);
	drmModeFreeFB2(fb);
	if (!ok) {
		drm_unmap(d);
		return -1;
	}
	d->fb_id = id;
	d->all_dirty = 1;	/* another buffer: compare everything */
	return 0;
}

static int drm_changes(struct capture *c, unsigned char *dirty, int TX, int TY, int *cursor_changed)
{
	(void)cursor_changed;
	struct drm *d = c->priv;
	int r = drm_follow(c);
	if (r)
		return -1;
	if (!d->seen)
		d->seen = calloc((size_t)c->W * c->H, 4);
	if (d->all_dirty) {
		memset(dirty, 1, (size_t)TX * TY);
		d->all_dirty = 0;
		return 0;
	}
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	double now = ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
	if (now - d->last_sample < SAMPLE_MS)
		return 0;
	d->last_sample = now;
	sync_dmabuf(d, DMA_BUF_SYNC_START);
	static unsigned char line[16384 * 4];
	for (int ty = 0; ty < TY; ty++)
		for (int k = 0; k < SAMPLE_ROWS; k++) {
			int y = ty * TILE + d->phase + k * (TILE / SAMPLE_ROWS);
			if (y >= c->H)
				break;
			memcpy(line, d->map + d->offset + (size_t)y * d->pitch, (size_t)c->W * 4);	/* one pass over slow memory */
			const unsigned char *was = d->seen + (size_t)y * c->W * 4;
			for (int tx = 0; tx < TX; tx++) {
				int x = tx * TILE, w = x + TILE > c->W ? c->W - x : TILE;
				if (!dirty[ty * TX + tx] && memcmp(line + x * 4, was + x * 4, (size_t)w * 4))
					dirty[ty * TX + tx] = 1;
			}
		}
	sync_dmabuf(d, DMA_BUF_SYNC_END);
	d->phase = (d->phase + 1) % (TILE / SAMPLE_ROWS);
	return 0;
}

static void drm_rearm(struct capture *c)
{
	(void)c;
}

/* Copy the box once out of display memory into `seen` (cached), and serve it from there. */
static const unsigned char *drm_read(struct capture *c, int x, int y, int w, int h, int *bpl)
{
	struct drm *d = c->priv;
	if (!d->map)
		return NULL;
	sync_dmabuf(d, DMA_BUF_SYNC_START);
	for (int r = 0; r < h; r++)
		memcpy(d->seen + ((size_t)(y + r) * c->W + x) * 4, d->map + d->offset + (size_t)(y + r) * d->pitch + (size_t)x * 4,
		       (size_t)w * 4);
	sync_dmabuf(d, DMA_BUF_SYNC_END);
	*bpl = c->W * 4;
	return d->seen + ((size_t)y * c->W + x) * 4;
}

static int drm_pointer(struct capture *c, int *x, int *y)
{
	(void)c; (void)x; (void)y;
	return 0;
}

static uint32_t *drm_cursor(struct capture *c, int *w, int *h, int *xhot, int *yhot)
{
	(void)c; (void)w; (void)h; (void)xhot; (void)yhot;
	return NULL;
}

struct capture *capture_drm(const char *device)
{
	struct drm *d = calloc(1, sizeof *d);
	d->buf_fd = -1;
	d->fd = open(device, O_RDWR | O_CLOEXEC);
	if (d->fd < 0) { fprintf(stderr, "drm: cannot open %s: %s\n", device, strerror(errno)); free(d); return NULL; }
	struct capture *c = calloc(1, sizeof *c);
	c->priv = d;
	c->fd = -1;
	if (drm_follow(c)) { close(d->fd); free(d); free(c); return NULL; }
	if (c->W > 16384) { fprintf(stderr, "drm: screen wider than 16384\n"); return NULL; }
	c->changes = drm_changes;
	c->rearm = drm_rearm;
	c->read = drm_read;
	c->pointer = drm_pointer;
	c->cursor = drm_cursor;
	return c;
}
