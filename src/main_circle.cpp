/*
 *  main_circle.cpp - Startup code and emulation services for QuadraPi
 *
 *  Based on Unix/main_unix.cpp from Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"

#include "cpu_emulation.h"
#include "sys.h"
#include "rom_patches.h"
#include "xpram.h"
#include "timer.h"
#include "video.h"
#include "emul_op.h"
#include "prefs.h"
#include "prefs_editor.h"
#include "macos_util.h"
#include "user_strings.h"
#include "version.h"
#include "main.h"
#include "audio.h"
#include "adb.h"

#include "platform.h"
#include "qp.h"

#include <fatfs/ff.h>
#include <string.h>
#include <strings.h>

#define DEBUG 0
#include "debug.h"

// Constants
const char ROM_FILE_NAME[] = QP_ROM;
const int SCRATCH_MEM_SIZE = 0x10000;	// Size of scratch memory area
const uint32 ROM_AREA_SIZE = 0x100000;	// 1 MB (largest 68k ROM)
const uint32 VRAM_AREA_SIZE = 0x800000;	// 8 MB (up to 1600x1200x32)

// CPU and FPU type, addressing mode
int CPUType;
bool CPUIs68060;
int FPUType;
bool TwentyFourBitAddressing;

// Global variables used by the bounds-checked direct addressing (memory.h)
uae_u32 MacMemLimit = 0;
uae_u8 *MacDummyPage = NULL;

// Mac frame buffer area (inside the Mac address window)
uint8 *MacVRAMHost = NULL;
uint32 MacVRAMSize = VRAM_AREA_SIZE;

// Interrupt flags
uint32 InterruptFlags = 0;

// vde switch variable (unused)
char *vde_sock = NULL;

// Scratch memory for ROM patches
uint8 *ScratchMem = NULL;

// tick inhibit flag (used by emul_op.cpp during boot)
bool tick_inhibit = false;

// Emulation state
static uint64 next_tick_usec = 0;
static uint64 last_second_usec = 0;
static int tick_counter = 0;

// CPU speed limiting
static const int32 CPU_QUANTUM = 1000;		// instructions between service checks
int32 emulated_ticks = CPU_QUANTUM;
static uint32 cpu_target_kips = 0;			// 0 = unlimited (instructions per ms)
static uint64 throttle_start_usec = 0;
static uint64 throttle_instr = 0;

// Statistics for the menu
static uint64 instr_counter = 0;
static uint64 mips_window_start = 0;
static uint64 mips_window_instr = 0;
static uint32 measured_kips = 0;

// Menu request flag (set by input code, handled from the CPU loop)
volatile bool menu_requested = false;

static void one_tick(void);
static void one_second(void);


/*
 *  Bad memory accesses (outside RAM/ROM/frame buffer)
 */

static int bad_access_reports = 0;

uae_u32 bad_mem_get(uaecptr addr, int size)
{
	if (bad_access_reports < 20) {
		bad_access_reports++;
		D(bug("bad read %d @ %08x\n", size, addr));
	}
	return 0;
}

void bad_mem_put(uaecptr addr, uae_u32 v, int size)
{
	if (bad_access_reports < 20) {
		bad_access_reports++;
		D(bug("bad write %d @ %08x\n", size, addr));
	}
}


/*
 *  CPU speed
 */

// Approximate 68040 throughput: ~0.7 MIPS per MHz
void qp_set_cpu_speed(int mhz)
{
	cpu_target_kips = mhz > 0 ? (uint32)mhz * 700 : 0;
	throttle_start_usec = GetTicks_usec();
	throttle_instr = 0;
}

uint32 qp_measured_mips_x10(void)
{
	return measured_kips / 100;
}


/*
 *  Services that run while the 68k is idle or between instruction quanta
 */

static inline void check_tick(uint64 now)
{
	if (now >= next_tick_usec) {
		one_tick();
		next_tick_usec += 16625;
		if (next_tick_usec < now)	// we fell behind (e.g. menu, long disk access)
			next_tick_usec = now + 16625;
	}
}

void qp_services(void)
{
	uint64 now = GetTicks_usec();
	check_tick(now);
	input_service();
	audio_service();
	if (menu_requested) {
		menu_requested = false;
		menu_run();		// modal, emulation paused
		// re-anchor timing after the pause
		uint64 t = GetTicks_usec();
		next_tick_usec = t + 16625;
		throttle_start_usec = t;
		throttle_instr = 0;
	}
}

