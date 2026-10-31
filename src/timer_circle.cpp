/*
 *  timer_circle.cpp - Time Manager emulation and clock for QuadraPi
 *
 *  The Raspberry Pi has no battery backed clock. The Mac date/time is kept
 *  in /quadra/clock.dat (saved every minute and at shutdown) and can be set
 *  from the Mac "Date & Time" control panel or from the QuadraPi menu.
 *
 *  Based on Unix/timer_unix.cpp from Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"
#include "macos_util.h"
#include "timer.h"
#include "platform.h"
#include "qp.h"

#include <fatfs/ff.h>

#define DEBUG 0
#include "debug.h"

// Mac time (seconds since 1.1.1904) at plat_usec() == clock_base_usec
static uint32 clock_base_mac = 0;
static uint64 clock_base_usec = 0;
static bool clock_loaded = false;

// 1 January 2026, 00:00 (default when no clock file exists)
static const uint32 DEFAULT_MAC_TIME = 3850070400u;

static void load_clock(void)
{
	clock_loaded = true;
	clock_base_usec = plat_usec();
	clock_base_mac = DEFAULT_MAC_TIME;

	FIL f;
	if (f_open(&f, QP_CLOCK, FA_READ) == FR_OK) {
		char buf[32] = {0};
		UINT br = 0;
		f_read(&f, buf, sizeof(buf) - 1, &br);
		f_close(&f);
		unsigned long v = strtoul(buf, NULL, 10);
		if (v > 2082844800u)	// later than 1970
			clock_base_mac = (uint32)v + 5;	// a few seconds for the reboot
	}
}

uint32 TimerDateTime(void)
{
	if (!clock_loaded)
		load_clock();
	return clock_base_mac + (uint32)((plat_usec() - clock_base_usec) / 1000000);
}

void TimerSetDateTime(uint32_t mac_seconds)
{
	if (!clock_loaded)
		load_clock();
	clock_base_usec = plat_usec();
	clock_base_mac = mac_seconds;
	TimerSaveClock();
}

// Called by emul_op.cpp when the Mac writes one byte of the RTC seconds counter
void TimerSetRTCByte(int index, uint8 value)
{
	uint32 t = TimerDateTime();
	int shift = index * 8;
	t = (t & ~(0xffu << shift)) | ((uint32)value << shift);
	clock_base_usec = plat_usec();
	clock_base_mac = t;
	if (index == 3)
		TimerSaveClock();
}

void TimerSaveClock(void)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%u\n", TimerDateTime());
	FIL f;
	if (f_open(&f, QP_CLOCK, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
		UINT bw;
		f_write(&f, buf, strlen(buf), &bw);
		f_close(&f);
	}
}


/*
 *  Return microseconds since boot (64 bit)
 */

void Microseconds(uint32 &hi, uint32 &lo)
{
	uint64 tl = plat_usec();
	hi = tl >> 32;
	lo = tl;
}


/*
 *  Get current time (monotonic)
 */

void timer_current_time(tm_time_t &t)
{
	uint64 us = plat_usec();
	t.tv_sec = us / 1000000;
	t.tv_nsec = (us % 1000000) * 1000;
}


/*
 *  Add times
 */

void timer_add_time(tm_time_t &res, tm_time_t a, tm_time_t b)
{
	res.tv_sec = a.tv_sec + b.tv_sec;
	res.tv_nsec = a.tv_nsec + b.tv_nsec;
	if (res.tv_nsec >= 1000000000) {
		res.tv_sec++;
		res.tv_nsec -= 1000000000;
	}
}


/*
 *  Subtract times
 */

void timer_sub_time(tm_time_t &res, tm_time_t a, tm_time_t b)
{
	res.tv_sec = a.tv_sec - b.tv_sec;
	res.tv_nsec = a.tv_nsec - b.tv_nsec;
	if (res.tv_nsec < 0) {
		res.tv_sec--;
		res.tv_nsec += 1000000000;
	}
}


/*
 *  Compare times (<0: a < b, =0: a = b, >0: a > b)
 */

int timer_cmp_time(tm_time_t a, tm_time_t b)
{
	if (a.tv_sec == b.tv_sec)
		return a.tv_nsec - b.tv_nsec;
	else
		return a.tv_sec - b.tv_sec;
}


/*
 *  Convert Mac time value (>0: milliseconds, <0: microseconds) to tm_time_t
 */

void timer_mac2host_time(tm_time_t &res, int32 mactime)
{
	if (mactime > 0) {
		// Time in milliseconds
		res.tv_sec = mactime / 1000;
		res.tv_nsec = (mactime % 1000) * 1000000;
	} else {
		// Time in negative microseconds
		res.tv_sec = -mactime / 1000000;
		res.tv_nsec = (-mactime % 1000000) * 1000;
	}
}


/*
 *  Convert positive tm_time_t to Mac time value (>0: milliseconds, <0: microseconds)
 */

int32 timer_host2mac_time(tm_time_t hosttime)
{
	if (hosttime.tv_sec < 0)
		return 0;
	else {
		uint64 t = (uint64)hosttime.tv_sec * 1000000 + hosttime.tv_nsec / 1000;
		if (t > 0x7fffffff)
			return t / 1000;	// Time in milliseconds
		else
			return -t;			// Time in negative microseconds
	}
}


/*
 *  Get current value of microsecond timer
 */

uint64 GetTicks_usec(void)
{
	return plat_usec();
}


/*
 *  Delay by specified number of microseconds
 */

void Delay_usec(uint64 usec)
{
	uint64 end = plat_usec() + usec;
	while (plat_usec() < end)
		;
}
