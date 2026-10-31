/*
 *  xpram_circle.cpp - XPRAM (parameter RAM) handling for QuadraPi
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"
#include "xpram.h"
#include "platform.h"
#include "qp.h"

#include <fatfs/ff.h>

static uint8 last_saved[XPRAM_SIZE];
static bool frozen = false;	// PRAM reset requested, do not save any more

void LoadXPRAM(const char *vmdir)
{
	FIL f;
	if (f_open(&f, QP_XPRAM, FA_READ) == FR_OK) {
		UINT br = 0;
		f_read(&f, XPRAM, XPRAM_SIZE, &br);
		f_close(&f);
	}
	memcpy(last_saved, XPRAM, XPRAM_SIZE);
}

void SaveXPRAM(void)
{
	if (frozen)
		return;
	FIL f;
	if (f_open(&f, QP_XPRAM, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
		UINT bw = 0;
		f_write(&f, XPRAM, XPRAM_SIZE, &bw);
		f_close(&f);
	}
	memcpy(last_saved, XPRAM, XPRAM_SIZE);
}

void XPRAMSaveIfChanged(void)
{
	if (memcmp(last_saved, XPRAM, XPRAM_SIZE) != 0)
		SaveXPRAM();
}

void ZapPRAM(void)
{
	f_unlink(QP_XPRAM);
	memset(XPRAM, 0, XPRAM_SIZE);
	memcpy(last_saved, XPRAM, XPRAM_SIZE);
}

void ZapPRAMOnRestart(void)
{
	f_unlink(QP_XPRAM);
	frozen = true;
}
