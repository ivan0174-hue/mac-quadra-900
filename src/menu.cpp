/*
 *  menu.cpp - On-screen configuration menu for QuadraPi (F12)
 *
 *  While the menu is open the emulation is paused. It allows to:
 *   - choose the hard disk images (applied at restart)
 *   - insert / eject floppy and CD-ROM images (immediately)
 *   - change RAM size and Mac screen size (applied at restart)
 *   - change the CPU speed, sound output and volume, mouse speed (immediately)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"
#include "main.h"
#include "prefs.h"
#include "timer.h"
#include "macos_util.h"
#include "platform.h"
#include "gfx.h"
#include "qp.h"

#include <fatfs/ff.h>
#include <string>
#include <vector>
#include <algorithm>
#include <strings.h>

using std::string;
using std::vector;



// Colors (Mac "Platinum" look)
static const uint32_t C_BG = 0xFFDDDDDD;
static const uint32_t C_FG = 0xFF000000;
static const uint32_t C_DIM = 0xFF707070;
static const uint32_t C_SEL_BG = 0xFF000000;
static const uint32_t C_SEL_FG = 0xFFFFFFFF;
static const uint32_t C_TITLE = 0xFFFFFFFF;
static const uint32_t C_WARN = 0xFFB00000;
static const uint32_t C_ACCENT = 0xFF3355AA;

static const int MAX_DISKS = 4;
static const int VISIBLE_ROWS = 15;
static const int ROW_H = 22;
static const int LIST_Y = 44;

static bool restart_needed = false;

/*
 *  Input inside the menu
 */

enum menu_key { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_ESC, K_MENU, K_PGUP, K_PGDN, K_CHAR };

static int held_key = K_NONE;
static uint64 held_since = 0, last_repeat = 0;
static int mouse_accum = 0;
static int typed_char = 0;

static int hid_to_menu_key(int code)
{
	switch (code) {
		case 0x52: return K_UP;
		case 0x51: return K_DOWN;
		case 0x50: return K_LEFT;
		case 0x4f: return K_RIGHT;
		case 0x28: case 0x58: case 0x2c: return K_ENTER;
		case 0x29: case 0x2a: return K_ESC;
		case 0x4b: return K_PGUP;
		case 0x4e: return K_PGDN;
	}
	return K_NONE;
}

static int menu_hotkey(void)
{
	int k = PrefsFindInt32("menukey");
	return (k > 0 && k < 256) ? k : 0x45;
}

static int read_key(void)
{
	input_event ev;
	while (input_pop(ev)) {
		if (ev.type == EV_KEY_DOWN) {
			if (ev.code == menu_hotkey())
				return K_MENU;
			int k = hid_to_menu_key(ev.code);
			if (k != K_NONE) {
				held_key = k;
				held_since = last_repeat = GetTicks_usec();
				return k;
			}
			if (ev.code >= 0x04 && ev.code <= 0x1d) {
				typed_char = 'a' + ev.code - 0x04;
				return K_CHAR;
			}
		} else if (ev.type == EV_KEY_UP) {
			if (hid_to_menu_key(ev.code) == held_key)
				held_key = K_NONE;
		} else if (ev.type == EV_MOUSE) {
			if (ev.buttons & 1) {
				// wait for release to avoid repeats
				return K_ENTER;
			}
			if (ev.buttons & 2)
				return K_ESC;
			if (ev.wheel)
				return ev.wheel > 0 ? K_UP : K_DOWN;
			mouse_accum += ev.dy;
			if (mouse_accum > 24) { mouse_accum = 0; return K_DOWN; }
			if (mouse_accum < -24) { mouse_accum = 0; return K_UP; }
		}
	}

	// Auto repeat for held navigation keys
	if (held_key != K_NONE && held_key != K_ENTER && held_key != K_ESC) {
		uint64 now = GetTicks_usec();
		if (now - held_since > 400000 && now - last_repeat > 70000) {
			last_repeat = now;
			return held_key;
		}
	}
	return K_NONE;
}