void cpu_do_check_ticks(void)
{
	emulated_ticks = CPU_QUANTUM;
	instr_counter += CPU_QUANTUM;
	mips_window_instr += CPU_QUANTUM;

	qp_services();

	// Speed limit
	if (cpu_target_kips) {
		throttle_instr += CPU_QUANTUM;
		uint64 now = GetTicks_usec();
		uint64 due = throttle_start_usec + throttle_instr * 1000 / cpu_target_kips;
		if (now > throttle_start_usec + 1000000) {
			// start a new window every second (avoid long catch-up bursts)
			throttle_start_usec = now;
			throttle_instr = 0;
		}
		while (now < due) {
			if (due - now > 300)
				qp_services();
			now = GetTicks_usec();
		}
	}
}


/*
 *  Idle: the Mac has nothing to do (SynchIdleTime patch)
 */

void idle_wait(void)
{
	// Sleep until the next 60Hz tick or until an input event arrives
	while (GetTicks_usec() < next_tick_usec) {
		input_service();
		audio_service();
		if (InterruptFlags || menu_requested)
			break;
		__asm__ volatile("yield");
	}
	qp_services();
}

void idle_resume(void)
{
}


/*
 *  60Hz and 1Hz services
 */

static void one_second(void)
{
	// Pseudo Mac 1Hz interrupt, update local time
	WriteMacInt32(0x20c, TimerDateTime());

	SetInterruptFlag(INTFLAG_1HZ);
	TriggerInterrupt();

	// Write pending disk image data to the SD card every 2 seconds
	static int flush_counter = 0;
	if (++flush_counter >= 2) {
		flush_counter = 0;
		SysFlushAll();
	}

	static int second_counter = 0;
	if (++second_counter >= 60) {
		second_counter = 0;
		XPRAMSaveIfChanged();
		TimerSaveClock();
	}

	// MIPS statistics
	uint64 now = GetTicks_usec();
	if (now > mips_window_start) {
		measured_kips = mips_window_instr * 1000 / (now - mips_window_start);
	}
	mips_window_start = now;
	mips_window_instr = 0;
}

static void one_tick(void)
{
	if (++tick_counter >= 60) {
		tick_counter = 0;
		one_second();
	}

	plat_housekeeping();

	// Trigger 60Hz interrupt
	if (ROMVersion != ROM_VERSION_CLASSIC || HasMacStarted()) {
		SetInterruptFlag(INTFLAG_60HZ);
		TriggerInterrupt();
	}

	// Network is not emulated, but keep the ether driver happy
	SetInterruptFlag(INTFLAG_ETHER);
}


/*
 *  Find a ROM file: preferences first, then any 512K/1M "*.rom" in /quadra
 */

static bool load_rom_file(const char *path)
{
	FIL f;
	if (f_open(&f, path, FA_READ) != FR_OK)
		return false;
	FSIZE_t size = f_size(&f);
	if (size != 512*1024 && size != 1024*1024) {
		f_close(&f);
		return false;
	}
	UINT br = 0;
	FRESULT r = f_read(&f, ROMBaseHost, (UINT)size, &br);
	f_close(&f);
	if (r != FR_OK || br != size)
		return false;
	ROMSize = size;
	plat_log("ROM loaded from %s (%u KB)", path, (unsigned)(size / 1024));
	return true;
}

static bool find_and_load_rom(void)
{
	const char *pref = PrefsFindString("rom");
	if (pref && *pref && load_rom_file(pref))
		return true;
	if (load_rom_file(QP_ROM))
		return true;

	// Look for any ROM image in the QuadraPi folder
	DIR dir;
	FILINFO fi;
	if (f_opendir(&dir, QP_DIR) != FR_OK)
		return false;
	bool found = false;
	while (!found && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
		if (fi.fattrib & AM_DIR)
			continue;
		const char *ext = strrchr(fi.fname, '.');
		if (!ext || strcasecmp(ext, ".rom") != 0)
			continue;
		char path[300];
		snprintf(path, sizeof(path), "%s/%s", QP_DIR, fi.fname);
		if (load_rom_file(path)) {
			PrefsReplaceString("rom", path);
			found = true;
		}
	}
	f_closedir(&dir);
	return found;
}


/*
 *  Main program (runs on core 0, never returns)
 */

