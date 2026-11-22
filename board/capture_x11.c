/*
 * screenserve's X11 source: the X server reports changed rectangles (DAMAGE, raw rectangles), so nothing is read
 * while the screen is still; a box is read through shared memory (MIT-SHM), with no copy through the X socket; the
 * pointer's position and image come from XQueryPointer and XFIXES, since X draws the pointer outside the screen image.
 *
 * Reading cost follows where X keeps the screen. On an Orange Pi Zero 3 with Xorg's modesetting driver and no glamor,
 * X drew straight into the uncached display buffer and a full 1920x1080 read took 85 ms; with X's shadow framebuffer
 * (board/20-shadowfb.conf) it took 12 to 20 ms.
 */
#include "capture.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>

struct x11 {
	Display *dpy;
	int scr;
	Window root;
	int dmg_ev, fix_ev;
	Damage damage;
	XShmSegmentInfo shm;
	XImage *box;
};

static int x11_changes(struct capture *c, unsigned char *dirty, int TX, int TY, int *cursor_changed)
{
	struct x11 *x = c->priv;
	while (XPending(x->dpy)) {
		XEvent ev;
		XNextEvent(x->dpy, &ev);
		if (ev.type == x->dmg_ev + XDamageNotify) {
			XDamageNotifyEvent *d = (XDamageNotifyEvent *)&ev;
			capture_mark(dirty, TX, TY, d->area.x, d->area.y, d->area.width, d->area.height);
		} else if (ev.type == x->fix_ev + XFixesCursorNotify) {
			*cursor_changed = 1;
		}
	}
	return 0;
}

static void x11_rearm(struct capture *c)
{
	struct x11 *x = c->priv;
	XDamageSubtract(x->dpy, x->damage, None, None);
}

static const unsigned char *x11_read(struct capture *c, int bx, int by, int w, int h, int *bpl)
{
	struct x11 *x = c->priv;
	if (x->box)
		XFree(x->box);	/* the header only: its pixels are the shared segment */
	x->box = XShmCreateImage(x->dpy, DefaultVisual(x->dpy, x->scr), DefaultDepth(x->dpy, x->scr), ZPixmap,
				 x->shm.shmaddr, &x->shm, w, h);
	if (!x->box || !XShmGetImage(x->dpy, x->root, x->box, bx, by, AllPlanes))
		return NULL;
	*bpl = x->box->bytes_per_line;
	return (const unsigned char *)x->box->data;
}

static int x11_pointer(struct capture *c, int *px, int *py)
{
	struct x11 *x = c->priv;
	Window rr, cr;
	int wx, wy;
	unsigned mask;
	return XQueryPointer(x->dpy, x->root, &rr, &cr, px, py, &wx, &wy, &mask) ? 1 : 0;
}

static uint32_t *x11_cursor(struct capture *c, int *w, int *h, int *xhot, int *yhot)
{
	struct x11 *x = c->priv;
	XFixesCursorImage *ci = XFixesGetCursorImage(x->dpy);
	if (!ci)
		return NULL;
	uint32_t *px = malloc((size_t)ci->width * ci->height * 4 + 4);
	for (int i = 0; i < ci->width * ci->height; i++)
		px[i] = (uint32_t)ci->pixels[i];	/* unsigned long per pixel on 64-bit */
	*w = ci->width; *h = ci->height; *xhot = ci->xhot; *yhot = ci->yhot;
	XFree(ci);
	return px;
}

struct capture *capture_x11(const char *display)
{
	struct x11 *x = calloc(1, sizeof *x);
	x->dpy = XOpenDisplay(display);
	if (!x->dpy) { fprintf(stderr, "x11: cannot open display %s\n", display); free(x); return NULL; }
	x->scr = DefaultScreen(x->dpy);
	x->root = RootWindow(x->dpy, x->scr);
	int err;
	if (!XShmQueryExtension(x->dpy) || !XDamageQueryExtension(x->dpy, &x->dmg_ev, &err) ||
	    !XFixesQueryExtension(x->dpy, &x->fix_ev, &err)) {
		fprintf(stderr, "x11: the X server lacks MIT-SHM, DAMAGE or XFIXES\n");
		return NULL;
	}
	struct capture *c = calloc(1, sizeof *c);
	c->W = DisplayWidth(x->dpy, x->scr);
	c->H = DisplayHeight(x->dpy, x->scr);
	XImage *probe = XShmCreateImage(x->dpy, DefaultVisual(x->dpy, x->scr), DefaultDepth(x->dpy, x->scr), ZPixmap,
					NULL, &x->shm, c->W, c->H);
	if (!probe || probe->bits_per_pixel != 32) {
		fprintf(stderr, "x11: need a 32-bit screen image\n");
		return NULL;
	}
	x->shm.shmid = shmget(IPC_PRIVATE, (size_t)probe->bytes_per_line * c->H, IPC_CREAT | 0600);
	x->shm.shmaddr = shmat(x->shm.shmid, NULL, 0);
	x->shm.readOnly = False;
	XShmAttach(x->dpy, &x->shm);
	shmctl(x->shm.shmid, IPC_RMID, NULL);	/* freed when both sides detach */
	XFree(probe);
	x->damage = XDamageCreate(x->dpy, x->root, XDamageReportRawRectangles);
	XFixesSelectCursorInput(x->dpy, x->root, XFixesDisplayCursorNotifyMask);
	c->fd = ConnectionNumber(x->dpy);
	c->changes = x11_changes;
	c->rearm = x11_rearm;
	c->read = x11_read;
	c->pointer = x11_pointer;
	c->cursor = x11_cursor;
	c->priv = x;
	return c;
}