static int wait_key(void)
{
	for (;;) {
		plat_housekeeping();
		int k = read_key();
		if (k != K_NONE)
			return k;
	}
}

// Swallow pending mouse button releases etc.
static void drain_input(void)
{
	input_event ev;
	uint64 end = GetTicks_usec() + 50000;
	while (GetTicks_usec() < end)
		while (input_pop(ev))
			;
	held_key = K_NONE;
	mouse_accum = 0;
}


/*
 *  Drawing
 */

static canvas *cv(void)
{
	return video_overlay_canvas();
}

static void draw_frame(const char *title)
{
	canvas *c = cv();
	cv_fill(c, 0, 0, c->w, c->h, C_BG);
	cv_rect(c, 0, 0, c->w, c->h, C_FG);
	cv_rect(c, 1, 1, c->w - 2, c->h - 2, C_FG);
	// Title bar with Mac style stripes
	cv_fill(c, 2, 2, c->w - 4, 26, C_BG);
	for (int y = 6; y < 26; y += 3)
		cv_fill(c, 8, y, c->w - 16, 1, C_DIM);
	int tw = text_width(title);
	cv_fill(c, (c->w - tw) / 2 - 10, 4, tw + 20, 22, C_BG);
	cv_text(c, (c->w - tw) / 2, 6, title, C_FG);
	cv_fill(c, 2, 28, c->w - 4, 1, C_FG);
}

static void draw_footer(const char *text)
{
	canvas *c = cv();
	cv_fill(c, 2, c->h - 28, c->w - 4, 1, C_FG);
	cv_text(c, 12, c->h - 23, text, C_DIM);
}

static string shorten(const string &s, size_t max)
{
	if (s.size() <= max)
		return s;
	return "..." + s.substr(s.size() - (max - 3));
}

static string base_name(const char *path)
{
	if (!path || !*path)
		return "";
	const char *p = strrchr(path, '/');
	return p ? p + 1 : path;
}


/*
 *  Generic list
 */

struct list_item {
	string label;
	string value;
	bool enabled;
	int tag;
};

static void draw_list(const char *title, const vector<list_item> &items, int sel, int top, const char *footer, const char *status = NULL)
{
	draw_frame(title);
	canvas *c = cv();
	int n = items.size();
	for (int r = 0; r < VISIBLE_ROWS && top + r < n; r++) {
		const list_item &it = items[top + r];
		int y = LIST_Y + r * ROW_H;
		bool is_sel = (top + r == sel);
		if (is_sel)
			cv_fill(c, 8, y - 3, c->w - 16, ROW_H, C_SEL_BG);
		uint32_t fg = is_sel ? C_SEL_FG : (it.enabled ? C_FG : C_DIM);
		cv_text(c, 16, y, it.label.c_str(), fg);
		if (!it.value.empty()) {
			string v = shorten(it.value, 34);
			int w = text_width(v.c_str());
			cv_text(c, c->w - 20 - w, y, v.c_str(), is_sel ? C_SEL_FG : C_ACCENT);
		}
	}
	// Scroll indicators
	if (top > 0)
		cv_text(c, c->w - 24, LIST_Y - 14, "^", C_FG);
	if (top + VISIBLE_ROWS < n)
		cv_text(c, c->w - 24, LIST_Y + VISIBLE_ROWS * ROW_H - 6, "v", C_FG);
	if (status)
		cv_text(c, 16, c->h - 50, status, restart_needed ? C_WARN : C_DIM);
	draw_footer(footer);
	video_overlay_updated();
}

static void fix_scroll(int sel, int &top, int n)
{
	if (sel < top)
		top = sel;
	if (sel >= top + VISIBLE_ROWS)
		top = sel - VISIBLE_ROWS + 1;
	if (top < 0)
		top = 0;
	(void)n;
}

static int next_enabled(const vector<list_item> &items, int sel, int dir)
{
	int n = items.size();
	for (int i = 0; i < n; i++) {
		sel += dir;
		if (sel < 0) sel = n - 1;
		if (sel >= n) sel = 0;
		if (items[sel].enabled)
			return sel;
	}
	return sel;
}


