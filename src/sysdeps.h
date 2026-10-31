/*
 *  sysdeps.h - System dependent definitions for QuadraPi
 *              (Basilisk II running bare metal on Raspberry Pi 4 via Circle)
 *
 *  Based on Unix/sysdeps.h from Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef SYSDEPS_H
#define SYSDEPS_H

#include "config.h"
#include "user_strings_unix.h"

#include <sys/types.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>

/* Network byte order helpers (no socket headers needed) */
#ifndef htons
static inline uint16_t qp_htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t qp_htonl(uint32_t v) { return __builtin_bswap32(v); }
#define htons(x) qp_htons(x)
#define ntohs(x) qp_htons(x)
#define htonl(x) qp_htonl(x)
#define ntohl(x) qp_htonl(x)
#endif

/* Mac and host address space are distinct */
#define REAL_ADDRESSING 0

/* Using 68k emulator */
#define EMULATED_68K 1

/* The m68k emulator uses a prefetch buffer ? */
#define USE_PREFETCH_BUFFER 0

#define ROM_IS_WRITE_PROTECTED 0
#define USE_SCRATCHMEM_SUBTERFUGE 1

/* ExtFS (shared folder on SD card) */
#define SUPPORTS_EXTFS 1

/* Data types */
typedef unsigned char uint8;
typedef signed char int8;
typedef unsigned short uint16;
typedef short int16;
typedef unsigned int uint32;
typedef int int32;
#ifndef _UINT64
typedef unsigned long uint64;
#define _UINT64
#endif
typedef long int64;
#define VAL64(a) (a ## l)
#define UVAL64(a) (a ## ul)
typedef uint64 uintptr;
typedef int64 intptr;

typedef off_t loff_t;

/* Time data type for Time Manager emulation */
typedef struct timespec tm_time_t;

#define UNKNOWN_FLOAT_FORMAT 0
#define IEEE_FLOAT_FORMAT 1
#define VAX_FLOAT_FORMAT 2
#define IBM_FLOAT_FORMAT 3
#define C4X_FLOAT_FORMAT 4

/* UAE CPU data types */
#define uae_s8 int8
#define uae_u8 uint8
#define uae_s16 int16
#define uae_u16 uint16
#define uae_s32 int32
#define uae_u32 uint32
#define uae_s64 int64
#define uae_u64 uint64
typedef uae_u32 uaecptr;

/* AArch64 handles unaligned accesses to normal memory */
#define CPU_CAN_ACCESS_UNALIGNED

/* Timing functions */
extern uint64 GetTicks_usec(void);
extern void Delay_usec(uint64 usec);

/* Spinlocks: everything Basilisk-related runs on one core */
typedef volatile int spinlock_t;
static const spinlock_t SPIN_LOCK_UNLOCKED = 0;
static inline void spin_lock(spinlock_t *lock) {}
static inline void spin_unlock(spinlock_t *lock) {}
static inline int spin_trylock(spinlock_t *lock) { return 1; }

/* Little-endian AArch64: use byte-reverse instructions */
static inline uae_u32 do_get_mem_long(uae_u32 *a) { return __builtin_bswap32(*a); }
static inline uae_u32 do_get_mem_word(uae_u16 *a) { return __builtin_bswap16(*a); }
static inline void do_put_mem_long(uae_u32 *a, uae_u32 v) { *a = __builtin_bswap32(v); }
static inline void do_put_mem_word(uae_u16 *a, uae_u32 v) { *a = __builtin_bswap16((uae_u16)v); }
#define HAVE_OPTIMIZED_BYTESWAP_32
static inline uae_u32 do_byteswap_32(uae_u32 v) { return __builtin_bswap32(v); }
#define HAVE_OPTIMIZED_BYTESWAP_16
static inline uae_u32 do_byteswap_16(uae_u32 v) { return __builtin_bswap16((uae_u16)v); }

#define do_get_mem_byte(a) ((uae_u32)*((uae_u8 *)(a)))
#define do_put_mem_byte(a, v) (*(uae_u8 *)(a) = (v))

#define call_mem_get_func(func, addr) ((*func)(addr))
#define call_mem_put_func(func, addr, v) ((*func)(addr, v))
#define __inline__ inline
#define CPU_EMU_SIZE 0
#undef NO_INLINE_MEMORY_ACCESS
#undef MD_HAVE_MEM_1_FUNCS
#define ENUMDECL typedef enum
#define ENUMNAME(name) name
#define write_log printf

#define ASM_SYM(a)

#ifndef REGPARAM
# define REGPARAM
#endif
#define REGPARAM2

#ifndef UNUSED
#define UNUSED(x) ((void)x)
#endif

#ifndef unlikely
#define unlikely(x)  __builtin_expect(!!(x), 0)
#endif
#ifndef likely
#define likely(x)    __builtin_expect(!!(x), 1)
#endif
#define ALWAYS_INLINE   inline __attribute__((always_inline))
#define memptr uint32

/* No precise (threaded) Time Manager; timer tasks are checked from the CPU loop */
#define PRECISE_TIMING 0

#endif
