/*
 *  platform.h - Interface between the Circle kernel and the Basilisk II port
 *
 *  QuadraPi - Macintosh Quadra 900 emulator for Raspberry Pi 4 (bare metal)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <stddef.h>

// Base directory on the SD card for everything QuadraPi uses
#define QP_DIR        "SD:/quadra"
#define QP_PREFS      QP_DIR "/quadra.cfg"
#define QP_XPRAM      QP_DIR "/xpram.dat"
#define QP_CLOCK      QP_DIR "/clock.dat"
#define QP_ROM        QP_DIR "/q900.rom"
#define QP_HD_DIR     QP_DIR "/hd"
#define QP_FD_DIR     QP_DIR "/floppy"
#define QP_CD_DIR     QP_DIR "/cd"
#define QP_SHARED_DIR QP_DIR "/shared"

/*
 *  Services implemented by the Circle kernel (kernel.cpp), callable from core 0
 */

// Monotonic microsecond counter
uint64_t plat_usec(void);

// Periodic housekeeping (USB plug and play, CPU temperature throttling).
// Call a few times per second from core 0.
void plat_housekeeping(void);

// Logging (serial port + in-memory ring shown on fatal error screens)
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Show a fatal error screen and halt (never returns)
void plat_fatal(const char *msg) __attribute__((noreturn));

// Reboot the Raspberry Pi (used to apply settings that need a restart)
void plat_reboot(void) __attribute__((noreturn));

// Power off / halt (shows a "safe to switch off" screen)
void plat_halt(void) __attribute__((noreturn));

// System information for the menu
unsigned plat_cpu_temp(void);       // degrees Celsius
unsigned plat_arm_clock_mhz(void);  // current ARM clock
unsigned plat_ram_mb(void);         // physical RAM of the Pi
size_t   plat_heap_free(void);      // bytes available for emulated RAM

// USB mass storage mounted as "USB:" ?
bool plat_usb_storage_present(void);

// Sound output (all functions must be called on core 0)
enum { SOUND_OUT_HDMI = 0, SOUND_OUT_JACK = 1 };
bool     plat_sound_open(int output, unsigned sample_rate);
void     plat_sound_close(void);
unsigned plat_sound_frames_free(void);                 // frames that can be written now
void     plat_sound_write(const int16_t *stereo, unsigned frames);
unsigned plat_sound_rate(void);

// Physical frame buffer (32 bpp, 0xAARRGGBB)
struct plat_fb {
	uint32_t *pixels;
	unsigned width, height;
	unsigned pitch;			// in pixels
};
const plat_fb *plat_framebuffer(void);

/*
 *  Input events (produced in USB interrupt context, consumed on core 0)
 */

enum input_event_type {
	EV_KEY_DOWN,		// code = USB HID usage id
	EV_KEY_UP,
	EV_MOUSE			// dx, dy, buttons (bit0 left, bit1 right, bit2 middle), wheel
};

struct input_event {
	uint8_t type;
	uint8_t code;
	uint8_t buttons;
	int8_t wheel;
	int16_t dx, dy;
};

bool input_pop(input_event &ev);
void input_push(const input_event &ev);

// Modifier bits of the most recent keyboard report (USB HID format)
extern volatile uint8_t input_modifiers;

/*
 *  Emulator side entry points (called by the kernel)
 */

// Runs the whole emulator on core 0 (never returns)
void emulator_main(void) __attribute__((noreturn));

// Display refresh loop, runs on core 1 (never returns)
void video_core1_main(void) __attribute__((noreturn));

#endif