/*
 *  Message box
 */

static void message_box(const char *title, const char *text, bool wait)
{
	draw_frame(title);
	canvas *c = cv();
	int y = 60;
	const char *p = text;
	char line[100];
	while (*p && y < c->h - 60) {
		int n = 0;
		while (*p && *p != '\n' && n < 74)
			line[n++] = *p++;
		line[n] = 0;
		if (*p == '\n')
			p++;
		cv_text(c, 24, y, line, C_FG);
		y += LINE_H;
	}
	draw_footer(wait ? "Premi un tasto per continuare" : "");
	video_overlay_updated();
	if (wait) {
		drain_input();
		wait_key();
	}
}

static bool confirm(const char *title, const char *text)
{
	string t = string(text) + "\n\nInvio = conferma     Esc = annulla";
	message_box(title, t.c_str(), false);
	drain_input();
	for (;;) {
		int k = wait_key();
		if (k == K_ENTER)
			return true;
		if (k == K_ESC || k == K_MENU)
			return false;
	}
}


/*
 *  File browser
 */

struct dir_entry {
	string name;
	bool is_dir;
	uint64 size;
};

static bool by_name(const dir_entry &a, const dir_entry &b)
{
	if (a.is_dir != b.is_dir)
		return a.is_dir;
	return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
}

static void read_dir(const string &path, vector<dir_entry> &out)
{
	out.clear();
	DIR dir;
	FILINFO fi;
	if (f_opendir(&dir, path.c_str()) != FR_OK)
		return;
	while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
		if (fi.fname[0] == '.' || (fi.fattrib & (AM_HID | AM_SYS)))
			continue;
		dir_entry e;
		e.name = fi.fname;
		e.is_dir = (fi.fattrib & AM_DIR) != 0;
		e.size = fi.fsize;
		out.push_back(e);
	}
	f_closedir(&dir);
	std::sort(out.begin(), out.end(), by_name);
}

static string size_str(uint64 s)
{
	char buf[32];
	if (s >= 1024ull * 1024 * 1024)
		snprintf(buf, sizeof(buf), "%.1f GB", s / (1024.0 * 1024 * 1024));
	else if (s >= 1024 * 1024)
		snprintf(buf, sizeof(buf), "%u MB", (unsigned)(s / (1024 * 1024)));
	else
		snprintf(buf, sizeof(buf), "%u KB", (unsigned)(s / 1024));
	return buf;
}

enum { TAG_FILE = 1, TAG_DIR, TAG_UP, TAG_NONE, TAG_USB, TAG_SD, TAG_CREATE };

// Returns chosen path, "" for "none", or "\x01" for cancel
static const string CANCELLED = "\x01";

static string create_image_dialog(const string &dir);

