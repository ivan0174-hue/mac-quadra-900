/*
 *  video_circle.cpp - Video output for QuadraPi
 *
 *  The Mac draws into a frame buffer inside its own address space. Core 1
 *  converts it (1/2/4/8 bit indexed, 16 or 32 bit direct) to the 32 bit
 *  HDMI frame buffer, with integer scaling, about 60 times per second.
 *  Only lines that changed are converted.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "video.h"
#include "video_defs.h"
#include "platform.h"
#include "gfx.h"
#include "qp.h"

#include <circle/synchronize.h>
#include <circle/timer.h>

#define DEBUG 0
#include "debug.h"

// State shared with the display core
struct shared_mode {
	volatile int w, h;
	volatile int depth;			// video_depth
	volatile uint32 bpr;
	volatile uint32 gen;		// incremented on every change
};
static shared_mode cur;
static volatile uint32 palette32[256];
static volatile uint32 palette_gen = 0;
static volatile uint32 redraw_gen = 0;
static volatile bool menu_visible = false;
static volatile uint32 overlay_gen = 0;

// Overlay used by the menu (drawn by core 0, shown by core 1)
static const int OVERLAY_W = 640, OVERLAY_H = 440;
static canvas overlay;
volatile int overlay_x = 0, overlay_y = 0, overlay_scale = 1;

// Display core private data
static uint8 *shadow = NULL;		// copy of the Mac frame buffer (change detection)
static uint32 *linebuf = NULL;		// one converted line

// Current geometry (display core)
static int g_scale = 1, g_ox = 0, g_oy = 0;

class circle_monitor_desc : public monitor_desc {
public:
	circle_monitor_desc(const vector<video_mode> &available_modes, video_depth default_depth, uint32 default_id)
		: monitor_desc(available_modes, default_depth, default_id) {}

	virtual void switch_to_current_mode(void);
	virtual void set_palette(uint8 *pal, int num);
	virtual void set_gamma(uint8 *gamma, int num) {}
};

static void publish_mode(const video_mode &mode)
{
	cur.w = mode.x;
	cur.h = mode.y;
	cur.depth = mode.depth;
	cur.bpr = mode.bytes_per_row;
	DataSyncBarrier();
	cur.gen = cur.gen + 1;
}

void circle_monitor_desc::switch_to_current_mode(void)
{
	const video_mode &mode = get_current_mode();
	D(bug("switch to %dx%d depth %d\n", mode.x, mode.y, mode.depth));
	memset(MacVRAMHost, 0, mode.bytes_per_row * mode.y);
	set_mac_frame_base(Host2MacAddr(MacVRAMHost));
	publish_mode(mode);
}

void circle_monitor_desc::set_palette(uint8 *pal, int num)
{
	for (int i = 0; i < 256; i++) {
		int c = i % (num > 0 ? num : 1);
		palette32[i] = fbcol(0xFF000000u | (pal[c*3] << 16) | (pal[c*3+1] << 8) | pal[c*3+2]);
	}
	DataSyncBarrier();
	palette_gen = palette_gen + 1;
}


/*
 *  Initialization
 */

static const struct { int w, h, id; } std_modes[] = {
	{  512,  384, 0x80 },
	{  640,  480, 0x81 },
	{  800,  600, 0x82 },
	{  832,  624, 0x83 },
	{ 1024,  768, 0x84 },
	{ 1152,  870, 0x85 },
	{ 1280, 1024, 0x86 },
	{ 1600, 1200, 0x87 },
	{ 0, 0, 0 }
};

static void add_mode(vector<video_mode> &modes, int w, int h, int id, video_depth d)
{
	video_mode m;
	m.x = w;
	m.y = h;
	m.resolution_id = id;
	m.depth = d;
	m.bytes_per_row = TrivialBytesPerRow(w, d);
	m.user_data = 0;
	modes.push_back(m);
}

