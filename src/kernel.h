/*
 *  kernel.h - Circle kernel for QuadraPi
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef KERNEL_H
#define KERNEL_H

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/nulldevice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/serial.h>
#include <circle/cputhrottle.h>
#include <circle/bcmframebuffer.h>
#include <circle/multicore.h>
#include <circle/memory.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/input/mouse.h>
#include <circle/input/console.h>
#include <circle/sound/soundbasedevice.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include <circle/types.h>

// Device that sends log text to the serial port and keeps the last lines in RAM
class CLogTee : public CDevice
{
public:
	CLogTee (void) : m_pSerial (0), m_nHead (0) { m_Ring[0] = '\0'; }
	void SetSerial (CDevice *pSerial) { m_pSerial = pSerial; }
	int Write (const void *pBuffer, size_t nCount) override;
	// Copies the tail of the log (at most nMax bytes) into pOut
	void GetTail (char *pOut, size_t nMax) const;
private:
	CDevice *m_pSerial;
	static const unsigned RingSize = 4096;
	char m_Ring[RingSize];
	unsigned m_nHead;
	bool m_bWrapped = false;
};

class CVideoCore : public CMultiCoreSupport
{
public:
	CVideoCore (CMemorySystem *pMemorySystem) : CMultiCoreSupport (pMemorySystem) {}
	void Run (unsigned nCore) override;
};

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);
	void Run (void);

	static CKernel *Get (void) { return s_pThis; }

	// Services used by platform.h functions
	void Housekeeping (void);
	void Fatal (const char *pMsg);
	void Halt (void);
	bool SoundOpen (int nOutput, unsigned nRate);
	void SoundClose (void);
	unsigned SoundFramesFree (void);
	void SoundWrite (const s16 *pStereo, unsigned nFrames);
	unsigned SoundRate (void) const { return m_nSoundRate; }
	unsigned CPUTemp (void) { return m_CPUThrottle.GetTemperature (); }
	unsigned ARMClockMHz (void) { return m_CPUThrottle.GetClockRate () / 1000000; }
	bool USBStoragePresent (void) const { return m_bUSBMounted; }
	CLogTee &LogTee (void) { return m_LogTee; }

private:
	bool InitFrameBuffer (void);
	void UpdateUSBDevices (void);
	void DrawTextScreen (const char *pTitle, const char *pBody, u32 nColor);

	static void KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6]);
	static void MouseStatusHandler (unsigned nButtons, int nDisplacementX, int nDisplacementY, int nWheelMove);
	static void KeyboardRemovedHandler (CDevice *pDevice, void *pContext);
	static void MouseRemovedHandler (CDevice *pDevice, void *pContext);

private:
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CNullDevice		m_NullDevice;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CSerialDevice		m_Serial;
	CLogTee			m_LogTee;
	CLogger			m_Logger;
	CCPUThrottle		m_CPUThrottle;
	CUSBHCIDevice		m_USBHCI;
	CEMMCDevice		m_EMMC;
	CConsole		m_Console;
	CVideoCore		m_VideoCore;

	FATFS			m_SDFileSystem;
	FATFS			m_USBFileSystem;
	bool			m_bUSBMounted;

	CBcmFrameBuffer		*m_pFrameBuffer;

	CUSBKeyboardDevice	*m_pKeyboard;
	CMouseDevice		*m_pMouse;

	CSoundBaseDevice	*m_pSound;
	unsigned		m_nSoundRate;

	unsigned		m_nLastHousekeeping;

	static CKernel *s_pThis;
};

#endif