static string browse(const char *title, string dir, bool allow_none, bool allow_create)
{
	int sel = 0, top = 0;
	vector<dir_entry> entries;
	string loaded_dir;
	vector<list_item> items;

	for (;;) {
		if (loaded_dir != dir) {
			read_dir(dir, entries);
			loaded_dir = dir;
			items.clear();
			if (allow_none)
				items.push_back({"[ Nessuno ]", "", true, TAG_NONE});
			if (allow_create)
				items.push_back({"[ Crea nuova immagine disco... ]", "", true, TAG_CREATE});
			bool at_root = (dir == "SD:/" || dir == "USB:/");
			if (!at_root)
				items.push_back({"[ .. cartella superiore ]", "", true, TAG_UP});
			if (plat_usb_storage_present() && dir.compare(0, 3, "USB") != 0)
				items.push_back({"[ Chiavetta USB ]", "", true, TAG_USB});
			if (dir.compare(0, 3, "USB") == 0)
				items.push_back({"[ Scheda SD ]", "", true, TAG_SD});
			for (size_t i = 0; i < entries.size(); i++) {
				if (entries[i].is_dir)
					items.push_back({entries[i].name + "/", "", true, (int)(TAG_DIR | (i << 4))});
				else
					items.push_back({entries[i].name, size_str(entries[i].size), true, (int)(TAG_FILE | (i << 4))});
			}
			if (items.empty())
				items.push_back({"(cartella vuota)", "", false, 0});
			sel = 0;
			top = 0;
			if (!items[0].enabled)
				sel = 0;
		}

		fix_scroll(sel, top, items.size());
		string status = "Cartella: " + shorten(dir, 60);
		draw_list(title, items, sel, top, "Invio = scegli   Esc = indietro   frecce = muovi", status.c_str());

		int k = wait_key();
		int n = items.size();
		switch (k) {
			case K_UP: sel = next_enabled(items, sel, -1); break;
			case K_DOWN: sel = next_enabled(items, sel, 1); break;
			case K_PGUP: sel = std::max(0, sel - VISIBLE_ROWS); break;
			case K_PGDN: sel = std::min(n - 1, sel + VISIBLE_ROWS); break;
			case K_CHAR:
				// jump to the first entry starting with the typed letter
				for (int i = 1; i <= n; i++) {
					int j = (sel + i) % n;
					if (!items[j].label.empty() && tolower(items[j].label[0]) == typed_char) {
						sel = j;
						break;
					}
				}
				break;
			case K_ESC:
			case K_MENU:
				return CANCELLED;
			case K_LEFT: {
				bool at_root = (dir == "SD:/" || dir == "USB:/");
				if (at_root)
					return CANCELLED;
				size_t p = dir.find_last_of('/', dir.size() - 2);
				dir = (p == string::npos) ? dir : dir.substr(0, p + 1);
				break;
			}
			case K_RIGHT:
			case K_ENTER: {
				if (!items[sel].enabled)
					break;
				int tag = items[sel].tag & 15;
				int idx = items[sel].tag >> 4;
				if (tag == TAG_NONE)
					return "";
				if (tag == TAG_CREATE) {
					string p = create_image_dialog(dir);
					if (p != CANCELLED)
						return p;
					loaded_dir.clear();
				} else if (tag == TAG_UP) {
					size_t p = dir.find_last_of('/', dir.size() - 2);
					dir = (p == string::npos) ? dir : dir.substr(0, p + 1);
				} else if (tag == TAG_USB) {
					dir = "USB:/";
				} else if (tag == TAG_SD) {
					dir = QP_DIR "/";
				} else if (tag == TAG_DIR) {
					dir = dir + entries[idx].name + "/";
				} else if (tag == TAG_FILE && k == K_ENTER) {
					string d = dir;
					if (!d.empty() && d[d.size() - 1] == '/')
						d.erase(d.size() - 1);
					return d + "/" + entries[idx].name;
				}
				break;
			}
		}
	}
}

