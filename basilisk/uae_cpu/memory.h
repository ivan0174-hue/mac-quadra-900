/*
 * UAE - The Un*x Amiga Emulator
 *
 * memory management
 *
 * Copyright 1995 Bernd Schmidt
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef UAE_MEMORY_H
#define UAE_MEMORY_H

#if !DIRECT_ADDRESSING && !REAL_ADDRESSING

/* Enabling this adds one additional native memory reference per 68k memory
 * access, but saves one shift (on the x86). Enabling this is probably
 * better for the cache. My favourite benchmark (PP2) doesn't show a
 * difference, so I leave this enabled. */

#if 1 || defined SAVE_MEMORY
#define SAVE_MEMORY_BANKS
#endif

typedef uae_u32 (REGPARAM2 *mem_get_func)(uaecptr) REGPARAM;
typedef void (REGPARAM2 *mem_put_func)(uaecptr, uae_u32) REGPARAM;
typedef uae_u8 *(REGPARAM2 *xlate_func)(uaecptr) REGPARAM;

#undef DIRECT_MEMFUNCS_SUCCESSFUL

#ifndef CAN_MAP_MEMORY
#undef USE_COMPILER
#endif

#if defined(USE_COMPILER) && !defined(USE_MAPPED_MEMORY)
#define USE_MAPPED_MEMORY
#endif

typedef struct {
    /* These ones should be self-explanatory... */
    mem_get_func lget, wget, bget;
    mem_put_func lput, wput, bput;
    /* Use xlateaddr to translate an Amiga address to a uae_u8 * that can
     * be used to address memory without calling the wget/wput functions.
     * This doesn't work for all memory banks, so this function may call
     * abort(). */
    xlate_func xlateaddr;
} addrbank;

extern uae_u8 filesysory[65536];

extern addrbank ram_bank;	// Mac RAM
extern addrbank rom_bank;	// Mac ROM
extern addrbank frame_bank;	// Frame buffer

/* Default memory access functions */

extern uae_u8 *REGPARAM2 default_xlate(uaecptr addr) REGPARAM;

#define bankindex(addr) (((uaecptr)(addr)) >> 16)

#ifdef SAVE_MEMORY_BANKS
extern addrbank *mem_banks[65536];
#define get_mem_bank(addr) (*mem_banks[bankindex(addr)])
#define put_mem_bank(addr, b) (mem_banks[bankindex(addr)] = (b))
#else
extern addrbank mem_banks[65536];
#define get_mem_bank(addr) (mem_banks[bankindex(addr)])
#define put_mem_bank(addr, b) (mem_banks[bankindex(addr)] = *(b))
#endif

extern void memory_init(void);
extern void map_banks(addrbank *bank, int first, int count);

#ifndef NO_INLINE_MEMORY_ACCESS

#define longget(addr) (call_mem_get_func(get_mem_bank(addr).lget, addr))
#define wordget(addr) (call_mem_get_func(get_mem_bank(addr).wget, addr))
#define byteget(addr) (call_mem_get_func(get_mem_bank(addr).bget, addr))
#define longput(addr,l) (call_mem_put_func(get_mem_bank(addr).lput, addr, l))
#define wordput(addr,w) (call_mem_put_func(get_mem_bank(addr).wput, addr, w))
#define byteput(addr,b) (call_mem_put_func(get_mem_bank(addr).bput, addr, b))

#else

extern uae_u32 longget(uaecptr addr);
extern uae_u32 wordget(uaecptr addr);
extern uae_u32 byteget(uaecptr addr);
extern void longput(uaecptr addr, uae_u32 l);
extern void wordput(uaecptr addr, uae_u32 w);
extern void byteput(uaecptr addr, uae_u32 b);

#endif

#ifndef MD_HAVE_MEM_1_FUNCS

#define longget_1 longget
#define wordget_1 wordget
#define byteget_1 byteget
#define longput_1 longput
#define wordput_1 wordput
#define byteput_1 byteput

#endif

#endif /* !DIRECT_ADDRESSING && !REAL_ADDRESSING */

