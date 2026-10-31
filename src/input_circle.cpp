/*
 *  input_circle.cpp - USB keyboard and mouse -> Mac ADB for QuadraPi
 *
 *  Keys are translated by position (USB HID usage -> ADB key code), so the
 *  keyboard layout is chosen inside Mac OS (e.g. "Italiano" in the
 *  Keyboard control panel).
 *
 *  Default modifier mapping for PC keyboards:
 *    Alt (left)       -> Command
 *    Windows / AltGr  -> Option
 *    Ctrl             -> Control
 *  ("swap_opt_cmd false" in quadra.cfg swaps Command and Option.)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"
#include "main.h"
#include "adb.h"
#include "prefs.h"
#include "video.h"
#include "platform.h"
#include "qp.h"

static uint8 hid_to_adb[256];
static bool tables_ready = false;
static bool mac_key_down[128];
static bool button_down[3];
static float mouse_x = 320, mouse_y = 240;
static float mouse_speed = 1.0f;
static int menu_key = 0x45;
static bool swap_opt_cmd = true;

static void init_tables(void)
{
	memset(hid_to_adb, 0xff, sizeof(hid_to_adb));

	// Letters a..z
	static const uint8 letters[26] = {
		0x00, 0x0b, 0x08, 0x02, 0x0e, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2e,	// a..m
		0x2d, 0x1f, 0x23, 0x0c, 0x0f, 0x01, 0x11, 0x20, 0x09, 0x0d, 0x07, 0x10, 0x06	// n..z
	};
	for (int i = 0; i < 26; i++)
		hid_to_adb[0x04 + i] = letters[i];

	// Digits 1..9, 0
	static const uint8 digits[10] = {0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1a, 0x1c, 0x19, 0x1d};
	for (int i = 0; i < 10; i++)
		hid_to_adb[0x1e + i] = digits[i];

	hid_to_adb[0x28] = 0x24;	// Return
	hid_to_adb[0x29] = 0x35;	// Escape
	hid_to_adb[0x2a] = 0x33;	// Backspace -> Delete
	hid_to_adb[0x2b] = 0x30;	// Tab
	hid_to_adb[0x2c] = 0x31;	// Space
	hid_to_adb[0x2d] = 0x1b;	// -
	hid_to_adb[0x2e] = 0x18;	// =
	hid_to_adb[0x2f] = 0x21;	// [
	hid_to_adb[0x30] = 0x1e;	// ]
	hid_to_adb[0x31] = 0x2a;	// backslash
	hid_to_adb[0x32] = 0x2a;	// ISO key next to Return
	hid_to_adb[0x33] = 0x29;	// ;
	hid_to_adb[0x34] = 0x27;	// '
	hid_to_adb[0x35] = 0x32;	// ` (key left of 1)
	hid_to_adb[0x36] = 0x2b;	// ,
	hid_to_adb[0x37] = 0x2f;	// .
	hid_to_adb[0x38] = 0x2c;	// /
	hid_to_adb[0x39] = 0x39;	// Caps Lock

	// F1..F12
	static const uint8 fkeys[12] = {0x7a, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6d, 0x67, 0x6f};
	for (int i = 0; i < 12; i++)
		hid_to_adb[0x3a + i] = fkeys[i];
	hid_to_adb[0x46] = 0x69;	// Print Screen -> F13
	hid_to_adb[0x47] = 0x6b;	// Scroll Lock -> F14
	hid_to_adb[0x48] = 0x71;	// Pause -> F15

	hid_to_adb[0x49] = 0x72;	// Insert -> Help
	hid_to_adb[0x4a] = 0x73;	// Home
	hid_to_adb[0x4b] = 0x74;	// Page Up
	hid_to_adb[0x4c] = 0x75;	// Delete -> Forward Delete
	hid_to_adb[0x4d] = 0x77;	// End
	hid_to_adb[0x4e] = 0x79;	// Page Down
	hid_to_adb[0x4f] = 0x3c;	// Right
	hid_to_adb[0x50] = 0x3b;	// Left
	hid_to_adb[0x51] = 0x3d;	// Down
	hid_to_adb[0x52] = 0x3e;	// Up

	// Keypad
	hid_to_adb[0x53] = 0x47;	// Num Lock -> Clear
	hid_to_adb[0x54] = 0x4b;	// /
	hid_to_adb[0x55] = 0x43;	// *
	hid_to_adb[0x56] = 0x4e;	// -
	hid_to_adb[0x57] = 0x45;	// +
	hid_to_adb[0x58] = 0x4c;	// Enter
	static const uint8 kp[10] = {0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5b, 0x5c, 0x52};	// 1..9, 0
	for (int i = 0; i < 10; i++)
		hid_to_adb[0x59 + i] = kp[i];
	hid_to_adb[0x63] = 0x41;	// .
	hid_to_adb[0x64] = 0x0a;	// ISO key next to left Shift (< > on Italian keyboards)
	hid_to_adb[0x67] = 0x51;	// keypad =

	tables_ready = true;
}

static int modifier_to_adb(int code)
{
	switch (code) {
		case 0xe0: case 0xe4: return 0x36;					// Control
		case 0xe1: case 0xe5: return 0x38;					// Shift
		case 0xe2: return swap_opt_cmd ? 0x37 : 0x3a;		// left Alt
		case 0xe3: case 0xe7: return swap_opt_cmd ? 0x3a : 0x37;	// Windows
		case 0xe6: return 0x3a;								// AltGr -> Option
	}
	return -1;
}

void input_set_mouse_speed(int percent)
{
	if (percent < 10) percent = 10;
	if (percent > 400) percent = 400;
	mouse_speed = percent / 100.0f;
}

static void key_down(int adb)
{
	if (adb < 0 || adb > 127)
		return;
	mac_key_down[adb] = true;
	ADBKeyDown(adb);
}

static void key_up(int adb)
{
	if (adb < 0 || adb > 127)
		return;
	if (!mac_key_down[adb])
		return;
	mac_key_down[adb] = false;
	ADBKeyUp(adb);
}

void input_release_all(void)
{
	for (int i = 0; i < 128; i++)
		if (mac_key_down[i])
			key_up(i);
	for (int b = 0; b < 3; b++)
		if (button_down[b]) {
			button_down[b] = false;
			ADBMouseUp(b);
		}
}

void input_service(void)
{
	static bool first = true;
	if (first) {
		first = false;
		if (!tables_ready)
			init_tables();
		ADBSetRelMouseMode(false);
		swap_opt_cmd = PrefsFindBool("swap_opt_cmd");
		menu_key = PrefsFindInt32("menukey");
		if (menu_key <= 0 || menu_key > 255)
			menu_key = 0x45;
		input_set_mouse_speed(PrefsFindInt32("mousespeed"));
	}

	input_event ev;
	bool moved = false;
	while (input_pop(ev)) {
		switch (ev.type) {
			case EV_KEY_DOWN:
				if (ev.code == menu_key) {
					input_release_all();
					menu_requested = true;
					return;		// leave remaining events to the menu
				}
				if (ev.code >= 0xe0)
					key_down(modifier_to_adb(ev.code));
				else if (hid_to_adb[ev.code] != 0xff)
					key_down(hid_to_adb[ev.code]);
				break;

			case EV_KEY_UP:
				if (ev.code == menu_key)
					break;
				if (ev.code >= 0xe0)
					key_up(modifier_to_adb(ev.code));
				else if (hid_to_adb[ev.code] != 0xff)
					key_up(hid_to_adb[ev.code]);
				break;

			case EV_MOUSE: {
				int w = 640, h = 480, d;
				video_get_mac_mode(w, h, d);
				mouse_x += ev.dx * mouse_speed;
				mouse_y += ev.dy * mouse_speed;
				if (mouse_x < 0) mouse_x = 0;
				if (mouse_y < 0) mouse_y = 0;
				if (mouse_x > w - 1) mouse_x = w - 1;
				if (mouse_y > h - 1) mouse_y = h - 1;
				if (ev.dx || ev.dy)
					moved = true;

				for (int b = 0; b < 3; b++) {
					bool down = (ev.buttons >> b) & 1;
					if (down != button_down[b]) {
						button_down[b] = down;
						if (down)
							ADBMouseDown(b);
						else
							ADBMouseUp(b);
					}
				}

				// Wheel -> cursor up/down (System 7 has no scroll wheel support)
				if (ev.wheel) {
					int code = ev.wheel > 0 ? 0x3e : 0x3d;
					int n = ev.wheel > 0 ? ev.wheel : -ev.wheel;
					for (int i = 0; i < n * 3; i++) {
						ADBKeyDown(code);
						ADBKeyUp(code);
					}
				}
				break;
			}
		}
	}
	if (moved)
		ADBMouseMoved((int)mouse_x, (int)mouse_y);
}