static string create_image_dialog(const string &dir)
{
	static const int sizes_mb[] = {20, 40, 80, 100, 250, 500, 1000, 2000};
	vector<list_item> items;
	for (int i = 0; i < 8; i++) {
		char buf[40];
		snprintf(buf, sizeof(buf), "%d MB", sizes_mb[i]);
		items.push_back({buf, "", true, sizes_mb[i]});
	}
	int sel = 3, top = 0;
	for (;;) {
		draw_list("Nuova immagine disco", items, sel, top, "Invio = crea   Esc = annulla",
				  "Il Mac chiederà di inizializzare il disco al primo avvio.");
		int k = wait_key();
		if (k == K_UP) sel = next_enabled(items, sel, -1);
		else if (k == K_DOWN) sel = next_enabled(items, sel, 1);
		else if (k == K_ESC || k == K_MENU) return CANCELLED;
		else if (k == K_ENTER) break;
	}

	int mb = items[sel].tag;
	string d = dir;
	if (!d.empty() && d[d.size() - 1] == '/')
		d.erase(d.size() - 1);
	char path[300];
	for (int n = 1; n < 100; n++) {
		snprintf(path, sizeof(path), "%s/disco%d-%dMB.hfv", d.c_str(), n, mb);
		FILINFO fi;
		if (f_stat(path, &fi) != FR_OK)
			break;
	}

	message_box("Nuova immagine disco", "Creazione del file in corso...", false);

	FIL f;
	if (f_open(&f, path, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
		message_box("Errore", "Impossibile creare il file.", true);
		return CANCELLED;
	}
	// Clear the first 64 KB (no stale partition data), then extend the file
	static uint8 zero[4096];
	memset(zero, 0, sizeof(zero));
	UINT bw;
	bool ok = true;
	for (int i = 0; i < 16 && ok; i++)
		ok = f_write(&f, zero, sizeof(zero), &bw) == FR_OK && bw == sizeof(zero);
	if (ok)
		ok = f_lseek(&f, (FSIZE_t)mb * 1024 * 1024) == FR_OK && f_tell(&f) == (FSIZE_t)mb * 1024 * 1024;
	f_close(&f);
	if (!ok) {
		f_unlink(path);
		message_box("Errore", "Spazio insufficiente sulla scheda.", true);
		return CANCELLED;
	}
	return path;
}


/*
 *  Settings helpers
 */

static const int ram_choices[] = {8, 16, 32, 64, 128, 256, 512};
static const int cpu_choices[] = {25, 33, 40, 50, 66, 100, 0};
static const struct { int w, h; } res_choices[] = {
	{512, 384}, {640, 480}, {800, 600}, {832, 624}, {1024, 768}, {1152, 870}, {1280, 1024}, {1600, 1200}, {0, 0}
};
static const int NUM_RES = sizeof(res_choices) / sizeof(res_choices[0]);

static int cycle_index(int cur, int n, int dir)
{
	cur += dir;
	if (cur < 0) cur = n - 1;
	if (cur >= n) cur = 0;
	return cur;
}

static string cpu_label(int mhz)
{
	if (mhz == 0)
		return "Massima";
	char buf[48];
	if (mhz == 25)
		snprintf(buf, sizeof(buf), "25 MHz (originale)");
	else
		snprintf(buf, sizeof(buf), "%d MHz", mhz);
	return buf;
}

static void get_disks(vector<string> &disks)
{
	disks.clear();
	const char *s;
	for (int i = 0; (s = PrefsFindString("disk", i)) != NULL; i++)
		disks.push_back(s);
}

static void set_disks(const vector<string> &disks)
{
	while (PrefsFindString("disk"))
		PrefsRemoveItem("disk");
	for (size_t i = 0; i < disks.size(); i++)
		if (!disks[i].empty())
			PrefsAddString("disk", disks[i].c_str());
}

static string date_string(void)
{
	time_t t = (time_t)((int64)TimerDateTime() - 2082844800LL);
	struct tm tmv;
	gmtime_r(&t, &tmv);
	char buf[40];
	snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900, tmv.tm_hour, tmv.tm_min);
	return buf;
}


/*
 *  Main menu
 */

enum {
	M_HD0 = 100, M_FD0 = 200, M_CD = 300,
	M_RAM = 1, M_CPU, M_SOUND, M_SOUNDOUT, M_VOLUME, M_RES, M_DEPTH, M_MOUSE, M_KEYS,
	M_DATE, M_APPLY, M_RESET_PRAM, M_CLOSE, M_INFO
};

