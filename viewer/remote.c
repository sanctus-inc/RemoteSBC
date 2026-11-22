/* remotesbc: SDL window showing the board's HDMI capture, forwarding this
 * machine's mouse and keyboard to a uinput daemon on the board.
 *
 * The capture feed is decoded by an ffmpeg child writing raw YUYV to a pipe,
 * which keeps this program free of libav* linkage.  Input is grabbed only
 * while the window has focus and the user has toggled grab on, so the
 * workstation stays usable.
 *
 * Build: cc -O2 remote.c -o remotesbc $(pkg-config --cflags --libs sdl2)
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_thread.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <lz4.h>

#define APP_NAME "remotesbc"	/* window title and message boxes */

#define CAP_W 1920
#define CAP_H 1080

/* wire protocol: <BHiB> little-endian, matching uinputd.py */
#pragma pack(push, 1)
struct ev { unsigned char kind; unsigned short code; int value; unsigned char pad; };
#pragma pack(pop)

static int sock_fd = -1;

/* Video is read on its own thread.  Sharing the loop with event polling made
 * every mouse motion wait for a whole 4MB frame, which is the bulk of the
 * input lag: at 60fps that is up to 16ms before SDL_PollEvent even runs. */
struct vid {
	FILE *pipe;
	unsigned char *buf[2];	/* double buffer: reader fills one, GPU reads the other */
	size_t sz;
	int ready;		/* index holding a complete frame, -1 if none */
	int writing;
	volatile int stop;
	SDL_mutex *lock;
};

static int vid_thread(void *arg)
{
	struct vid *v = arg;
	while (!v->stop) {
		if (fread(v->buf[v->writing], 1, v->sz, v->pipe) != v->sz)
			break;
		SDL_LockMutex(v->lock);
		int done = v->writing;
		v->writing = v->ready < 0 ? (done + 1) % 2 : v->ready;
		v->ready = done;
		SDL_UnlockMutex(v->lock);
	}
	v->stop = 1;
	return 0;
}

static void send_ev(unsigned char kind, unsigned short code, int value)
{
	if (sock_fd < 0)
		return;
	struct ev e = { kind, code, value, 0 };
	if (write(sock_fd, &e, sizeof e) != sizeof e) {
		fprintf(stderr, "link lost\n");
		close(sock_fd);
		sock_fd = -1;
	}
}

/* SDL scancode -> Linux input keycode.  Only the keys a person actually uses
 * to drive a desktop; anything unmapped is dropped rather than guessed at. */
