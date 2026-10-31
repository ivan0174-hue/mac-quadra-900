/*
 *  sys_circle.cpp - Disk image access for QuadraPi (FatFs on SD card / USB)
 *
 *  Hard disks are image files listed as "disk" in quadra.cfg.
 *  The two floppy drives and the CD-ROM drive are "slots" whose image can
 *  be changed at run time from the menu (F12).
 *
 *  Based on Unix/sys_unix.cpp from Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"
#include "main.h"
#include "macos_util.h"
#include "prefs.h"
#include "user_strings.h"
#include "sys.h"
#include "sony.h"
#include "cdrom.h"
#include "platform.h"
#include "qp.h"



#include <fatfs/ff.h>
#include <string>

#define DEBUG 0
#include "debug.h"

enum { KIND_FILE, KIND_SLOT };

struct file_handle {
	int kind;
	int slot;				// for KIND_SLOT
	bool is_cdrom;
	bool is_floppy;
	bool attached;			// image file open?
	bool read_only;
	bool dirty;
	std::string path;
	FIL fil;
	DWORD *cltbl;			// FatFs fast seek table
	loff_t start_byte;		// header size
	loff_t size;			// usable size
	uint32 id;				// unique id for the block cache
};

static file_handle *slot_handles[NUM_REMOVABLE_SLOTS];
static uint32 next_handle_id = 1;
static const int MAX_HANDLES = 16;
static file_handle *all_handles[MAX_HANDLES];


/*
 *  Read cache: 32 KB blocks, shared by all images (SD card latency is high,
 *  the Mac reads many small blocks)
 */

static const uint32 CACHE_BLOCK = 32768;
static const int CACHE_SLOTS = 256;		// 8 MB

struct cache_slot {
	uint32 id;				// 0 = free
	uint64 block;
	uint32 valid;			// bytes valid (short at end of file)
	uint32 last_use;
	uint8 *data;
};

static cache_slot cache[CACHE_SLOTS];
static uint8 *cache_mem = NULL;
static uint32 cache_clock = 0;

static void cache_init(void)
{
	cache_mem = (uint8 *)malloc(CACHE_SLOTS * CACHE_BLOCK);
	for (int i = 0; i < CACHE_SLOTS; i++) {
		cache[i].id = 0;
		cache[i].data = cache_mem ? cache_mem + i * CACHE_BLOCK : NULL;
	}
}

static void cache_invalidate(uint32 id)
{
	for (int i = 0; i < CACHE_SLOTS; i++)
		if (cache[i].id == id)
			cache[i].id = 0;
}

static cache_slot *cache_find(uint32 id, uint64 block)
{
	for (int i = 0; i < CACHE_SLOTS; i++)
		if (cache[i].id == id && cache[i].block == block) {
			cache[i].last_use = ++cache_clock;
			return &cache[i];
		}
	return NULL;
}

static cache_slot *cache_victim(void)
{
	cache_slot *best = &cache[0];
	for (int i = 0; i < CACHE_SLOTS; i++) {
		if (cache[i].id == 0)
			return &cache[i];
		if (cache[i].last_use < best->last_use)
			best = &cache[i];
	}
	return best;
}


/*
 *  Low level file access
 */

static bool fh_seek(file_handle *fh, uint64 pos)
{
	return f_lseek(&fh->fil, (FSIZE_t)pos) == FR_OK;
}

static void setup_fast_seek(file_handle *fh)
{
	// Cluster link map table makes random access to large images fast
	DWORD n = 256;
	for (int tries = 0; tries < 6; tries++) {
		fh->cltbl = (DWORD *)malloc(n * sizeof(DWORD));
		if (!fh->cltbl)
			return;
		fh->cltbl[0] = n;
		fh->fil.cltbl = fh->cltbl;
		FRESULT r = f_lseek(&fh->fil, CREATE_LINKMAP);
		if (r == FR_OK)
			return;
		DWORD need = fh->cltbl[0];
		free(fh->cltbl);
		fh->cltbl = NULL;
		fh->fil.cltbl = NULL;
		if (r != FR_NOT_ENOUGH_CORE || need <= n)
			return;
		n = need;
	}
}