static void build_main(vector<list_item> &items)
{
	items.clear();
	vector<string> disks;
	get_disks(disks);

	for (int i = 0; i < MAX_DISKS; i++) {
		char lbl[40];
		snprintf(lbl, sizeof(lbl), "Disco rigido %d", i + 1);
		string v = i < (int)disks.size() ? base_name(disks[i].c_str()) : "(nessuno)";
		items.push_back({lbl, v, true, M_HD0 + i});
	}
	for (int i = 0; i < 2; i++) {
		char lbl[40];
		snprintf(lbl, sizeof(lbl), "Floppy %d", i + 1);
		const char *img = sys_slot_image(SLOT_FLOPPY0 + i);
		items.push_back({lbl, img ? base_name(img) : "(vuoto)", true, M_FD0 + i});
	}
	{
		const char *img = sys_slot_image(SLOT_CDROM);
		items.push_back({"CD-ROM", img ? base_name(img) : "(vuoto)", true, M_CD});
	}

	char buf[64];
	snprintf(buf, sizeof(buf), "%d MB", PrefsFindInt32("ramsize") >> 20);
	items.push_back({"Memoria RAM  (al riavvio)", buf, true, M_RAM});
	items.push_back({"Velocità CPU (68040)", cpu_label(PrefsFindInt32("cpuspeed")), true, M_CPU});
	items.push_back({"Audio", PrefsFindBool("nosound") ? "Disattivato" : "Attivo", true, M_SOUND});
	items.push_back({"Uscita audio", PrefsFindInt32("soundout") == SOUND_OUT_JACK ? "Jack 3,5 mm" : "HDMI", true, M_SOUNDOUT});
	snprintf(buf, sizeof(buf), "%d%%", PrefsFindInt32("volume"));
	items.push_back({"Volume", buf, true, M_VOLUME});

	int w = 640, h = 480;
	const char *scr = PrefsFindString("screen");
	if (scr)
		sscanf(scr, "win/%d/%d", &w, &h);
	if (w <= 0)
		snprintf(buf, sizeof(buf), "Schermo intero");
	else
		snprintf(buf, sizeof(buf), "%d x %d", w, h);
	items.push_back({"Risoluzione Mac  (al riavvio)", buf, true, M_RES});
	int d = PrefsFindInt32("displaycolordepth");
	const char *dn = d == 1 ? "Bianco e nero" : d == 2 ? "4 colori" : d == 4 ? "16 colori" :
					 d == 16 ? "Migliaia" : d == 32 ? "Milioni" : "256 colori";
	items.push_back({"Colori all'avvio  (al riavvio)", dn, true, M_DEPTH});
	snprintf(buf, sizeof(buf), "%d%%", PrefsFindInt32("mousespeed"));
	items.push_back({"Velocità mouse", buf, true, M_MOUSE});
	items.push_back({"Tasto Alt sinistro", PrefsFindBool("swap_opt_cmd") ? "Comando" : "Opzione", true, M_KEYS});
	items.push_back({"Data e ora del Mac", date_string(), true, M_DATE});
	items.push_back({"Azzera PRAM  (al riavvio)", "", true, M_RESET_PRAM});
	items.push_back({restart_needed ? "> Applica e riavvia <" : "Riavvia il Macintosh", "", true, M_APPLY});
	items.push_back({"Chiudi menu", "F12 / Esc", true, M_CLOSE});
}

static void menu_status(char *buf, size_t n)
{
	uint32 mips = qp_measured_mips_x10();
	snprintf(buf, n, "%s CPU Pi %u MHz, %u C - 68040 ~%u.%u MIPS",
			 restart_needed ? "Riavvio necessario!" : "Quadra 900 -",
			 plat_arm_clock_mhz(), plat_cpu_temp(), mips / 10, mips % 10);
}

static void handle_disk(int idx)
{
	vector<string> disks;
	get_disks(disks);
	string start = QP_HD_DIR "/";
	if (idx < (int)disks.size()) {
		size_t p = disks[idx].find_last_of('/');
		if (p != string::npos)
			start = disks[idx].substr(0, p + 1);
	}
	char title[40];
	snprintf(title, sizeof(title), "Disco rigido %d", idx + 1);
	string r = browse(title, start, true, true);
	if (r == CANCELLED)
		return;
	if (idx < (int)disks.size())
		disks[idx] = r;
	else if (!r.empty())
		disks.push_back(r);
	set_disks(disks);
	SavePrefs();
	restart_needed = true;
}