static unsigned short keymap(SDL_Scancode sc)
{
	switch (sc) {
	case SDL_SCANCODE_ESCAPE: return 1;
	case SDL_SCANCODE_1: return 2;   case SDL_SCANCODE_2: return 3;
	case SDL_SCANCODE_3: return 4;   case SDL_SCANCODE_4: return 5;
	case SDL_SCANCODE_5: return 6;   case SDL_SCANCODE_6: return 7;
	case SDL_SCANCODE_7: return 8;   case SDL_SCANCODE_8: return 9;
	case SDL_SCANCODE_9: return 10;  case SDL_SCANCODE_0: return 11;
	case SDL_SCANCODE_MINUS: return 12; case SDL_SCANCODE_EQUALS: return 13;
	case SDL_SCANCODE_BACKSPACE: return 14; case SDL_SCANCODE_TAB: return 15;
	case SDL_SCANCODE_Q: return 16;  case SDL_SCANCODE_W: return 17;
	case SDL_SCANCODE_E: return 18;  case SDL_SCANCODE_R: return 19;
	case SDL_SCANCODE_T: return 20;  case SDL_SCANCODE_Y: return 21;
	case SDL_SCANCODE_U: return 22;  case SDL_SCANCODE_I: return 23;
	case SDL_SCANCODE_O: return 24;  case SDL_SCANCODE_P: return 25;
	case SDL_SCANCODE_LEFTBRACKET: return 26;
	case SDL_SCANCODE_RIGHTBRACKET: return 27;
	case SDL_SCANCODE_RETURN: return 28;
	case SDL_SCANCODE_LCTRL: return 29;
	case SDL_SCANCODE_A: return 30;  case SDL_SCANCODE_S: return 31;
	case SDL_SCANCODE_D: return 32;  case SDL_SCANCODE_F: return 33;
	case SDL_SCANCODE_G: return 34;  case SDL_SCANCODE_H: return 35;
	case SDL_SCANCODE_J: return 36;  case SDL_SCANCODE_K: return 37;
	case SDL_SCANCODE_L: return 38;
	case SDL_SCANCODE_SEMICOLON: return 39;
	case SDL_SCANCODE_APOSTROPHE: return 40;
	case SDL_SCANCODE_GRAVE: return 41;
	case SDL_SCANCODE_LSHIFT: return 42;
	case SDL_SCANCODE_BACKSLASH: return 43;
	case SDL_SCANCODE_Z: return 44;  case SDL_SCANCODE_X: return 45;
	case SDL_SCANCODE_C: return 46;  case SDL_SCANCODE_V: return 47;
	case SDL_SCANCODE_B: return 48;  case SDL_SCANCODE_N: return 49;
	case SDL_SCANCODE_M: return 50;
	case SDL_SCANCODE_COMMA: return 51; case SDL_SCANCODE_PERIOD: return 52;
	case SDL_SCANCODE_SLASH: return 53;
	case SDL_SCANCODE_RSHIFT: return 54;
	case SDL_SCANCODE_LALT: return 56;
	case SDL_SCANCODE_SPACE: return 57;
	case SDL_SCANCODE_CAPSLOCK: return 58;
	case SDL_SCANCODE_F1: return 59;  case SDL_SCANCODE_F2: return 60;
	case SDL_SCANCODE_F3: return 61;  case SDL_SCANCODE_F4: return 62;
	case SDL_SCANCODE_F5: return 63;  case SDL_SCANCODE_F6: return 64;
	case SDL_SCANCODE_F7: return 65;  case SDL_SCANCODE_F8: return 66;
	case SDL_SCANCODE_F9: return 67;  case SDL_SCANCODE_F10: return 68;
	case SDL_SCANCODE_F11: return 87; case SDL_SCANCODE_F12: return 88;
	case SDL_SCANCODE_HOME: return 102;
	case SDL_SCANCODE_UP: return 103;
	case SDL_SCANCODE_PAGEUP: return 104;
	case SDL_SCANCODE_LEFT: return 105;
	case SDL_SCANCODE_RIGHT: return 106;
	case SDL_SCANCODE_END: return 107;
	case SDL_SCANCODE_DOWN: return 108;
	case SDL_SCANCODE_PAGEDOWN: return 109;
	case SDL_SCANCODE_INSERT: return 110;
	case SDL_SCANCODE_DELETE: return 111;
	case SDL_SCANCODE_RCTRL: return 97;
	case SDL_SCANCODE_RALT: return 100;
	case SDL_SCANCODE_LGUI: return 125;
	default: return 0;
	}
}


/* The network source (screen/screenserve.c on the board): changed tiles of the board's own screen, LZ4 per tile, decoded
 * here into a framebuffer; the main thread uploads the box that changed. Also the board's pointer, which X draws
 * outside the screen image. */
struct net {
	int fd, W, H, stop;
	unsigned char *fb;		/* W*H BGRX */
	int x0, y0, x1, y1;		/* changed box since the last upload; x1 < x0 when none */
	int px, py, moved;
	unsigned char *cur;		/* cursor ARGB, premultiplied */
	int cw, ch, cxh, cyh, cur_new;
	SDL_mutex *lock;
};

static int read_full(int fd, void *p, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, (char *)p + got, n - got);
		if (r <= 0)
			return -1;
		got += (size_t)r;
	}
	return 0;
}