bool VideoInit(bool classic)
{
	const plat_fb *fb = plat_framebuffer();
	int fbw = fb->width, fbh = fb->height;

	// Buffers for the display core
	if (!shadow)
		shadow = (uint8 *)calloc(1, MacVRAMSize);
	if (!linebuf)
		linebuf = (uint32 *)calloc(1600 * 4 + 64, 1);
	overlay.w = OVERLAY_W;
	overlay.h = OVERLAY_H;
	overlay.pitch = OVERLAY_W;
	if (!overlay.px)
		overlay.px = (uint32_t *)calloc(OVERLAY_W * OVERLAY_H, 4);

	// Requested Mac screen size
	int dw = 640, dh = 480;
	const char *mode_str = PrefsFindString("screen");
	if (mode_str) {
		if (sscanf(mode_str, "win/%d/%d", &dw, &dh) != 2 && sscanf(mode_str, "dga/%d/%d", &dw, &dh) != 2) {
			dw = 640;
			dh = 480;
		}
	}
	if (dw <= 0 || dh <= 0) {	// "native": as large as the HDMI screen
		dw = fbw;
		dh = fbh;
	}
	if (dw > fbw) dw = fbw;
	if (dh > fbh) dh = fbh;
	if (dw > 1600) dw = 1600;
	if (dh > 1200) dh = 1200;
	dw &= ~7;

	// Default depth
	int pref_depth = PrefsFindInt32("displaycolordepth");
	video_depth ddepth = pref_depth ? DepthModeForPixelDepth(pref_depth) : VDEPTH_8BIT;

	// Mode list: the requested size plus all standard sizes below it,
	// each with every depth from 1 to 32 bit
	vector<video_mode> modes;
	int default_id = 0x80;
	bool have_default = false;
	for (int i = 0; std_modes[i].w; i++) {
		if (std_modes[i].w == dw && std_modes[i].h == dh) {
			have_default = true;
			default_id = std_modes[i].id;
		}
		if (std_modes[i].w > dw || std_modes[i].h > dh)
			continue;
		for (int d = VDEPTH_1BIT; d <= VDEPTH_32BIT; d++)
			add_mode(modes, std_modes[i].w, std_modes[i].h, std_modes[i].id, (video_depth)d);
	}
	if (!have_default) {
		default_id = 0x88;	// custom size
		for (int d = VDEPTH_1BIT; d <= VDEPTH_32BIT; d++)
			add_mode(modes, dw, dh, default_id, (video_depth)d);
	}

	circle_monitor_desc *monitor = new circle_monitor_desc(modes, ddepth, default_id);
	VideoMonitors.push_back(monitor);

	const video_mode &mode = monitor->get_current_mode();
	monitor->set_mac_frame_base(Host2MacAddr(MacVRAMHost));
	publish_mode(mode);
	plat_log("Mac screen %dx%d, %d modes", mode.x, mode.y, (int)modes.size());
	return true;
}

void VideoExit(void)
{
}

void VideoQuitFullScreen(void)
{
}

void VideoInterrupt(void)
{
}

void VideoRefresh(void)
{
}


/*
 *  Interface for the menu
 */

void video_force_redraw(void)
{
	DataSyncBarrier();
	redraw_gen = redraw_gen + 1;
}

void video_get_mac_mode(int &w, int &h, int &depth)
{
	w = cur.w;
	h = cur.h;
	static const int bits[] = {1, 2, 4, 8, 16, 32};
	depth = bits[cur.depth];
}

void video_get_scale(int &scale, int &ox, int &oy)
{
	scale = g_scale;
	ox = g_ox;
	oy = g_oy;
}

canvas *video_overlay_canvas(void)
{
	return &overlay;
}

void video_set_menu_visible(bool visible)
{
	const plat_fb *fb = plat_framebuffer();
	int s = 1;
	if ((int)fb->width >= OVERLAY_W * 2 && (int)fb->height >= OVERLAY_H * 2)
		s = 2;
	overlay_scale = s;
	overlay_x = ((int)fb->width - OVERLAY_W * s) / 2;
	overlay_y = ((int)fb->height - OVERLAY_H * s) / 2;
	DataSyncBarrier();
	menu_visible = visible;
	overlay_gen = overlay_gen + 1;
	if (!visible)
		video_force_redraw();
}

void video_overlay_updated(void)
{
	DataSyncBarrier();
	overlay_gen = overlay_gen + 1;
}


/*
 *  Display core (core 1)
 */

