#include <err.h>
#include <poll.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <mpd/client.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/xpm.h>
#include <X11/extensions/Xrender.h>

#ifdef USE_STB
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize.h"
#endif

#include "buttons.xpm"
#include "album.data"

#define FONT            "-misc-fixed-*-*-*-*-*-*-*-*-*-*-*-*"
#define UNKNOWN_TITLE   "(?)"
#define SHADE           0xFF
#define ALPHA           0x37    /* value empirically chosen */
#define JPEGMAGICSIZE   4
#define WINSIZE         58
#define BORDER          1
#define IMGSIZE         (WINSIZE - 2 * BORDER)
#define DEFCLASS        "WMPDArt"
#define SCROLLINC       4
#define BUFSIZE         8192
#define UPDATE_TIME     250
#define TITLEMAXLEN     1024
#define NCHANNELS       4

enum { POLL_X11, POLL_MPD, POLL_LAST };
enum { BG_NORMAL, BG_HOVERED, BG_LAST };

static struct mpd_connection *mpd;
static struct pollfd pfds[POLL_LAST];
static size_t titlelen = 0;
static XFontSet fontset;
static Display *dpy;
static Window root, win;
static Pixmap savepix[BG_LAST];
static Pixmap nopix[BG_LAST];
static Pixmap pix[BG_LAST] = {None, None};
static Pixmap btnpix = None, btnmask = None;
static GC gc, btngc;
static int btnw, btnh, btnx, btny;
static int fonth, textw, scroll = 0;
static int screen;
static int hovered = 0;
static char title[TITLEMAXLEN];
static enum mpd_state state;

static void *
emalloc(size_t size)
{
	void *p;

	if ((p = malloc(size)) == NULL)
		err(1, "malloc");
	return p;
}

static void *
erealloc(void *ptr, size_t size)
{
	void *p;

	if ((p = realloc(ptr, size)) == NULL)
		err(1, "realloc");
	return p;
}

static void *
ecalloc(size_t nmemb, size_t size)
{
	void *p;
	if ((p = calloc(nmemb, size)) == NULL)
		err(1, "calloc");
	return p;
}

static void
mpderr(void)
{
	warnx("mpd: %s", mpd_connection_get_error_message(mpd));
	mpd_connection_free(mpd);
	exit(1);
}

static int
isjpg(const unsigned char *data, size_t size)
{
	static unsigned char mask[JPEGMAGICSIZE] = { 0xFF, 0xFF, 0xFF, 0xF0 };
	static unsigned char bits[JPEGMAGICSIZE] = { 0xFF, 0xD8, 0xFF, 0xE0 };
	int i;
	
	if (size < JPEGMAGICSIZE)
		return 0;
	for (i = 0; i < JPEGMAGICSIZE; i++)
		if ((data[i] & mask[i]) != bits[i])
			return 0;
	return 1;
}

static void
commitpixmap(void)
{
	XCopyArea(dpy, pix[hovered], win, gc, 0, 0, WINSIZE, WINSIZE, 0, 0);
}

static unsigned char *
uncompress(unsigned char *src, size_t size, int *w, int *h, int *c)
{
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;
	unsigned char *dst, *b[1];

	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, src, size);
	jpeg_read_header(&cinfo, TRUE);
	cinfo.out_color_space = JCS_EXT_BGRX;   /* XImage uses BGRA */
	jpeg_start_decompress(&cinfo);
	*w = cinfo.output_width;
	*h = cinfo.output_height;
	*c = cinfo.output_components;
	dst = ecalloc(cinfo.output_width * cinfo.output_height, cinfo.output_components);
	while (cinfo.output_scanline < cinfo.output_height) {
		*b = &dst[cinfo.output_scanline * cinfo.output_width * cinfo.output_components],
		jpeg_read_scanlines(&cinfo, b, 1);
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	free(src);
	return dst;
}