static void handle_removable(int slot)
{
	const char *img = sys_slot_image(slot);
	const char *name = slot == SLOT_CDROM ? "CD-ROM" : (slot == SLOT_FLOPPY0 ? "Floppy 1" : "Floppy 2");
	if (img) {
		string msg = string("Nell'unità c'è ") + base_name(img) + ".\n\n"
			"Di solito si espelle dal Mac (trascina l'icona nel Cestino\n"
			"o Comando-E). Espellerlo da qui mentre il Mac lo usa\n"
			"può causare errori.\n\nVuoi espellerlo comunque?";
		if (confirm(name, msg.c_str()))
			sys_slot_eject(slot);
		return;
	}
	string start = slot == SLOT_CDROM ? QP_CD_DIR "/" : QP_FD_DIR "/";
	string r = browse(name, start, false, false);
	if (r == CANCELLED || r.empty())
		return;
	if (!sys_slot_insert(slot, r.c_str()))
		message_box("Errore", "Impossibile inserire l'immagine.", true);
}

static void change_value(int tag, int dir)
{
	switch (tag) {
		case M_RAM: {
			int cur = PrefsFindInt32("ramsize") >> 20, i = 0;
			while (i < 7 && ram_choices[i] < cur) i++;
			if (i >= 7) i = 6;
			int maxmb = (int)(plat_heap_free() >> 20) + (cur) - 64;
			int ni = i;
			for (int t = 0; t < 7; t++) {
				ni = cycle_index(ni, 7, dir);
				if (ram_choices[ni] <= maxmb || ram_choices[ni] <= 64)
					break;
			}
			PrefsReplaceInt32("ramsize", ram_choices[ni] << 20);
			restart_needed = true;
			break;
		}
		case M_CPU: {
			int cur = PrefsFindInt32("cpuspeed"), i = 0;
			while (i < 7 && cpu_choices[i] != cur) i++;
			if (i >= 7) i = 6;
			i = cycle_index(i, 7, dir);
			PrefsReplaceInt32("cpuspeed", cpu_choices[i]);
			qp_set_cpu_speed(cpu_choices[i]);
			break;
		}
		case M_SOUND:
			PrefsReplaceBool("nosound", !PrefsFindBool("nosound"));
			audio_apply_prefs();
			break;
		case M_SOUNDOUT:
			PrefsReplaceInt32("soundout", PrefsFindInt32("soundout") == SOUND_OUT_JACK ? SOUND_OUT_HDMI : SOUND_OUT_JACK);
			audio_apply_prefs();
			break;
		case M_VOLUME: {
			int v = PrefsFindInt32("volume") + dir * 10;
			if (v < 0) v = 0;
			if (v > 100) v = 100;
			PrefsReplaceInt32("volume", v);
			audio_apply_prefs();
			break;
		}
		case M_RES: {
			int w = 640, h = 480;
			const char *scr = PrefsFindString("screen");
			if (scr)
				sscanf(scr, "win/%d/%d", &w, &h);
			int i = 0;
			while (i < NUM_RES && !(res_choices[i].w == w && res_choices[i].h == h)) i++;
			if (i >= NUM_RES) i = 1;
			const plat_fb *fb = plat_framebuffer();
			for (int t = 0; t < NUM_RES; t++) {
				i = cycle_index(i, NUM_RES, dir);
				if ((unsigned)res_choices[i].w <= fb->width && (unsigned)res_choices[i].h <= fb->height)
					break;
			}
			char buf[40];
			snprintf(buf, sizeof(buf), "win/%d/%d", res_choices[i].w, res_choices[i].h);
			PrefsReplaceString("screen", buf);
			restart_needed = true;
			break;
		}
		case M_DEPTH: {
			static const int depths[] = {1, 2, 4, 8, 16, 32};
			int cur = PrefsFindInt32("displaycolordepth"), i = 0;
			while (i < 6 && depths[i] != cur) i++;
			if (i >= 6) i = 3;
			i = cycle_index(i, 6, dir);
			PrefsReplaceInt32("displaycolordepth", depths[i]);
			restart_needed = true;
			break;
		}
		case M_MOUSE: {
			int v = PrefsFindInt32("mousespeed") + dir * 25;
			if (v < 25) v = 25;
			if (v > 400) v = 400;
			PrefsReplaceInt32("mousespeed", v);
			input_set_mouse_speed(v);
			break;
		}
		case M_KEYS:
			PrefsReplaceBool("swap_opt_cmd", !PrefsFindBool("swap_opt_cmd"));
			restart_needed = true;
			break;
		case M_DATE: {
			// Left/right: one hour; the exact time can be set in the Mac control panel
			TimerSetDateTime(TimerDateTime() + dir * 3600);
			break;
		}
		default:
			return;
	}
	SavePrefs();
}