static bool attach_image(file_handle *fh, const char *path, bool read_only)
{
	BYTE mode = read_only ? FA_READ : (FA_READ | FA_WRITE);
	FRESULT r = f_open(&fh->fil, path, mode);
	if (r != FR_OK && !read_only) {
		r = f_open(&fh->fil, path, FA_READ);
		read_only = true;
	}
	if (r != FR_OK) {
		plat_log("Cannot open image %s (error %d)", path, (int)r);
		return false;
	}

	// Write-protected file attribute?
	FILINFO fi;
	if (f_stat(path, &fi) == FR_OK && (fi.fattrib & AM_RDO))
		read_only = true;

	fh->attached = true;
	fh->read_only = read_only || fh->is_cdrom;
	fh->dirty = false;
	fh->path = path;
	fh->cltbl = NULL;
	fh->id = next_handle_id++;

	loff_t file_size = f_size(&fh->fil);
	if (fh->is_cdrom) {
		fh->start_byte = 0;
		fh->size = file_size;
	} else {
		FileDiskLayout(file_size, NULL, fh->start_byte, fh->size);
	}

	setup_fast_seek(fh);
	plat_log("Image %s attached (%u KB%s)", path, (unsigned)(fh->size / 1024), fh->read_only ? ", read-only" : "");
	return true;
}

static void detach_image(file_handle *fh)
{
	if (!fh->attached)
		return;
	if (fh->dirty)
		f_sync(&fh->fil);
	f_close(&fh->fil);
	if (fh->cltbl)
		free(fh->cltbl);
	fh->cltbl = NULL;
	cache_invalidate(fh->id);
	fh->attached = false;
	fh->dirty = false;
	fh->path.clear();
}

static const char *slot_pref_name(int slot)
{
	switch (slot) {
		case SLOT_FLOPPY0: return "fd0";
		case SLOT_FLOPPY1: return "fd1";
		default: return "cd0";
	}
}


/*
 *  Initialization
 */

void SysInit(void)
{
	cache_init();
}

void SysExit(void)
{
	SysFlushAll();
}

void SysFlushAll(void)
{
	for (int i = 0; i < MAX_HANDLES; i++) {
		file_handle *fh = all_handles[i];
		if (fh && fh->attached && fh->dirty) {
			f_sync(&fh->fil);
			fh->dirty = false;
		}
	}
}

void SysAddFloppyPrefs(void)
{
}

void SysAddDiskPrefs(void)
{
}

void SysAddCDROMPrefs(void)
{
	// The CD-ROM drive is always present (as an image slot)
}


/*
 *  Open file/device, create new file handle (returns NULL on error)
 */

void *Sys_open(const char *name, bool read_only, bool is_cdrom)
{
	file_handle *fh = new file_handle;
	fh->kind = KIND_FILE;
	fh->slot = -1;
	fh->is_cdrom = is_cdrom;
	fh->is_floppy = false;
	fh->attached = false;
	fh->read_only = read_only;
	fh->dirty = false;
	fh->cltbl = NULL;
	fh->start_byte = 0;
	fh->size = 0;
	fh->id = 0;

	if (name[0] == '@' && strncmp(name, "@slot", 5) == 0) {
		// Removable drive slot
		int slot = name[5] - '0';
		if (slot < 0 || slot >= NUM_REMOVABLE_SLOTS) {
			delete fh;
			return NULL;
		}
		fh->kind = KIND_SLOT;
		fh->slot = slot;
		fh->is_cdrom = (slot == SLOT_CDROM);
		fh->is_floppy = !fh->is_cdrom;
		slot_handles[slot] = fh;

		const char *img = PrefsFindString(slot_pref_name(slot));
		if (img && *img) {
			if (!attach_image(fh, img, fh->is_cdrom)) {
				PrefsReplaceString(slot_pref_name(slot), "");
			}
		}
	} else {
		// Hard disk image
		if (!attach_image(fh, name, read_only)) {
			char msg[400];
			snprintf(msg, sizeof(msg), "Impossibile aprire l'immagine disco:\n%s", name);
			WarningAlert(msg);
			delete fh;
			return NULL;
		}
		// Small images (<= 1.44 MB) behave like floppies
		if (!is_cdrom && fh->size <= 1474560)
			fh->is_floppy = true;
	}

	for (int i = 0; i < MAX_HANDLES; i++)
		if (!all_handles[i]) {
			all_handles[i] = fh;
			break;
		}
	return fh;
}


/*
 *  Close file/device, delete file handle
 */

void Sys_close(void *arg)
{
	file_handle *fh = (file_handle *)arg;
	if (!fh)
		return;
	detach_image(fh);
	for (int i = 0; i < MAX_HANDLES; i++)
		if (all_handles[i] == fh)
			all_handles[i] = NULL;
	if (fh->kind == KIND_SLOT)
		slot_handles[fh->slot] = NULL;
	delete fh;
}