static unsigned char *
downsample(unsigned char *src, int sw, int sh, int c, int *dw, int *dh)
{
	unsigned char *dst;

	if (src == NULL)
		return NULL;
	/* scale to fit */
	if (sw > sh) {
		*dw = IMGSIZE;
		*dh = (sh * IMGSIZE) / sw;
	} else {
		*dw = (sw * IMGSIZE) / sh;
		*dh = IMGSIZE;
	}
	dst = ecalloc((*dw) * (*dh), c);
#ifdef USE_STB
	stbir_resize_uint8(src, sw, sh, 0, dst, *dw, *dh, 0, c);
#else
	double xr, yr;
	int i, j, k, x, y;

	/* scale image down with nearest-neighbor interpolation (thanks anon) */
	xr = sw / *dw;
	yr = sh / *dh;
	for (i = 0; i < *dw; i++) {
		x = i * xr;
		for (j = 0; j < *dh; j++) {
			y = j * yr;
			for (k = 0; k < c; k++) {
				dst[i * (*dw) * c + j * c + k] = src[x * sw * c + y * c + k];
			}
		}
	}
#endif
	free(src);
	return dst;
}

static void
applyshade(unsigned char *buf, int w, int h, int c, int f)
{
	int i, j, k, from, to;

	from = f ? fonth : 0;
	to = f ? w : fonth;
	for (i = from; i < to; i++) {
		for (j = 0; j < h; j++) {
			k = i * w * c + j * c;
			buf[k + 0] = (SHADE * 255 / USHRT_MAX) + ((buf[k + 0] - (SHADE * 255 / USHRT_MAX)) * ALPHA) / 255;
			buf[k + 1] = (SHADE * 255 / USHRT_MAX) + ((buf[k + 1] - (SHADE * 255 / USHRT_MAX)) * ALPHA) / 255;
			buf[k + 2] = (SHADE * 255 / USHRT_MAX) + ((buf[k + 2] - (SHADE * 255 / USHRT_MAX)) * ALPHA) / 255;
		}
	}
}

static void
copypixmap(XImage *image, Pixmap pix, unsigned char *src, int w, int h)
{
	XGCValues val;

	image->data = src;

	/* draw cover on pixmap */
	XPutImage(
		dpy, pix,
		gc, image,
		0, 0,
		BORDER + (WINSIZE - w) / 2,
		BORDER + (WINSIZE - h) / 2,
		w, h
	);

	/* draw black shadow */
	val.foreground = BlackPixel(dpy, screen);
	XChangeGC(dpy, gc, GCForeground, &val);
	XFillRectangles(
		dpy, pix, gc,
		(XRectangle []){
			[0].x = 0,
			[1].x = 0,
			[0].y = 0,
			[1].y = 0,
			[0].width  = WINSIZE,
			[1].width  = BORDER,
			[0].height = BORDER,
			[1].height = WINSIZE,
		},
		2
	);

	/* draw white shadow */
	val.foreground = WhitePixel(dpy, screen);
	XChangeGC(dpy, gc, GCForeground, &val);
	XFillRectangles(
		dpy, pix, gc,
		(XRectangle []){
			[0].x = 0,
			[1].x = WINSIZE - BORDER,
			[0].y = WINSIZE - BORDER,
			[1].y = 0,
			[0].width  = WINSIZE,
			[1].width  = BORDER,
			[0].height = BORDER,
			[1].height = WINSIZE,
		},
		2
	);
}

static void
drawbuttons(Pixmap pix)
{
	XGCValues val;

	/* draw left button */
	val.clip_x_origin = btnx;
	val.clip_y_origin = WINSIZE - btny - btnh;
	XChangeGC(dpy, btngc, GCForeground, &val);
	XCopyArea(
		dpy,
		btnpix, pix,
		btngc,
		0, 0,
		btnw, btnh,
		btnx, WINSIZE - btny - btnh
	);

	/* draw right button */
	val.clip_x_origin = WINSIZE - btnx - btnw;
	val.clip_y_origin = WINSIZE - btny - btnh;
	XChangeGC(dpy, btngc, GCForeground, &val);
	XCopyArea(
		dpy,
		btnpix, pix,
		btngc,
		btnw, 0,
		btnw, btnh,
		WINSIZE - btnx - btnw, WINSIZE - btny - btnh
	);
}

