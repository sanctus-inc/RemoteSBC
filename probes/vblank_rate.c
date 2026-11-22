/* How often the X server delivers a vblank to Present clients (compositors, GTK) on this board.
 * Asks for a notification at the next MSC (frame counter) ten times and prints the interval.
 * With the display pipe running it is the refresh period (16.7 ms at 60 Hz); with no CRTC
 * active the server falls back to a timer, and this shows that timer's period.
 * cc -O2 vblank_rate.c -o vblank_rate -lX11 -lXext -lXpresent */
#include <X11/Xlib.h>
#include <X11/extensions/Xpresent.h>
#include <stdio.h>
#include <time.h>

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(void)
{
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) { fprintf(stderr, "no display\n"); return 2; }
	int op, ev, err;
	if (!XPresentQueryExtension(dpy, &op, &ev, &err)) { fprintf(stderr, "no Present\n"); return 2; }
	Window w = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0);
	XPresentSelectInput(dpy, w, PresentCompleteNotifyMask);
	double prev = now_ms();
	for (int i = 0; i < 10; i++) {
		XPresentNotifyMSC(dpy, w, 0, 0, 1, 0);	/* the next frame */
		for (;;) {
			XEvent e;
			XNextEvent(dpy, &e);
			if (e.type == GenericEvent) break;
		}
		double t = now_ms();
		printf("vblank %d after %.1f ms\n", i, t - prev);
		prev = t;
	}
	return 0;
}
