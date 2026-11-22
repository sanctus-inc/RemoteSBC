/* Latency target for screen/probes/latency.py. Grabs the keyboard and shows a window of the given size whose
 * colour flips on every key press, so a key sent to uinputd changes known pixels. With --animate it flips as fast
 * as X takes the draws instead, for the frame-rate and bandwidth measurement.
 *   cc -O2 flip.c -o flip -lX11      ./flip X Y W H [--animate SECONDS] */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
	if (argc < 5) { fprintf(stderr, "usage: flip X Y W H [--animate SECONDS]\n"); return 2; }
	int x = atoi(argv[1]), y = atoi(argv[2]), w = atoi(argv[3]), h = atoi(argv[4]);
	double animate = argc > 6 && !strcmp(argv[5], "--animate") ? atof(argv[6]) : 0;
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) return 2;
	int scr = DefaultScreen(dpy);
	XSetWindowAttributes a = { .override_redirect = True, .background_pixel = 0 };
	Window win = XCreateWindow(dpy, RootWindow(dpy, scr), x, y, w, h, 0, CopyFromParent, InputOutput,
				   CopyFromParent, CWOverrideRedirect | CWBackPixel, &a);
	XSelectInput(dpy, win, KeyPressMask | ExposureMask);
	XMapRaised(dpy, win);
	GC gc = XCreateGC(dpy, win, 0, NULL);
	XSync(dpy, False);
	for (int i = 0; i < 50 && XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime); i++) {
		struct timespec t = { 0, 20000000 };
		nanosleep(&t, NULL);
	}
	unsigned long colour[2] = { 0x000000, 0xffffff };
	int c = 0;
	XSetForeground(dpy, gc, colour[c]);
	XFillRectangle(dpy, win, gc, 0, 0, w, h);
	XSync(dpy, False);
	printf("ready\n");
	fflush(stdout);
	if (animate) {
		struct timespec t0, t;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		long n = 0;
		do {
			/* a new colour every draw: with two colours the read-back can fall in step with the flips and
			 * see the colour it already sent each time, which counts no frames at all */
			XSetForeground(dpy, gc, (unsigned long)(n * 2654435761u) & 0xffffff);
			XFillRectangle(dpy, win, gc, 0, 0, w, h);
			XSync(dpy, False);
			n++;
			clock_gettime(CLOCK_MONOTONIC, &t);
		} while ((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9 < animate);
		printf("drew %ld flips\n", n);
		return 0;
	}
	for (;;) {
		XEvent e;
		XNextEvent(dpy, &e);
		if (e.type == KeyPress) {
			c ^= 1;
			XSetForeground(dpy, gc, colour[c]);
			XFillRectangle(dpy, win, gc, 0, 0, w, h);
			XFlush(dpy);
		} else if (e.type == Expose) {
			XFillRectangle(dpy, win, gc, 0, 0, w, h);
			XFlush(dpy);
		}
	}
}
