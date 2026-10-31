/*
 *  gfx.cpp - Minimal 32 bpp drawing helpers
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "gfx.h"
#include <circle/chargenerator.h>
#include <circle/synchronize.h>
#include <string.h>

static canvas s_screen;
static const plat_fb *s_fb = 0;
volatile bool gfx_core0_owns_screen = false;
bool gfx_swap_rb = false;

static CCharGenerator &chargen(void)
{
	static CCharGenerator s_CharGen(Font8x16);
	return s_CharGen;
}

// Decode one UTF-8 sequence to a Latin-1 code (unknown -> '?')
static unsigned next_char(const char *&p)
{
	unsigned char c = (unsigned char)*p++;
	if (c < 0x80)
		return c;
	unsigned cp;
	int extra;
	if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
	else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
	else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
	else return '?';
	while (extra-- > 0) {
		unsigned char d = (unsigned char)*p;
		if ((d & 0xC0) != 0x80)
			return '?';
		cp = (cp << 6) | (d & 0x3F);
		p++;
	}
	if (cp == 0x2192) return 0xBB;	// right arrow -> >>
	if (cp == 0x2190) return 0xAB;	// left arrow  -> <<
	if (cp == 0x2026) return '.';	// ellipsis
	if (cp == 0x2019 || cp == 0x2018) return '\'';
	return cp <= 0xFF ? cp : '?';
}

void cv_fill(canvas *c, int x, int y, int w, int h, uint32_t color)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > c->w) w = c->w - x;
	if (y + h > c->h) h = c->h - y;
	if (w <= 0 || h <= 0)
		return;
	for (int j = 0; j < h; j++) {
		uint32_t *p = c->px + (y + j) * c->pitch + x;
		for (int i = 0; i < w; i++)
			p[i] = color;
	}
}

void cv_rect(canvas *c, int x, int y, int w, int h, uint32_t color)
{
	cv_fill(c, x, y, w, 1, color);
	cv_fill(c, x, y + h - 1, w, 1, color);
	cv_fill(c, x, y, 1, h, color);
	cv_fill(c, x + w - 1, y, 1, h, color);
}

int text_width(const char *utf8, int scale)
{
	int n = 0;
	const char *p = utf8;
	while (*p) {
		next_char(p);
		n++;
	}
	return n * FONT_W * scale;
}

int cv_text(canvas *c, int x, int y, const char *utf8, uint32_t color, int scale)
{
	CCharGenerator &cg = chargen();
	const char *p = utf8;
	int x0 = x;
	while (*p) {
		unsigned ch = next_char(p);
		if (ch > ' ' && ch != 0x7F && ch != 0xA0) {
			for (int row = 0; row < FONT_H; row++) {
				CCharGenerator::TPixelLine line = cg.GetPixelLine((char)ch, row);
				for (int col = 0; col < FONT_W; col++) {
					if (!cg.GetPixel(col, line))
						continue;
					int px = x + col * scale, py = y + row * scale;
					for (int sy = 0; sy < scale; sy++) {
						int yy = py + sy;
						if (yy < 0 || yy >= c->h)
							continue;
						for (int sx = 0; sx < scale; sx++) {
							int xx = px + sx;
							if (xx >= 0 && xx < c->w)
								c->px[yy * c->pitch + xx] = color;
						}
					}
				}
			}
		}
		x += FONT_W * scale;
	}
	return x - x0;
}

void gfx_init(const plat_fb *fb)
{
	s_fb = fb;
	s_screen.px = fb->pixels;
	s_screen.w = fb->width;
	s_screen.h = fb->height;
	s_screen.pitch = fb->pitch;
}

const plat_fb *plat_framebuffer(void)
{
	return s_fb;
}

canvas *gfx_screen(void)
{
	return &s_screen;
}

unsigned gfx_width(void)
{
	return s_screen.w;
}

unsigned gfx_height(void)
{
	return s_screen.h;
}

void gfx_clear(uint32_t color)
{
	cv_fill(&s_screen, 0, 0, s_screen.w, s_screen.h, fbcol(color));
}

void gfx_text(int x, int y, const char *utf8, uint32_t color, int scale)
{
	cv_text(&s_screen, x, y, utf8, fbcol(color), scale);
}

void gfx_text_centered(int y, const char *utf8, uint32_t color, int scale)
{
	int w = text_width(utf8, scale);
	cv_text(&s_screen, (s_screen.w - w) / 2, y, utf8, fbcol(color), scale);
}

void gfx_set_overlay_owner_core0(void)
{
	gfx_core0_owns_screen = true;
	DataSyncBarrier();
	// give the display core time to finish its current frame
	for (volatile int i = 0; i < 2000000; i++);
}
