/*
 *  prefs_circle.cpp - Preferences handling for QuadraPi
 *
 *  The settings live in /quadra/quadra.cfg on the SD card, in the usual
 *  Basilisk II text format (one "keyword value" pair per line).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"
#include "prefs.h"
#include "platform.h"
#include "qp.h"

// Platform-specific preferences items
prefs_desc platform_prefs_items[] = {
	{"cpuspeed", TYPE_INT32, false,    "CPU speed limit in MHz (0 = as fast as possible)"},
	{"soundout", TYPE_INT32, false,    "sound output (0 = HDMI, 1 = 3.5mm jack)"},
	{"volume", TYPE_INT32, false,      "sound volume in percent"},
	{"mousespeed", TYPE_INT32, false,  "mouse speed in percent"},
	{"idlewait", TYPE_BOOLEAN, false,  "sleep when the Mac is idle"},
	{"fd0", TYPE_STRING, false,        "image in floppy drive 1"},
	{"fd1", TYPE_STRING, false,        "image in floppy drive 2"},
	{"cd0", TYPE_STRING, false,        "image in the CD-ROM drive"},
	{"menukey", TYPE_INT32, false,     "USB HID code of the menu key (default F12)"},
	{NULL, TYPE_END, false, NULL} // End of list
};

// Slot names used for the removable drives (see sys_circle.cpp)
static void install_removable_slots(void)
{
	while (PrefsFindString("floppy"))
		PrefsRemoveItem("floppy");
	while (PrefsFindString("cdrom"))
		PrefsRemoveItem("cdrom");
	PrefsAddString("floppy", "@slot0");
	PrefsAddString("floppy", "@slot1");
	PrefsAddString("cdrom", "@slot2");
}

void LoadPrefs(const char *vmdir)
{
	FILE *f = fopen(QP_PREFS, "r");
	if (f != NULL) {
		LoadPrefsFromStream(f);
		fclose(f);
		plat_log("Settings loaded from %s", QP_PREFS);
	} else {
		plat_log("No %s, writing defaults", QP_PREFS);
		install_removable_slots();
		SavePrefs();
	}
	install_removable_slots();
}

void SavePrefs(void)
{
	FILE *f = fopen(QP_PREFS, "w");
	if (f != NULL) {
		fprintf(f, "# QuadraPi - impostazioni (modificabili anche dal menu con F12)\n");
		SavePrefsToStream(f);
		fclose(f);
	} else
		plat_log("Cannot write %s", QP_PREFS);
}

/*
 *  Defaults: a Macintosh Quadra 900
 */

void AddPlatformPrefsDefaults(void)
{
	PrefsReplaceInt32("ramsize", 64 * 1024 * 1024);
	PrefsReplaceInt32("modelid", 14);	// Gestalt 20 = Quadra 900
	PrefsReplaceInt32("cpu", 4);		// 68040
	PrefsReplaceBool("fpu", true);
	PrefsReplaceString("screen", "win/640/480");
	PrefsReplaceInt32("displaycolordepth", 8);
	PrefsReplaceInt32("frameskip", 0);
	PrefsReplaceBool("swap_opt_cmd", true);
	PrefsReplaceString("extfs", QP_SHARED_DIR);
	PrefsReplaceString("rom", QP_ROM);
	PrefsAddInt32("cpuspeed", 0);
	PrefsAddInt32("soundout", SOUND_OUT_HDMI);
	PrefsAddInt32("volume", 80);
	PrefsAddInt32("mousespeed", 100);
	PrefsAddBool("idlewait", true);
	PrefsAddInt32("menukey", 0x45);		// F12
}

/*
 *  Serial ports are not emulated
 */

void SysAddSerialPrefs(void)
{
}