static int net_thread(void *arg)
{
	struct net *n = arg;
	unsigned char kind;
	char *cbuf = NULL;
	size_t ccap = 0;
	unsigned char tile[64 * 64 * 4];
	while (!n->stop && read_full(n->fd, &kind, 1) == 0) {
		if (kind == 'P') {
			int16_t xy[2];
			if (read_full(n->fd, xy, 4)) break;
			SDL_LockMutex(n->lock);
			n->px = xy[0]; n->py = xy[1]; n->moved = 1;
			SDL_UnlockMutex(n->lock);
		} else if (kind == 'C') {
			uint16_t h[4];
			if (read_full(n->fd, h, 8)) break;
			size_t sz = (size_t)h[0] * h[1] * 4;
			unsigned char *c = malloc(sz ? sz : 1);
			if (read_full(n->fd, c, sz)) { free(c); break; }
			SDL_LockMutex(n->lock);
			free(n->cur);
			n->cur = c; n->cw = h[0]; n->ch = h[1]; n->cxh = h[2]; n->cyh = h[3]; n->cur_new = 1;
			SDL_UnlockMutex(n->lock);
		} else if (kind == 'F') {
			uint32_t seq;
			uint16_t count;
			if (read_full(n->fd, &seq, 4) || read_full(n->fd, &count, 2)) break;
			for (int i = 0; i < count; i++) {
				uint16_t t[4];
				uint32_t clen;
				if (read_full(n->fd, t, 8) || read_full(n->fd, &clen, 4)) goto done;
				if (clen > ccap) { ccap = clen; cbuf = realloc(cbuf, ccap); }
				if (read_full(n->fd, cbuf, clen)) goto done;
				int x = t[0], y = t[1], w = t[2], h = t[3];
				if (w > 64 || h > 64 || x + w > n->W || y + h > n->H) goto done;
				if (LZ4_decompress_safe(cbuf, (char *)tile, (int)clen, w * h * 4) != w * h * 4) goto done;
				SDL_LockMutex(n->lock);
				for (int r = 0; r < h; r++)
					memcpy(n->fb + ((size_t)(y + r) * n->W + x) * 4, tile + (size_t)r * w * 4, (size_t)w * 4);
				if (n->x1 < n->x0) { n->x0 = x; n->y0 = y; n->x1 = x + w; n->y1 = y + h; }
				else {
					if (x < n->x0) n->x0 = x;
					if (y < n->y0) n->y0 = y;
					if (x + w > n->x1) n->x1 = x + w;
					if (y + h > n->y1) n->y1 = y + h;
				}
				SDL_UnlockMutex(n->lock);
			}
		} else {
			break;
		}
	}
done:
	free(cbuf);
	n->stop = 1;
	return 0;
}

/* Started from the menu there is no terminal, so a failure to reach the board is shown in a box as well. */
static int fail(const char *fmt, const char *host, int port)
{
	char msg[256];
	snprintf(msg, sizeof msg, fmt, host, port, errno == EAGAIN || errno == EWOULDBLOCK ? "no answer within 3 s" : strerror(errno));
	fprintf(stderr, "%s\n", msg);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP_NAME, msg, NULL);
	return 1;
}

/* A connect or read that waits on the board gives up after 3 s instead of leaving a process with no window. */
static void bounded(int fd, int on)
{
	struct timeval t = { on ? 3 : 0, 0 };
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
}

#include "font.h"
#include <dirent.h>

#define BAR_H 26

/* The capture card's video node: the lowest /dev/videoN whose name says capture and whose index is 0 (the card
 * registers a second, metadata node). The node moves when the card is replugged, so it is looked up, not fixed. */
static const char *find_capture(void)
{
	static char path[64];
	int best = -1;
	DIR *d = opendir("/sys/class/video4linux");
	struct dirent *de;
	while (d && (de = readdir(d))) {
		int n;
		if (sscanf(de->d_name, "video%d", &n) != 1)
			continue;
		char f[300], buf[128] = "";
		snprintf(f, sizeof f, "/sys/class/video4linux/%s/name", de->d_name);
		FILE *fp = fopen(f, "r");
		if (fp) { if (!fgets(buf, sizeof buf, fp)) buf[0] = 0; fclose(fp); }
		if (!strstr(buf, "capture"))
			continue;
		snprintf(f, sizeof f, "/sys/class/video4linux/%s/index", de->d_name);
		int idx = 1;
		fp = fopen(f, "r");
		if (fp) { if (fscanf(fp, "%d", &idx) != 1) idx = 1; fclose(fp); }
		if (idx == 0 && (best < 0 || n < best))
			best = n;
	}
	if (d)
		closedir(d);
	snprintf(path, sizeof path, "/dev/video%d", best < 0 ? 0 : best);
	return path;
}

/* A line of text as a texture, white with the glyphs' coverage as alpha (ASCII only; others show as '?'). */
static SDL_Texture *text_texture(SDL_Renderer *ren, const char *str, int *tw)
{
	int w = 0;
	for (const char *c = str; *c; c++)
		w += font_w[(*c >= 32 && *c < 127 ? *c : '?') - 32];
	SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w > 0 ? w : 1, FONT_H, 32, SDL_PIXELFORMAT_ARGB8888);
	Uint32 *px = sf->pixels;
	int x = 0;
	for (const char *c = str; *c; c++) {
		int g = (*c >= 32 && *c < 127 ? *c : '?') - 32, gw = font_w[g];
		for (int yy = 0; yy < FONT_H; yy++)
			for (int xx = 0; xx < gw; xx++)
				px[yy * (sf->pitch / 4) + x + xx] = ((Uint32)font_px[font_off[g] + yy * gw + xx] << 24) | 0xFFFFFF;
		x += gw;
	}
	SDL_Texture *t = SDL_CreateTextureFromSurface(ren, sf);
	SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
	SDL_FreeSurface(sf);
	*tw = w;
	return t;
}