/*
 *  Read "length" bytes from file/device, starting at "offset", to "buffer",
 *  returns number of bytes read (or 0)
 */

static size_t raw_read(file_handle *fh, void *buffer, uint64 pos, size_t length)
{
	if (!fh_seek(fh, pos))
		return 0;
	UINT br = 0;
	if (f_read(&fh->fil, buffer, (UINT)length, &br) != FR_OK)
		return 0;
	return br;
}

size_t Sys_read(void *arg, void *buffer, loff_t offset, size_t length)
{
	file_handle *fh = (file_handle *)arg;
	if (!fh || !fh->attached)
		return 0;

	if (offset < 0 || offset >= fh->size)
		return 0;
	if ((loff_t)(offset + length) > fh->size)
		length = fh->size - offset;

	uint64 pos = offset + fh->start_byte;

	// Large reads go straight to the card
	if (!cache_mem || length >= 4 * CACHE_BLOCK)
		return raw_read(fh, buffer, pos, length);

	uint8 *dst = (uint8 *)buffer;
	size_t done = 0;
	uint64 file_end = fh->size + fh->start_byte;
	while (done < length) {
		uint64 p = pos + done;
		uint64 block = p / CACHE_BLOCK;
		uint32 in_block = p % CACHE_BLOCK;
		cache_slot *cs = cache_find(fh->id, block);
		if (!cs) {
			cs = cache_victim();
			cs->id = 0;
			uint64 bstart = block * CACHE_BLOCK;
			uint32 want = CACHE_BLOCK;
			if (bstart + want > file_end)
				want = file_end - bstart;
			size_t got = raw_read(fh, cs->data, bstart, want);
			if (got == 0)
				return done;
			cs->id = fh->id;
			cs->block = block;
			cs->valid = got;
			cs->last_use = ++cache_clock;
		}
		if (in_block >= cs->valid)
			return done;
		uint32 n = cs->valid - in_block;
		if (n > length - done)
			n = length - done;
		memcpy(dst + done, cs->data + in_block, n);
		done += n;
	}
	return done;
}


/*
 *  Write "length" bytes from "buffer" to file/device, starting at "offset",
 *  returns number of bytes written (or 0)
 */

size_t Sys_write(void *arg, void *buffer, loff_t offset, size_t length)
{
	file_handle *fh = (file_handle *)arg;
	if (!fh || !fh->attached || fh->read_only)
		return 0;

	if (offset < 0 || offset >= fh->size)
		return 0;
	if ((loff_t)(offset + length) > fh->size)
		length = fh->size - offset;

	uint64 pos = offset + fh->start_byte;
	if (!fh_seek(fh, pos))
		return 0;
	UINT bw = 0;
	if (f_write(&fh->fil, buffer, (UINT)length, &bw) != FR_OK)
		return 0;
	fh->dirty = true;

	// Update cached copies
	if (cache_mem) {
		const uint8 *src = (const uint8 *)buffer;
		size_t done = 0;
		while (done < bw) {
			uint64 p = pos + done;
			uint64 block = p / CACHE_BLOCK;
			uint32 in_block = p % CACHE_BLOCK;
			uint32 n = CACHE_BLOCK - in_block;
			if (n > bw - done)
				n = bw - done;
			for (int i = 0; i < CACHE_SLOTS; i++)
				if (cache[i].id == fh->id && cache[i].block == block) {
					if (in_block + n <= cache[i].valid)
						memcpy(cache[i].data + in_block, src + done, n);
					else
						cache[i].id = 0;
				}
			done += n;
		}
	}
	return bw;
}


/*
 *  Return size of file/device (minus header)
 */

loff_t SysGetFileSize(void *arg)
{
	file_handle *fh = (file_handle *)arg;
	if (!fh || !fh->attached)
		return 0;
	return fh->size;
}


/*
 *  Eject volume (if applicable)
 */

void SysEject(void *arg)
{
	file_handle *fh = (file_handle *)arg;
	if (!fh)
		return;
	if (fh->kind == KIND_SLOT) {
		detach_image(fh);
		PrefsReplaceString(slot_pref_name(fh->slot), "");
		SavePrefs();
	}
}


/*
 *  Format volume (if applicable)
 */

bool SysFormat(void *arg)
{
	file_handle *fh = (file_handle *)arg;
	return fh && fh->attached && !fh->read_only;
}


/*
 *  Check if file/device is read-only (this includes the read-only flag on Sys_open())
 */

