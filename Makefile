#
# Makefile - QuadraPi (Basilisk II bare metal on Raspberry Pi 4, Circle)
#
# Requires circle-stdlib configured with:
#   ./configure -r 4 -p aarch64-none-elf- --kernel-max-size 32 \
#       -o ARM_ALLOW_MULTI_CORE -o KERNEL_STACK_SIZE=0x100000
# and FF_USE_FASTSEEK=1 in libs/circle/addon/fatfs/ffconf.h
#
# Usage: make STDLIBHOME=/path/to/circle-stdlib
#

STDLIBHOME ?= ../circle-stdlib

include $(STDLIBHOME)/Config.mk

B2 = basilisk
UAE = basilisk/uae_cpu

OBJS = \
	src/main.o src/kernel.o src/gfx.o src/main_circle.o src/timer_circle.o \
	src/sys_circle.o src/prefs_circle.o src/xpram_circle.o src/video_circle.o \
	src/audio_circle.o src/input_circle.o src/menu.o src/user_strings_unix.o \
	src/extfs_circle.o \
	$(B2)/main.o $(B2)/prefs.o $(B2)/prefs_items.o $(B2)/rom_patches.o $(B2)/slot_rom.o \
	$(B2)/rsrc_patches.o $(B2)/emul_op.o $(B2)/macos_util.o $(B2)/xpram.o $(B2)/timer.o \
	$(B2)/adb.o $(B2)/serial.o $(B2)/ether.o $(B2)/sony.o $(B2)/disk.o $(B2)/cdrom.o \
	$(B2)/scsi.o $(B2)/video.o $(B2)/audio.o $(B2)/extfs.o $(B2)/user_strings.o \
	$(B2)/clip_dummy.o $(B2)/ether_dummy.o $(B2)/scsi_dummy.o $(B2)/serial_dummy.o \
	$(UAE)/basilisk_glue.o $(UAE)/memory.o $(UAE)/newcpu.o $(UAE)/readcpu.o \
	$(UAE)/fpu/fpu_uae.o $(UAE)/cpustbl.o $(UAE)/cpudefs.o \
	$(UAE)/cpuemu1.o $(UAE)/cpuemu2.o $(UAE)/cpuemu3.o $(UAE)/cpuemu4.o \
	$(UAE)/cpuemu5.o $(UAE)/cpuemu6.o $(UAE)/cpuemu7.o $(UAE)/cpuemu8.o

TARGET = kernel8-rpi4
CHECK_DEPS = 0

include $(CIRCLEHOME)/Rules.mk

OPTIMIZE = -O2
WARNINGS = -Wall -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable \
	-Wno-sign-compare -Wno-parentheses -Wno-unused-value -Wno-misleading-indentation \
	-Wno-format -Wno-narrowing -Wno-deprecated-declarations -Wno-strict-aliasing \
	-Wno-maybe-uninitialized -Wno-class-memaccess -Wno-stringop-truncation \
	-Wno-unused-label -Wno-register -Wno-dangling-pointer

INCLUDE := $(CIRCLE_STDLIB_INCLUDES) -Isrc -I$(B2)/include -I$(UAE) -I$(B2) $(INCLUDE)
DEFINE += -DFPU_UAE -DDIRECT_ADDRESSING=1 -DQUADRAPI
CPPFLAGS += -fwrapv -fno-strict-aliasing -std=gnu++17
CFLAGS += -fwrapv

LIBS := $(CIRCLE_STDLIB_LIBS) $(CIRCLEHOME)/lib/sound/libsound.a

# The generated CPU emulation is compiled in 8 parts
$(UAE)/cpuemu%.o: $(UAE)/cpuemu.cpp
	@echo "  CPP   $@"
	@$(CPP) $(CPPFLAGS) -DPART_$* -c -o $@ $<