/* What the status bar says, as in VirtualBox's: what has the keyboard and mouse now, and the key that frees them. */
static const char *status_text(int grabbed)
{
	if (!grabbed)
		return "Workstation has keyboard and mouse  |  Click or focus this window to control the board";
	if (!SDL_GetRelativeMouseMode())
		return "Keyboard -> board  |  Click to send the mouse too  |  Press Right Ctrl to release";
	return "Keyboard and mouse -> board  |  Press Right Ctrl to release them";
}

#define TITLE_FREE APP_NAME " — click or focus to control, Right Ctrl to release"
#define TITLE_HELD APP_NAME " — CONTROLLING (Right Ctrl to release)"

/* Give the mouse and keyboard back to the workstation, and lift every key the board may think is still down. */
static void release(SDL_Window *win, int *grabbed)
{
	*grabbed = 0;
	SDL_SetRelativeMouseMode(SDL_FALSE);
	SDL_SetWindowKeyboardGrab(win, SDL_FALSE);
	SDL_SetWindowTitle(win, TITLE_FREE);
	for (int sc = 0; sc < SDL_NUM_SCANCODES; sc++) {
		unsigned short kc = keymap((SDL_Scancode)sc);
		if (kc)
			send_ev(1, kc, 0);
	}
	for (int b = 0; b < 3; b++)
		send_ev(1, 0x110 + b, 0);
	send_ev(4, 0, 0);
}