static void
coverfromjpg(unsigned char *buf, size_t size)
{
	XImage *image;
	int sw, sh, dw, dh;
	int c;

	buf = uncompress(buf, size, &sw, &sh, &c);
	buf = downsample(buf, sw, sh, c, &dw, &dh);
	image = XCreateImage(
		dpy,
		DefaultVisual(dpy, screen),
		DefaultDepth(dpy, screen),
		ZPixmap, 0,
		NULL,
		dw, dh,
		8 * c,
		0
	);
	applyshade(buf, dw, dh, c, 0);
	copypixmap(image, savepix[BG_NORMAL], buf, dw, dh);
	applyshade(buf, dw, dh, c, 1);
	copypixmap(image, savepix[BG_HOVERED], buf, dw, dh);
	XDestroyImage(image);
	drawbuttons(savepix[BG_HOVERED]);
	commitpixmap();
	XFlush(dpy);
}

static void
coverunknown(void)
{
	XCopyArea(dpy, nopix[BG_NORMAL], savepix[BG_NORMAL], gc, 0, 0, WINSIZE, WINSIZE, 0, 0);
	XCopyArea(dpy, nopix[BG_HOVERED], savepix[BG_HOVERED], gc, 0, 0, WINSIZE, WINSIZE, 0, 0);
}

static void
bgtopix(void)
{
	XCopyArea(dpy, savepix[BG_NORMAL], pix[BG_NORMAL], gc, 0, 0, WINSIZE, WINSIZE, 0, 0);
	XCopyArea(dpy, savepix[BG_HOVERED], pix[BG_HOVERED], gc, 0, 0, WINSIZE, WINSIZE, 0, 0);
}

static void
setalbum(const char *uri)
{
	size_t bufsize, offset;
	int n;
	unsigned char *buf;

	bufsize = BUFSIZE;
	offset = 0;
	buf = emalloc(BUFSIZE);
	for (;;) {
		if ((n = mpd_run_albumart(mpd, uri, offset, buf + offset, BUFSIZE)) == -1) {
			if (!mpd_connection_clear_error(mpd))
				mpderr();
			goto nocover;
		}
		if (n == 0)
			break;
		offset += n;
		if (offset >= bufsize) {
			bufsize += BUFSIZE;
			buf = erealloc(buf, bufsize);
		}
	}
	if (isjpg(buf, offset)) {
		coverfromjpg(buf, offset);
	} else {
nocover:
		free(buf);
		coverunknown();
	}
	bgtopix();
	return;
}

static void
settitle(struct mpd_song *song)
{
	XRectangle box, dummy;
	const char *s;

	s = mpd_song_get_tag(song, MPD_TAG_TITLE, 0);
	if (s != NULL) {
		snprintf(
			title, TITLEMAXLEN,
			"%s - %s",
			mpd_song_get_tag(song, MPD_TAG_ARTIST, 0),
			mpd_song_get_tag(song, MPD_TAG_TITLE, 0)
		);
	} else {
		snprintf(title, TITLEMAXLEN, "%s", UNKNOWN_TITLE);
	}
	titlelen = strlen(title);
	XmbTextExtents(fontset, title, titlelen, &dummy, &box);
	textw = box.width;
}

static void
updatesong(void)
{
	struct mpd_song *song;
	const char *uri;
 
 	scroll = 0;
	if ((song = mpd_run_current_song(mpd)) == NULL)
		mpderr();
	uri = mpd_song_get_uri(song);
	setalbum(uri);
	settitle(song);
	mpd_song_free(song);
}

static void
mpdevent(void)
{
	static int once = 0;
	static int songid;
	static unsigned int queue;

	struct mpd_status *status;
	enum mpd_state s;
	unsigned int q;
	int n;

	if ((status = mpd_run_status(mpd)) == NULL)
		mpderr();
	s = mpd_status_get_state(status);
	q = mpd_status_get_queue_version(status);
	n = mpd_status_get_song_id(status);
	state = s;
	if ((!once || n != songid || q != queue) && (state & MPD_STATE_PLAY))
		updatesong();
	queue = q;
	songid = n;
	mpd_status_free(status);
	once = 1;
}

static void
showbuttons(void)
{
	if (state != MPD_STATE_PLAY)
		return;
	hovered = BG_HOVERED;
	commitpixmap();
}

static void
hidebuttons(void)
{
	if (state != MPD_STATE_PLAY)
		return;
	hovered = BG_NORMAL;
	commitpixmap();
}