void emulator_main(void)
{
	plat_log("QuadraPi - Macintosh Quadra 900 (Basilisk II core)");

	// Folders on the SD card
	f_mkdir(QP_DIR);
	f_mkdir(QP_HD_DIR);
	f_mkdir(QP_FD_DIR);
	f_mkdir(QP_CD_DIR);
	f_mkdir(QP_SHARED_DIR);

	// Read preferences
	int argc = 0;
	char *args[1] = { NULL };
	char **argv = args;
	PrefsInit(NULL, argc, argv);

	// Init system routines
	SysInit();

	// Read RAM size
	RAMSize = PrefsFindInt32("ramsize");
	if (RAMSize <= 1000)
		RAMSize *= 1024 * 1024;
	RAMSize &= 0xfff00000;	// Round down to 1MB boundary
	if (RAMSize < 4*1024*1024)
		RAMSize = 4*1024*1024;
	if (RAMSize > 1023*1024*1024)
		RAMSize = 1023*1024*1024;

	// Make sure the RAM fits in the Pi's memory (keep 48 MB for everything else)
	size_t heap_free = plat_heap_free();
	size_t overhead = ROM_AREA_SIZE + SCRATCH_MEM_SIZE + VRAM_AREA_SIZE + 48*1024*1024;
	if (heap_free > overhead && RAMSize > heap_free - overhead) {
		RAMSize = (uint32)((heap_free - overhead) & 0xfff00000);
		plat_log("RAM reduced to %u MB (not enough memory on the Pi)", RAMSize >> 20);
	}

	// Allocate one contiguous Mac address window: RAM | ROM | scratch | VRAM
	uint32 total = RAMSize + ROM_AREA_SIZE + SCRATCH_MEM_SIZE + VRAM_AREA_SIZE;
	uint8 *area_raw = (uint8 *)malloc(total + 0x10000);
	uint8 *area = area_raw ? (uint8 *)(((uintptr)area_raw + 0xffff) & ~(uintptr)0xffff) : NULL;
	if (area == NULL)
		plat_fatal("Memoria insufficiente per la RAM del Macintosh.\nRiduci la RAM nel file quadra.cfg.");
	memset(area, 0, total);
	RAMBaseHost = area;
	ROMBaseHost = RAMBaseHost + RAMSize;
	ScratchMem = ROMBaseHost + ROM_AREA_SIZE + SCRATCH_MEM_SIZE / 2;
	MacVRAMHost = ROMBaseHost + ROM_AREA_SIZE + SCRATCH_MEM_SIZE;
	MacMemLimit = total;
	MacDummyPage = (uint8 *)calloc(1, 0x110000);

	MEMBaseDiff = (uintptr)RAMBaseHost;
	RAMBaseMac = 0;
	ROMBaseMac = Host2MacAddr(ROMBaseHost);

	plat_log("Mac RAM %u MB at %p, ROM at Mac %08x", RAMSize >> 20, RAMBaseHost, ROMBaseMac);

	// Load Mac ROM
	if (!find_and_load_rom())
		plat_fatal("ROM del Macintosh non trovata.\n"
				   "Copia il file della ROM (es. Quad900.ROM, 1 MB)\nnella cartella /quadra della scheda SD.");

	// Initialize everything
	if (!InitAll(NULL))
		plat_fatal("Inizializzazione dell'emulatore fallita (ROM non supportata?).");

	// CPU speed limit
	qp_set_cpu_speed(PrefsFindInt32("cpuspeed"));

	// Start timers
	next_tick_usec = GetTicks_usec() + 16625;
	last_second_usec = GetTicks_usec();
	mips_window_start = last_second_usec;

	plat_log("Starting emulation");

	// Start 68k and jump to ROM boot routine
	Start680x0();

	QuitEmulator();
	for (;;);
}


/*
 *  Quit emulator (Mac "Shut Down")
 */

void QuitEmulator(void)
{
	plat_log("QuitEmulator");
	XPRAMSaveIfChanged();
	TimerSaveClock();
	SysFlushAll();
	plat_halt();
}


/*
 *  Code was patched, flush caches if necessary
 */

void FlushCodeCache(void *start, uint32 size)
{
}


/*
 *  Mutexes (everything runs on one core, so they are no-ops)
 */

struct B2_mutex {
	int dummy;
};

B2_mutex *B2_create_mutex(void)
{
	return new B2_mutex;
}

void B2_lock_mutex(B2_mutex *mutex)
{
}

void B2_unlock_mutex(B2_mutex *mutex)
{
}

void B2_delete_mutex(B2_mutex *mutex)
{
	delete mutex;
}


/*
 *  Interrupt flags
 */

void SetInterruptFlag(uint32 flag)
{
	InterruptFlags |= flag;
}

void ClearInterruptFlag(uint32 flag)
{
	InterruptFlags &= ~flag;
}


/*
 *  Display alerts
 */

void ErrorAlert(const char *text)
{
	plat_log("ERROR: %s", text);
	notice_show(text, true);
}

void WarningAlert(const char *text)
{
	plat_log("WARNING: %s", text);
	notice_show(text, false);
}

bool ChoiceAlert(const char *text, const char *pos, const char *neg)
{
	plat_log("CHOICE: %s", text);
	return false;
}