int main(int argc, char **argv)
{
	/* remotesbc [--net] [HOST] [INPUT_PORT] [DEVICE]: by default the picture comes from an HDMI capture card, which
	 * also shows boot and crash screens; with --net it is the board's own screen over the network (screenserve, on
	 * INPUT_PORT + 1), which skips the HDMI-capture-USB chain but exists only once the board's desktop runs. HOST
	 * defaults to 127.0.0.1, the end of an SSH tunnel (README, "Security"). */
	int use_net = argc > 1 && !strcmp(argv[1], "--net");
	if (use_net) { argv++; argc--; }
	const char *host = argc > 1 ? argv[1] : "127.0.0.1";
	int port = argc > 2 ? atoi(argv[2]) : 5900;
	const char *dev = argc > 3 ? argv[3] : find_capture();

	signal(SIGPIPE, SIG_IGN);

	/* connect to the board */
	sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
	inet_pton(AF_INET, host, &sa.sin_addr);
	bounded(sock_fd, 1);
	if (connect(sock_fd, (struct sockaddr *)&sa, sizeof sa) < 0)
		return fail("No input link to the board at %s:%d (uinputd): %s", host, port);
	bounded(sock_fd, 0);
	int one = 1;
	setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	printf("connected to %s:%d\n", host, port);

	struct net n = { .fd = -1, .x0 = 1, .x1 = 0 };
	if (use_net) {
		n.fd = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in ss = { .sin_family = AF_INET, .sin_port = htons(port + 1) };
		inet_pton(AF_INET, host, &ss.sin_addr);
		unsigned char k;
		uint32_t magic;
		uint16_t wh[2];
		bounded(n.fd, 1);
		if (connect(n.fd, (struct sockaddr *)&ss, sizeof ss) < 0 || read_full(n.fd, &k, 1) || k != 'H' ||
		    read_full(n.fd, &magic, 4) || magic != 0x32525754 || read_full(n.fd, wh, 4))
			return fail("No picture from the board's screen server at %s:%d (screenserve): %s", host, port + 1);
		bounded(n.fd, 0);
		setsockopt(n.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		n.W = wh[0]; n.H = wh[1];
		n.fb = calloc((size_t)n.W * n.H, 4);
		n.lock = SDL_CreateMutex();
	}

	/* ffmpeg decodes the capture card into raw YUYV on a pipe */
	char cmd[512];
	snprintf(cmd, sizeof cmd,
		 "ffmpeg -hide_banner -loglevel error -f v4l2 "
		 "-input_format yuyv422 -video_size %dx%d -framerate 60 "
		 "-i %s -fflags nobuffer -flags low_delay "
		 "-f rawvideo -pix_fmt yuyv422 - 2>/dev/null",
		 CAP_W, CAP_H, dev);
	FILE *vid = use_net ? NULL : popen(cmd, "r");
	if (!use_net && !vid) { fprintf(stderr, "cannot start ffmpeg\n"); return 1; }

	/* linear filtering: the window is smaller than 1920x1080, and point
	 * sampling turns the downscale into visible blocking */
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Window *win = SDL_CreateWindow(
		TITLE_FREE,
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		1280, 720 + BAR_H, SDL_WINDOW_RESIZABLE);
	SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
	SDL_Texture *tex = use_net
		? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, n.W, n.H)
		: SDL_CreateTexture(ren, SDL_PIXELFORMAT_YUY2, SDL_TEXTUREACCESS_STREAMING, CAP_W, CAP_H);
	SDL_Texture *cursor = NULL;
	int cur_w = 0, cur_h = 0, cur_xh = 0, cur_yh = 0, cur_x = -1000, cur_y = -1000;

	struct vid v = { .pipe = vid, .sz = (size_t)CAP_W * CAP_H * 2,
			 .ready = -1, .writing = 0, .stop = 0 };
	v.buf[0] = malloc(v.sz);
	v.buf[1] = malloc(v.sz);
	v.lock = SDL_CreateMutex();
	SDL_Thread *vt = use_net ? SDL_CreateThread(net_thread, "net", &n) : SDL_CreateThread(vid_thread, "video", &v);

	int running = 1, grabbed = 0, dirty = 1, link_up = -1;
	const char *shown = NULL;
	SDL_Texture *bar = NULL;
	int bar_w = 0;
	/* The release key is VirtualBox's default host key: Right Ctrl, pressed and released alone. It is never
	 * forwarded, so the board does not see it; Right Ctrl with another key is not a release. */
	int rctrl_down = 0, rctrl_combo = 0;

	while (running) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			switch (e.type) {
			case SDL_QUIT:
				running = 0;
				break;
			case SDL_WINDOWEVENT:
				if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_EXPOSED)
					dirty = 1;
				/* focusing the window takes the keyboard; losing focus gives everything back */
				if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED && !grabbed) {
					grabbed = 1;
					SDL_SetWindowKeyboardGrab(win, SDL_TRUE);
					SDL_SetWindowTitle(win, TITLE_HELD);
				} else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && grabbed) {
					release(win, &grabbed);
				}
				break;
			case SDL_MOUSEBUTTONDOWN:
				if (!grabbed || !SDL_GetRelativeMouseMode()) {	/* a click takes the mouse (and the keyboard) */
					grabbed = 1;
					SDL_SetWindowKeyboardGrab(win, SDL_TRUE);
					SDL_SetRelativeMouseMode(SDL_TRUE);
					SDL_SetWindowTitle(win, TITLE_HELD);
					break;
				}
				send_ev(1, 0x110 + (e.button.button == SDL_BUTTON_RIGHT ? 1 :
						    e.button.button == SDL_BUTTON_MIDDLE ? 2 : 0), 1);
				break;
			case SDL_MOUSEBUTTONUP:
				if (grabbed)
					send_ev(1, 0x110 + (e.button.button == SDL_BUTTON_RIGHT ? 1 :
							    e.button.button == SDL_BUTTON_MIDDLE ? 2 : 0), 0);
				break;
			case SDL_MOUSEMOTION:
				if (grabbed) {
					if (e.motion.xrel) send_ev(2, 0, e.motion.xrel);
					if (e.motion.yrel) send_ev(2, 1, e.motion.yrel);
				}
				break;
			case SDL_MOUSEWHEEL:
				if (grabbed && e.wheel.y)
					send_ev(2, 8, e.wheel.y > 0 ? 1 : -1);
				break;
			case SDL_KEYDOWN:
			case SDL_KEYUP: {
				if (e.key.keysym.scancode == SDL_SCANCODE_RCTRL) {
					if (e.key.repeat)
						break;
					if (e.type == SDL_KEYDOWN) {
						rctrl_down = 1;
						rctrl_combo = 0;
					} else {
						if (grabbed && rctrl_down && !rctrl_combo)
							release(win, &grabbed);
						rctrl_down = 0;
					}
					break;	/* never forwarded */
				}
				if (rctrl_down && e.type == SDL_KEYDOWN)
					rctrl_combo = 1;
				if (grabbed) {
					unsigned short kc = keymap(e.key.keysym.scancode);
					if (kc && !e.key.repeat)
						send_ev(1, kc, e.type == SDL_KEYDOWN ? 1 : 0);
				}
				break;
			}
			}
		}

		SDL_LockMutex(v.lock);
		int idx = v.ready;
		v.ready = -1;
		SDL_UnlockMutex(v.lock);

		const char *want = status_text(grabbed);
		if (want != shown || link_up != (sock_fd >= 0)) {
			char line[256];
			link_up = sock_fd >= 0;
			snprintf(line, sizeof line, "%s%s", want, link_up ? "" : "  |  Board input link DOWN");
			if (bar)
				SDL_DestroyTexture(bar);
			bar = text_texture(ren, line, &bar_w);
			shown = want;
			dirty = 1;
		}
		if (idx >= 0) {
			SDL_UpdateTexture(tex, NULL, v.buf[idx], CAP_W * 2);
			dirty = 1;
		}
		if (use_net) {
			SDL_LockMutex(n.lock);
			if (n.x1 > n.x0) {
				SDL_Rect r = { n.x0, n.y0, n.x1 - n.x0, n.y1 - n.y0 };
				SDL_UpdateTexture(tex, &r, n.fb + ((size_t)n.y0 * n.W + n.x0) * 4, n.W * 4);
				n.x0 = 1; n.x1 = 0;
				dirty = 1;
			}
			if (n.cur_new) {
				if (cursor)
					SDL_DestroyTexture(cursor);
				cursor = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, n.cw, n.ch);
				SDL_UpdateTexture(cursor, NULL, n.cur, n.cw * 4);
				SDL_SetTextureBlendMode(cursor, SDL_ComposeCustomBlendMode(
					SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
					SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD));
				cur_w = n.cw; cur_h = n.ch; cur_xh = n.cxh; cur_yh = n.cyh;
				n.cur_new = 0;
				dirty = 1;
			}
			if (n.moved) {
				cur_x = n.px; cur_y = n.py; n.moved = 0;
				dirty = 1;
			}
			SDL_UnlockMutex(n.lock);
		}
		if (dirty) {
			int W, H;
			SDL_GetRendererOutputSize(ren, &W, &H);
			SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
			SDL_RenderClear(ren);
			SDL_Rect video = { 0, 0, W, H > BAR_H ? H - BAR_H : H };
			SDL_RenderCopy(ren, tex, NULL, &video);
			if (use_net && cursor) {	/* the board's pointer, scaled with the picture */
				float sx = (float)video.w / n.W, sy = (float)video.h / n.H;
				SDL_Rect c = { (int)((cur_x - cur_xh) * sx), (int)((cur_y - cur_yh) * sy),
					       (int)(cur_w * sx + 0.5f), (int)(cur_h * sy + 0.5f) };
				SDL_RenderCopy(ren, cursor, NULL, &c);
			}
			SDL_Rect strip = { 0, H - BAR_H, W, BAR_H };
			if (grabbed)
				SDL_SetRenderDrawColor(ren, 20, 70, 140, 255);	/* controlling: blue, as VirtualBox marks capture */
			else
				SDL_SetRenderDrawColor(ren, 45, 45, 45, 255);
			SDL_RenderFillRect(ren, &strip);
			SDL_Rect txt = { 8, H - BAR_H + (BAR_H - FONT_H) / 2, bar_w, FONT_H };
			SDL_RenderCopy(ren, bar, NULL, &txt);
			SDL_RenderPresent(ren);
			dirty = 0;
		} else {
			SDL_Delay(1);	/* no new frame; stay responsive to input */
		}
		if (use_net ? n.stop : v.stop)
			running = 0;
	}

	v.stop = 1;
	n.stop = 1;
	if (use_net)
		shutdown(n.fd, SHUT_RDWR);
	SDL_WaitThread(vt, NULL);
	SDL_DestroyMutex(v.lock);
	free(v.buf[0]);
	free(v.buf[1]);
	if (vid)
		pclose(vid);
	SDL_DestroyTexture(tex);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	if (sock_fd >= 0) close(sock_fd);
	return 0;
}