static void
pressbutton(int x, int y)
{
	mpd_recv_idle(mpd, false);
	if (x < WINSIZE / 2 && y >= WINSIZE - btny - btnh)
		mpd_run_previous(mpd);
	else if (x >= WINSIZE / 2 && y >= WINSIZE - btny - btnh)
		mpd_run_next(mpd);
	else
		mpd_run_pause(mpd, state == MPD_STATE_PLAY);
	mpd_send_idle(mpd);
}

static void
drawtitle()
{
	XGCValues val;
	int i;

	for (i = 0; i < BG_LAST; i++) {
		if (pix[i] == None || titlelen == 0)
			return;

		/* draw saved bg */
		XCopyArea(dpy, savepix[i], pix[i], gc, 0, 0, WINSIZE, WINSIZE, 0, 0);

		/* draw text */
		XmbDrawString(dpy, pix[i], fontset, gc, WINSIZE - scroll, fonth, title, titlelen);

		/* draw black shadow */
		val.foreground = BlackPixel(dpy, screen);
		XChangeGC(dpy, gc, GCForeground, &val);
		XFillRectangles(
			dpy, pix[i], gc,
			(XRectangle []){
				[0].x = 0,
				[1].x = 0,
				[0].y = 0,
				[1].y = 0,
				[0].width  = WINSIZE,
				[1].width  = BORDER,
				[0].height = BORDER,
				[1].height = WINSIZE,
			},
			2
		);

		/* draw white shadow */
		val.foreground = WhitePixel(dpy, screen);
		XChangeGC(dpy, gc, GCForeground, &val);
		XFillRectangles(
			dpy, pix[i], gc,
			(XRectangle []){
				[0].x = 0,
				[1].x = WINSIZE - BORDER,
				[0].y = WINSIZE - BORDER,
				[1].y = 0,
				[0].width  = WINSIZE,
				[1].width  = BORDER,
				[0].height = BORDER,
				[1].height = WINSIZE,
			},
			2
		);
	}

	if (scroll >= WINSIZE + textw)
		scroll = 0;
	else
		scroll += SCROLLINC;

	commitpixmap();
	XFlush(dpy);
}

static void
xevent(void)
{
	XEvent ev;

	while (XPending(dpy) && !XNextEvent(dpy, &ev)) {
		switch (ev.type) {
		case Expose:
			if (ev.xexpose.count == 0)
				commitpixmap();
			break;
		case ButtonPress:
			pressbutton(ev.xbutton.x, ev.xbutton.y);
			break;
		case EnterNotify:
			showbuttons();
			break;
		case LeaveNotify:
			hidebuttons();
			break;
		case ConfigureNotify:
			break;
		}
	}
}

static void
run(void)
{
	int idleflag, handleidle;
	int ret;

	idleflag = 1;
	hovered = BG_NORMAL;
	while ((ret = poll(pfds, POLL_LAST, (state == MPD_STATE_PLAY ? UPDATE_TIME : -1))) != -1) {
		handleidle = 0;
		if (!(pfds[POLL_MPD].revents & POLLIN)
		   && (pfds[POLL_X11].revents & POLLIN)) {
			mpd_send_noidle(mpd);
			handleidle = 1;
		}
		if (pfds[POLL_X11].revents & POLLIN) {
			xevent();
		}
		if (pfds[POLL_MPD].revents & POLLIN || handleidle) {
			idleflag = 0;
			if (mpd_recv_idle(mpd, false)) {
				mpdevent();
			}
		}
		if (ret == 0) {
			drawtitle();
		}
		if ((pfds[POLL_X11].revents | pfds[POLL_MPD].revents) & POLLHUP) {
			break;
		}
		if (!idleflag) {
			mpd_send_idle(mpd);
			idleflag = 1;
		}
	}
	if (ret == -1)
		err(1, "poll");
}