void menu_run(void)
{
	audio_mute_for_menu(true);
	drain_input();
	video_set_menu_visible(true);

	vector<list_item> items;
	int sel = 0, top = 0;
	bool done = false;

	while (!done) {
		build_main(items);
		if (sel >= (int)items.size())
			sel = items.size() - 1;
		fix_scroll(sel, top, items.size());
		char status[120];
		menu_status(status, sizeof(status));
		draw_list("QuadraPi - Macintosh Quadra 900", items, sel, top,
				  "Frecce = scegli/modifica   Invio = apri   Esc/F12 = torna al Mac", status);

		int k = K_NONE;
		uint64 redraw_at = GetTicks_usec() + 1000000;
		while (k == K_NONE && GetTicks_usec() < redraw_at) {
			plat_housekeeping();
			k = read_key();
		}
		int tag = items[sel].tag;
		switch (k) {
			case K_UP: sel = next_enabled(items, sel, -1); break;
			case K_DOWN: sel = next_enabled(items, sel, 1); break;
			case K_PGUP: sel = std::max(0, sel - VISIBLE_ROWS); break;
			case K_PGDN: sel = std::min((int)items.size() - 1, sel + VISIBLE_ROWS); break;
			case K_ESC:
			case K_MENU:
				done = true;
				break;
			case K_LEFT:
			case K_RIGHT:
				change_value(tag, k == K_LEFT ? -1 : 1);
				break;
			case K_ENTER:
				if (tag >= M_HD0 && tag < M_HD0 + MAX_DISKS)
					handle_disk(tag - M_HD0);
				else if (tag == M_FD0 || tag == M_FD0 + 1)
					handle_removable(SLOT_FLOPPY0 + tag - M_FD0);
				else if (tag == M_CD)
					handle_removable(SLOT_CDROM);
				else if (tag == M_CLOSE)
					done = true;
				else if (tag == M_APPLY) {
					if (confirm("Riavvio", "Il Macintosh verrà riavviato.\n"
								"Salva prima il tuo lavoro sul Mac!\n\nContinuare?")) {
						SavePrefs();
						XPRAMSaveIfChanged();
						TimerSaveClock();
						SysFlushAll();
						plat_reboot();
					}
				} else if (tag == M_RESET_PRAM) {
					if (confirm("Azzera PRAM", "Le impostazioni della PRAM (disco di avvio,\n"
								"suoni, colori...) torneranno ai valori iniziali\n"
								"al prossimo riavvio.")) {
						ZapPRAMOnRestart();
						restart_needed = true;
					}
				} else
					change_value(tag, 1);
				drain_input();
				break;
		}
	}

	video_set_menu_visible(false);
	drain_input();
	audio_mute_for_menu(false);
}


/*
 *  Notices (warnings / errors raised by the emulator)
 */

void notice_show(const char *text, bool error)
{
	if (!plat_framebuffer() || !video_overlay_canvas()->px)
		return;
	video_set_menu_visible(true);
	draw_frame(error ? "QuadraPi - errore" : "QuadraPi - avviso");
	canvas *c = cv();
	int y = 60;
	const char *p = text;
	char line[100];
	while (*p && y < c->h - 60) {
		int n = 0;
		while (*p && *p != '\n' && n < 74)
			line[n++] = *p++;
		line[n] = 0;
		if (*p == '\n')
			p++;
		cv_text(c, 24, y, line, error ? C_WARN : C_FG);
		y += LINE_H;
	}
	draw_footer("Premi un tasto (continua da solo tra 10 secondi)");
	video_overlay_updated();

	uint64 end = GetTicks_usec() + 10000000;
	drain_input();
	while (GetTicks_usec() < end) {
		plat_housekeeping();
		if (read_key() != K_NONE)
			break;
	}
	video_set_menu_visible(false);
}