#if REAL_ADDRESSING
const uintptr MEMBaseDiff = 0;
#elif DIRECT_ADDRESSING
extern uintptr MEMBaseDiff;
#endif

#if REAL_ADDRESSING || DIRECT_ADDRESSING
/*
 *  QuadraPi: bounds-checked direct addressing.
 *  Valid Mac addresses are [0, MacMemLimit). Accesses outside this window
 *  (e.g. to real Macintosh hardware registers) read as 0 and writes are
 *  ignored, like Basilisk's SIGSEGV recovery does on Unix hosts.
 */
extern uae_u32 MacMemLimit;
extern uae_u8 *MacDummyPage;
extern uae_u32 bad_mem_get(uaecptr addr, int size);
extern void bad_mem_put(uaecptr addr, uae_u32 v, int size);
#define MEM_IN_RANGE(addr, n) (__builtin_expect((uae_u32)(addr) <= MacMemLimit - (n), 1))

static __inline__ uae_u8 *do_get_real_address(uaecptr addr)
{
	return (uae_u8 *)MEMBaseDiff + addr;
}
static __inline__ uae_u32 do_get_virtual_address(uae_u8 *addr)
{
	return (uintptr)addr - MEMBaseDiff;
}
static __inline__ uae_u32 get_long(uaecptr addr)
{
	if (MEM_IN_RANGE(addr, 4))
		return do_get_mem_long((uae_u32 *)do_get_real_address(addr));
	return bad_mem_get(addr, 4);
}
static __inline__ uae_u32 get_word(uaecptr addr)
{
	if (MEM_IN_RANGE(addr, 2))
		return do_get_mem_word((uae_u16 *)do_get_real_address(addr));
	return bad_mem_get(addr, 2);
}
static __inline__ uae_u32 get_byte(uaecptr addr)
{
	if (MEM_IN_RANGE(addr, 1))
		return do_get_mem_byte((uae_u8 *)do_get_real_address(addr));
	return bad_mem_get(addr, 1);
}
static __inline__ void put_long(uaecptr addr, uae_u32 l)
{
	if (MEM_IN_RANGE(addr, 4))
		do_put_mem_long((uae_u32 *)do_get_real_address(addr), l);
	else
		bad_mem_put(addr, l, 4);
}
static __inline__ void put_word(uaecptr addr, uae_u32 w)
{
	if (MEM_IN_RANGE(addr, 2))
		do_put_mem_word((uae_u16 *)do_get_real_address(addr), w);
	else
		bad_mem_put(addr, w, 2);
}
static __inline__ void put_byte(uaecptr addr, uae_u32 b)
{
	if (MEM_IN_RANGE(addr, 1))
		do_put_mem_byte((uae_u8 *)do_get_real_address(addr), b);
	else
		bad_mem_put(addr, b, 1);
}
static __inline__ uae_u8 *get_real_address(uaecptr addr)
{
	if (MEM_IN_RANGE(addr, 1))
		return do_get_real_address(addr);
	return MacDummyPage;
}
static __inline__ uae_u32 get_virtual_address(uae_u8 *addr)
{
	return do_get_virtual_address(addr);
}
#else
static __inline__ uae_u32 get_long(uaecptr addr)
{
    return longget_1(addr);
}
static __inline__ uae_u32 get_word(uaecptr addr)
{
    return wordget_1(addr);
}
static __inline__ uae_u32 get_byte(uaecptr addr)
{
    return byteget_1(addr);
}
static __inline__ void put_long(uaecptr addr, uae_u32 l)
{
    longput_1(addr, l);
}
static __inline__ void put_word(uaecptr addr, uae_u32 w)
{
    wordput_1(addr, w);
}
static __inline__ void put_byte(uaecptr addr, uae_u32 b)
{
    byteput_1(addr, b);
}
static __inline__ uae_u8 *get_real_address(uaecptr addr)
{
    return get_mem_bank(addr).xlateaddr(addr);
}
/* gb-- deliberately not implemented since it shall not be used... */
extern uae_u32 get_virtual_address(uae_u8 *addr);
#endif /* DIRECT_ADDRESSING || REAL_ADDRESSING */

#endif /* MEMORY_H */