static void
initx(int argc, char *argv[])
{
	int i;
	char *title;

	if ((dpy = XOpenDisplay(NULL)) == NULL)
		errx(1, "could not open display");
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	if ((title = strrchr(argv[0], '/')) != NULL)
		title++;
	else
		title = argv[0];
	win = XCreateWindow(
		dpy, root,
		0, 0, WINSIZE, WINSIZE, 0,
		CopyFromParent, CopyFromParent, CopyFromParent,
		CWEventMask,
		&(XSetWindowAttributes){
			.event_mask = ExposureMask | ButtonPressMask
			            | EnterWindowMask | LeaveWindowMask
			            | StructureNotifyMask,
		}
	);
	for (i = 0; i < BG_LAST; i++) {
		savepix[i] = XCreatePixmap(dpy, win, WINSIZE, WINSIZE, DefaultDepth(dpy, screen));
		nopix[i] = XCreatePixmap(dpy, win, WINSIZE, WINSIZE, DefaultDepth(dpy, screen));
		pix[i] = XCreatePixmap(dpy, win, WINSIZE, WINSIZE, DefaultDepth(dpy, screen));
	}
	gc = XCreateGC(dpy, root, 0, NULL);
	XmbSetWMProperties(
		dpy, win,
		title, title,
		argv, argc,
		&(XSizeHints){
			.flags = PMaxSize | PMinSize,
			.min_width = WINSIZE,
			.max_width = WINSIZE,
			.min_height = WINSIZE,
			.max_height = WINSIZE,
		},
		&(XWMHints){
			.flags = IconWindowHint | StateHint | WindowGroupHint,
			.initial_state = WithdrawnState,
			.window_group = win,
			.icon_window = win,
		},
		&(XClassHint){
			.res_class = DEFCLASS,
			.res_name = title,
		}
	);
	XMapWindow(dpy, win);
	XFlush(dpy);
}

static void
initmpd(void)
{
	if ((mpd = mpd_connection_new(NULL, 0, 0)) == NULL)
		errx(1, "could not connect to mpd");
	if (mpd_connection_get_error(mpd) != MPD_ERROR_SUCCESS)
		mpderr();
	mpdevent();
	mpd_send_idle(mpd);
}

static void
initpoll(void)
{
	pfds[POLL_X11] = (struct pollfd){
		.fd = ConnectionNumber(dpy),
		.events = POLLIN,
	};
	pfds[POLL_MPD] = (struct pollfd){
		.fd = mpd_connection_get_fd(mpd),
		.events = POLLIN,
	};
}

static void
initbuttons(void)
{
	XpmAttributes xa;

	memset(&xa, 0, sizeof(xa));
	if (XpmCreatePixmapFromData(dpy, root, buttons_xpm, &btnpix, &btnmask, &xa) != XpmSuccess)
		errx(1, "could not load xpm");
	if (!(xa.valuemask & (XpmSize | XpmHotspot)))
		errx(1, "could not load xpm");
	btngc = XCreateGC(
		dpy,
		root,
		GCClipYOrigin,
		&(XGCValues){
			.clip_mask = btnmask,
		}
	);
	btnw = xa.width / 2;
	btnh = xa.height;
	btnx = xa.x_hotspot;
	btny = xa.y_hotspot;
}

static void
initalbum(void)
{
	XImage *image;

	image = XCreateImage(
		dpy,
		DefaultVisual(dpy, screen),
		DefaultDepth(dpy, screen),
		ZPixmap, 0,
		NULL,
		IMGSIZE, IMGSIZE,
		8 * NCHANNELS,
		0
	);
	applyshade(album_data, IMGSIZE, IMGSIZE, NCHANNELS, 0);
	copypixmap(image, nopix[BG_NORMAL], album_data, IMGSIZE, IMGSIZE);
	applyshade(album_data, IMGSIZE, IMGSIZE, NCHANNELS, 1);
	copypixmap(image, nopix[BG_HOVERED], album_data, IMGSIZE, IMGSIZE);
	drawbuttons(nopix[BG_HOVERED]);
	image->data = NULL;
	XDestroyImage(image);
}

static void
initfont(void)
{
	XFontSetExtents *ext;
	char **mc;      /* dummy variable; allocated array of missing charsets */
	int nmc;        /* dummy variable; number of missing charsets */
	char *ds;       /* dummy variable; default string drawn in place of unknown chars */

	if ((fontset = XCreateFontSet(dpy, FONT, &mc, &nmc, &ds)) == NULL)
		errx(1, "XCreateFontSet: could not create fontset");
	XFreeStringList(mc);
	ext = XExtentsOfFontSet(fontset);
	fonth = ext->max_ink_extent.height;
}

int
main(int argc, char *argv[])
{
	initx(argc, argv);
	initbuttons();
	initfont();
	initalbum();
	initmpd();
	initpoll();
	run();
	return 0;
}