bool SysIsReadOnly(void *arg)
{
	file_handle *fh = (file_handle *)arg;
	if (!fh)
		return true;
	return fh->read_only;
}


/*
 *  Check if the given file handle refers to a fixed or a removable disk
 */

bool SysIsFixedDisk(void *arg)
{
	file_handle *fh = (file_handle *)arg;
	if (!fh)
		return true;
	return fh->kind == KIND_FILE && !fh->is_cdrom && !fh->is_floppy;
}


/*
 *  Check if a disk is inserted in the drive (always true for files)
 */

bool SysIsDiskInserted(void *arg)
{
	file_handle *fh = (file_handle *)arg;
	return fh && fh->attached;
}


/*
 *  Prevent/allow medium removal (if applicable)
 */

void SysPreventRemoval(void *arg)
{
}

void SysAllowRemoval(void *arg)
{
}


/*
 *  CD-ROM functions: images contain a single data track
 */

static void lba_to_msf(uint32 lba, uint8 &m, uint8 &s, uint8 &f)
{
	lba += 150;
	m = lba / (60 * 75);
	s = (lba / 75) % 60;
	f = lba % 75;
}

bool SysCDReadTOC(void *arg, uint8 *toc)
{
	file_handle *fh = (file_handle *)arg;
	if (!fh || !fh->attached)
		return false;

	uint8 *p = toc + 2;
	*p++ = 1;	// first track
	*p++ = 1;	// last track

	// Track 1: data
	uint8 m, s, f;
	lba_to_msf(0, m, s, f);
	*p++ = 0;
	*p++ = 0x14;	// ADR 1, data track
	*p++ = 1;
	*p++ = 0;
	*p++ = 0;
	*p++ = m;
	*p++ = s;
	*p++ = f;

	// Lead-out
	lba_to_msf((uint32)(fh->size / 2048), m, s, f);
	*p++ = 0;
	*p++ = 0x14;
	*p++ = 0xaa;
	*p++ = 0;
	*p++ = 0;
	*p++ = m;
	*p++ = s;
	*p++ = f;

	int toc_size = p - toc;
	toc[0] = toc_size >> 8;
	toc[1] = toc_size & 0xff;
	return true;
}

bool SysCDGetPosition(void *arg, uint8 *pos)
{
	return false;
}

bool SysCDPlay(void *arg, uint8 start_m, uint8 start_s, uint8 start_f, uint8 end_m, uint8 end_s, uint8 end_f)
{
	return false;
}

bool SysCDPause(void *arg)
{
	return false;
}

bool SysCDResume(void *arg)
{
	return false;
}

bool SysCDStop(void *arg, uint8 lead_out_m, uint8 lead_out_s, uint8 lead_out_f)
{
	return false;
}

bool SysCDScan(void *arg, uint8 start_m, uint8 start_s, uint8 start_f, bool reverse)
{
	return false;
}

void SysCDSetVolume(void *arg, uint8 left, uint8 right)
{
}

void SysCDGetVolume(void *arg, uint8 &left, uint8 &right)
{
	left = right = 0;
}


/*
 *  Removable media management for the menu
 */

const char *sys_slot_image(int slot)
{
	if (slot < 0 || slot >= NUM_REMOVABLE_SLOTS)
		return NULL;
	file_handle *fh = slot_handles[slot];
	if (!fh || !fh->attached)
		return NULL;
	return fh->path.c_str();
}

bool sys_slot_insert(int slot, const char *path)
{
	if (slot < 0 || slot >= NUM_REMOVABLE_SLOTS)
		return false;
	file_handle *fh = slot_handles[slot];
	if (!fh)
		return false;
	if (fh->attached)
		return false;		// eject first
	if (!attach_image(fh, path, fh->is_cdrom))
		return false;
	PrefsReplaceString(slot_pref_name(slot), path);
	SavePrefs();
	// Before the drivers are open the Mac finds the disk by itself
	if (HasMacStarted()) {
		if (slot == SLOT_CDROM)
			CDROMMountVolume(fh);
		else
			SonyMountVolume(fh);
	}
	return true;
}

bool sys_slot_eject(int slot)
{
	if (slot < 0 || slot >= NUM_REMOVABLE_SLOTS)
		return false;
	file_handle *fh = slot_handles[slot];
	if (!fh || !fh->attached)
		return false;
	detach_image(fh);
	PrefsReplaceString(slot_pref_name(slot), "");
	SavePrefs();
	return true;
}
