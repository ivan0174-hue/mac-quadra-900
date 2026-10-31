/*
 *  qp.h - Internal interfaces between the QuadraPi platform modules
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef QP_H
#define QP_H

#include <stdint.h>

// main_circle.cpp
void qp_services(void);
void qp_set_cpu_speed(int mhz);			// 0 = no limit
uint32_t qp_measured_mips_x10(void);
extern volatile bool menu_requested;
extern uint8_t *MacVRAMHost;
extern uint32_t MacVRAMSize;

// timer_circle.cpp
void TimerSaveClock(void);
void TimerSetDateTime(uint32_t mac_seconds);

// xpram_circle.cpp
void XPRAMSaveIfChanged(void);
void ZapPRAMOnRestart(void);

// prefs_circle.cpp
void SavePrefs(void);

// input_circle.cpp
void input_service(void);
void input_set_mouse_speed(int percent);
void input_release_all(void);		// release keys/buttons held when the menu opened

// audio_circle.cpp
void audio_service(void);
void audio_apply_prefs(void);		// output / volume changed in the menu
void audio_mute_for_menu(bool muted);

// sys_circle.cpp - removable media management used by the menu
enum { SLOT_FLOPPY0 = 0, SLOT_FLOPPY1 = 1, SLOT_CDROM = 2, NUM_REMOVABLE_SLOTS = 3 };
const char *sys_slot_image(int slot);			// path of inserted image or NULL
bool sys_slot_insert(int slot, const char *path);	// insert image and notify the Mac
bool sys_slot_eject(int slot);					// force eject
void SysFlushAll(void);

// video_circle.cpp
void video_force_redraw(void);
void video_get_mac_mode(int &w, int &h, int &depth);
void video_get_scale(int &scale, int &ox, int &oy);
void video_set_menu_visible(bool visible);
void video_overlay_updated(void);

// Overlay canvas the menu draws into (owned by video_circle.cpp)
struct canvas;
canvas *video_overlay_canvas(void);
extern volatile int overlay_x, overlay_y, overlay_scale;

// menu.cpp
void menu_run(void);
void notice_show(const char *text, bool error);

#endif
