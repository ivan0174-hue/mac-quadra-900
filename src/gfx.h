/*
 *  gfx.h - Minimal 32 bpp drawing helpers (frame buffer and off-screen canvases)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include "platform.h"

struct canvas {
	uint32_t *px;
	int w, h;
	int pitch;		// in pixels
};

// Font metrics (Circle 8x16 font, 3 extra pixels of line spacing)
enum { FONT_W = 8, FONT_H = 16, LINE_H = 19 };

// Canvas primitives
void cv_fill(canvas *c, int x, int y, int w, int h, uint32_t color);
void cv_rect(canvas *c, int x, int y, int w, int h, uint32_t color);	// 1 pixel outline
// Draws UTF-8 text (Latin-1 subset); returns width in pixels
int  cv_text(canvas *c, int x, int y, const char *utf8, uint32_t color, int scale = 1);
int  text_width(const char *utf8, int scale = 1);

// Physical frame buffer
// Colors are always written as 0xAARRGGBB; if the frame buffer uses RGB
// pixel order the red and blue channels are swapped on output.
extern bool gfx_swap_rb;
static inline uint32_t fbcol(uint32_t c)
{
	if (!gfx_swap_rb)
		return c;
	return (c & 0xFF00FF00u) | ((c >> 16) & 0xFF) | ((c & 0xFF) << 16);
}
void gfx_init(const plat_fb *fb);
canvas *gfx_screen(void);
unsigned gfx_width(void);
unsigned gfx_height(void);
void gfx_clear(uint32_t color);
void gfx_text(int x, int y, const char *utf8, uint32_t color, int scale);
void gfx_text_centered(int y, const char *utf8, uint32_t color, int scale);

// Tells the display core to stop drawing (core 0 takes over the screen)
void gfx_set_overlay_owner_core0(void);
extern volatile bool gfx_core0_owns_screen;

#endif