static inline uint32 conv16(uint16 be)
{
	uint16 v = __builtin_bswap16(be);
	uint32 r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, b = v & 0x1f;
	r = (r << 3) | (r >> 2);
	g = (g << 3) | (g >> 2);
	b = (b << 3) | (b >> 2);
	if (gfx_swap_rb)
		return 0xFF000000u | (b << 16) | (g << 8) | r;
	return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void convert_line(const uint8 *src, int w, int depth, const uint32 *pal)
{
	uint32 *d = linebuf;
	switch (depth) {
		case VDEPTH_1BIT:
			for (int x = 0; x < w; x += 8) {
				uint8 b = *src++;
				for (int i = 0; i < 8; i++)
					d[x + i] = pal[(b >> (7 - i)) & 1];
			}
			break;
		case VDEPTH_2BIT:
			for (int x = 0; x < w; x += 4) {
				uint8 b = *src++;
				d[x] = pal[b >> 6];
				d[x + 1] = pal[(b >> 4) & 3];
				d[x + 2] = pal[(b >> 2) & 3];
				d[x + 3] = pal[b & 3];
			}
			break;
		case VDEPTH_4BIT:
			for (int x = 0; x < w; x += 2) {
				uint8 b = *src++;
				d[x] = pal[b >> 4];
				d[x + 1] = pal[b & 15];
			}
			break;
		case VDEPTH_8BIT:
			for (int x = 0; x < w; x++)
				d[x] = pal[src[x]];
			break;
		case VDEPTH_16BIT: {
			const uint16 *s = (const uint16 *)src;
			for (int x = 0; x < w; x++)
				d[x] = conv16(s[x]);
			break;
		}
		case VDEPTH_32BIT: {
			const uint32 *s = (const uint32 *)src;
			if (gfx_swap_rb)
				for (int x = 0; x < w; x++)
					d[x] = (s[x] >> 8) | 0xFF000000u;
			else
				for (int x = 0; x < w; x++)
					d[x] = __builtin_bswap32(s[x]) | 0xFF000000u;
			break;
		}
	}
}

static void put_line(const plat_fb *fb, int y, int w)
{
	int s = g_scale;
	uint32 *row = fb->pixels + (g_oy + y * s) * fb->pitch + g_ox;
	if (s == 1) {
		memcpy(row, linebuf, w * 4);
		return;
	}
	uint32 *p = row;
	for (int x = 0; x < w; x++) {
		uint32 c = linebuf[x];
		for (int i = 0; i < s; i++)
			*p++ = c;
	}
	for (int i = 1; i < s; i++)
		memcpy(row + i * fb->pitch, row, w * s * 4);
}

static void draw_overlay(const plat_fb *fb)
{
	int s = overlay_scale;
	for (int y = 0; y < OVERLAY_H; y++) {
		const uint32_t *src = overlay.px + y * OVERLAY_W;
		for (int sy = 0; sy < s; sy++) {
			int yy = overlay_y + y * s + sy;
			if (yy < 0 || yy >= (int)fb->height)
				continue;
			uint32 *row = fb->pixels + yy * fb->pitch + overlay_x;
			if (s == 1)
				for (int x = 0; x < OVERLAY_W; x++)
					row[x] = fbcol(src[x]);
			else
				for (int x = 0; x < OVERLAY_W; x++) {
					uint32 c = fbcol(src[x]);
					row[x * 2] = c;
					row[x * 2 + 1] = c;
				}
		}
	}
}

void video_core1_main(void)
{
	const plat_fb *fb = plat_framebuffer();
	uint32 seen_mode = 0, seen_pal = 0, seen_redraw = 0, seen_overlay = 0;
	bool seen_menu = false;
	uint32 pal[256];
	uint64 next = CTimer::GetClockTicks64();

	for (;;) {
		// ~60 frames per second
		while (CTimer::GetClockTicks64() < next)
			;
		next += 16667;
		if (CTimer::GetClockTicks64() > next + 100000)
			next = CTimer::GetClockTicks64();

		if (gfx_core0_owns_screen || !shadow || cur.gen == 0)
			continue;

		bool full = false;
		DataMemBarrier();

		if (cur.gen != seen_mode) {
			seen_mode = cur.gen;
			int w = cur.w, h = cur.h;
			int s = 1;
			while ((w * (s + 1)) <= (int)fb->width && (h * (s + 1)) <= (int)fb->height)
				s++;
			g_scale = s;
			g_ox = ((int)fb->width - w * s) / 2;
			g_oy = ((int)fb->height - h * s) / 2;
			for (unsigned y = 0; y < fb->height; y++)
				memset(fb->pixels + y * fb->pitch, 0, fb->width * 4);
			full = true;
		}
		if (palette_gen != seen_pal) {
			seen_pal = palette_gen;
			for (int i = 0; i < 256; i++)
				pal[i] = palette32[i];
			full = true;
		}
		if (redraw_gen != seen_redraw) {
			seen_redraw = redraw_gen;
			full = true;
		}

		bool mv = menu_visible;
		if (mv) {
			// Emulation is paused: just keep the overlay up to date
			if (overlay_gen != seen_overlay || !seen_menu) {
				seen_overlay = overlay_gen;
				seen_menu = true;
				draw_overlay(fb);
			}
			continue;
		}
		if (seen_menu) {
			seen_menu = false;
			for (unsigned y = 0; y < fb->height; y++)
				memset(fb->pixels + y * fb->pitch, 0, fb->width * 4);
			full = true;
		}

		int w = cur.w, h = cur.h, depth = cur.depth;
		uint32 bpr = cur.bpr;
		if (bpr * h > MacVRAMSize)
			continue;
		const uint8 *src = MacVRAMHost;
		for (int y = 0; y < h; y++) {
			const uint8 *line = src + y * bpr;
			uint8 *sh = shadow + y * bpr;
			if (!full && memcmp(line, sh, bpr) == 0)
				continue;
			memcpy(sh, line, bpr);
			convert_line(sh, w, depth, pal);
			put_line(fb, y, w);
		}
	}
}
