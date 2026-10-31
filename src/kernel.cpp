/*
 *  kernel.cpp - Circle kernel for QuadraPi
 *
 *  Core 0: devices, USB, SD card, sound and the 68k emulation
 *  Core 1: display refresh (Mac frame buffer -> HDMI) and menu overlay
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "kernel.h"
#include "platform.h"
#include "gfx.h"

#include <circle/bcmpropertytags.h>
#include <circle/startup.h>
#include <circle/string.h>
#include <circle/util.h>
#include <circle/machineinfo.h>
#include <circle/synchronize.h>
#include <circle/sound/pwmsoundbasedevice.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <circle_glue.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char FromKernel[] = "quadrapi";

CKernel *CKernel::s_pThis = 0;

/*
 *  Log tee
 */

int CLogTee::Write (const void *pBuffer, size_t nCount)
{
	if (m_pSerial)
		m_pSerial->Write (pBuffer, nCount);

	const char *p = (const char *) pBuffer;
	for (size_t i = 0; i < nCount; i++)
	{
		m_Ring[m_nHead++] = p[i];
		if (m_nHead >= RingSize)
		{
			m_nHead = 0;
			m_bWrapped = true;
		}
	}
	return (int) nCount;
}

void CLogTee::GetTail (char *pOut, size_t nMax) const
{
	if (nMax == 0)
		return;

	size_t nAvail = m_bWrapped ? RingSize : m_nHead;
	size_t nCopy = nAvail < nMax - 1 ? nAvail : nMax - 1;
	size_t nStart = (m_nHead + RingSize - nCopy) % RingSize;
	for (size_t i = 0; i < nCopy; i++)
		pOut[i] = m_Ring[(nStart + i) % RingSize];
	pOut[nCopy] = '\0';
}

/*
 *  Secondary cores
 */

void CVideoCore::Run (unsigned nCore)
{
	if (nCore == 1)
		video_core1_main ();

	// Cores 2 and 3 are not used
}

/*
 *  Kernel
 */

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_CPUThrottle (CPUSpeedMaximum),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),
	m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED),
	m_Console (&m_NullDevice, &m_LogTee),
	m_VideoCore (CMemorySystem::Get ()),
	m_bUSBMounted (false),
	m_pFrameBuffer (0),
	m_pKeyboard (0),
	m_pMouse (0),
	m_pSound (0),
	m_nSoundRate (0),
	m_nLastHousekeeping (0)
{
	s_pThis = this;
	m_ActLED.Blink (2);
}

CKernel::~CKernel (void)
{
	s_pThis = 0;
}

boolean CKernel::Initialize (void)
{
	if (!m_Interrupt.Initialize ())
		return FALSE;

	if (m_Serial.Initialize (115200))
		m_LogTee.SetSerial (&m_Serial);

	if (!m_Logger.Initialize (&m_LogTee))
		return FALSE;

	if (!m_Timer.Initialize ())
		return FALSE;

	// Display first, so that errors can be shown on screen
	if (!InitFrameBuffer ())
		return FALSE;

	gfx_clear (0xFF000000);
	gfx_text_centered (m_pFrameBuffer->GetHeight () / 2 - 10,
			   "QuadraPi - avvio in corso...", 0xFFFFFFFF, 1);

	m_CPUThrottle.SetSpeed (CPUSpeedMaximum);

	if (!m_EMMC.Initialize ())
	{
		Fatal ("Impossibile inizializzare la scheda SD.");
	}

	if (f_mount (&m_SDFileSystem, "SD:", 1) != FR_OK)
	{
		Fatal ("Impossibile leggere la partizione FAT della scheda SD.");
	}

	if (!m_USBHCI.Initialize ())
	{
		m_Logger.Write (FromKernel, LogError, "USB init failed");
	}

	if (!m_Console.Initialize ())
		return FALSE;

	// stdout/stderr of the emulator go to the log
	CGlueStdioInit (m_Console);

	UpdateUSBDevices ();

	m_Logger.Write (FromKernel, LogNotice, "QuadraPi built " __DATE__ " " __TIME__);
	m_Logger.Write (FromKernel, LogNotice, "ARM clock %u MHz, temperature %u C",
			m_CPUThrottle.GetClockRate () / 1000000, m_CPUThrottle.GetTemperature ());

	return TRUE;
}

bool CKernel::InitFrameBuffer (void)
{
	unsigned nWidth = m_Options.GetWidth ();
	unsigned nHeight = m_Options.GetHeight ();

	if (nWidth == 0 || nHeight == 0)
	{
		CBcmPropertyTags Tags;
		TPropertyTagDisplayDimensions Dimensions;
		if (Tags.GetTag (PROPTAG_GET_DISPLAY_DIMENSIONS, &Dimensions, sizeof Dimensions))
		{
			nWidth = Dimensions.nWidth;
			nHeight = Dimensions.nHeight;
		}

		if (nWidth < 640 || nHeight < 480)
		{
			nWidth = 1024;
			nHeight = 768;
		}

		// Very large displays (4K) cost too much bandwidth; the GPU upscales
		if (nWidth > 1920 || nHeight > 1200)
		{
			nWidth = 1920;
			nHeight = 1080;
		}
	}

	m_pFrameBuffer = new CBcmFrameBuffer (nWidth, nHeight, 32);
	if (!m_pFrameBuffer->Initialize ())
	{
		delete m_pFrameBuffer;
		m_pFrameBuffer = new CBcmFrameBuffer (1024, 768, 32);
		if (!m_pFrameBuffer->Initialize ())
			return false;
	}

	// Pixel order: 0 = BGR (real Pi firmware default, 0xAARRGGBB in memory),
	// 1 = RGB (e.g. QEMU): then red and blue have to be swapped
	{
		CBcmPropertyTags Tags;
		TPropertyTagSimple Order;
		if (Tags.GetTag (0x00040006, &Order, sizeof Order))
			gfx_swap_rb = (Order.nValue == 1);
	}

	static plat_fb fb;
	fb.pixels = (uint32_t *) (uintptr) m_pFrameBuffer->GetBuffer ();
	fb.width = m_pFrameBuffer->GetWidth ();
	fb.height = m_pFrameBuffer->GetHeight ();
	fb.pitch = m_pFrameBuffer->GetPitch () / 4;
	gfx_init (&fb);

	m_Logger.Write (FromKernel, LogNotice, "Frame buffer %ux%u%s", fb.width, fb.height, gfx_swap_rb ? " (RGB)" : "");
	return true;
}

void CKernel::Run (void)
{
	// Start display refresh on core 1
	if (!m_VideoCore.Initialize ())
		Fatal ("Impossibile avviare il secondo core della CPU.");

	emulator_main ();
}

/*
 *  USB devices (keyboard, mouse, mass storage)
 */

void CKernel::UpdateUSBDevices (void)
{
	if (m_pKeyboard == 0)
	{
		m_pKeyboard = (CUSBKeyboardDevice *) m_DeviceNameService.GetDevice ("ukbd1", FALSE);
		if (m_pKeyboard != 0)
		{
			m_pKeyboard->RegisterRemovedHandler (KeyboardRemovedHandler, this);
			m_pKeyboard->RegisterKeyStatusHandlerRaw (KeyStatusHandlerRaw);
			m_Logger.Write (FromKernel, LogNotice, "USB keyboard attached");
		}
	}

	if (m_pMouse == 0)
	{
		m_pMouse = (CMouseDevice *) m_DeviceNameService.GetDevice ("mouse1", FALSE);
		if (m_pMouse != 0)
		{
			m_pMouse->RegisterRemovedHandler (MouseRemovedHandler, this);
			m_pMouse->RegisterStatusHandler (MouseStatusHandler);
			m_Logger.Write (FromKernel, LogNotice, "USB mouse attached");
		}
	}

	if (!m_bUSBMounted && m_DeviceNameService.GetDevice ("umsd1", TRUE) != 0)
	{
		if (f_mount (&m_USBFileSystem, "USB:", 1) == FR_OK)
		{
			m_bUSBMounted = true;
			m_Logger.Write (FromKernel, LogNotice, "USB storage mounted as USB:");
		}
	}
	else if (m_bUSBMounted && m_DeviceNameService.GetDevice ("umsd1", TRUE) == 0)
	{
		f_mount (0, "USB:", 0);
		m_bUSBMounted = false;
	}
}

void CKernel::KeyboardRemovedHandler (CDevice *pDevice, void *pContext)
{
	CKernel *pThis = (CKernel *) pContext;
	pThis->m_pKeyboard = 0;
}

void CKernel::MouseRemovedHandler (CDevice *pDevice, void *pContext)
{
	CKernel *pThis = (CKernel *) pContext;
	pThis->m_pMouse = 0;
}

// Called in interrupt context
void CKernel::KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6])
{
	static unsigned char s_ucLastModifiers = 0;
	static unsigned char s_LastKeys[6] = {0};

	input_modifiers = ucModifiers;

	// Modifier keys are reported as usages 0xE0..0xE7
	unsigned char ucChanged = ucModifiers ^ s_ucLastModifiers;
	for (unsigned i = 0; i < 8; i++)
	{
		if (ucChanged & (1 << i))
		{
			input_event ev = {};
			ev.type = (ucModifiers & (1 << i)) ? EV_KEY_DOWN : EV_KEY_UP;
			ev.code = 0xE0 + i;
			input_push (ev);
		}
	}
	s_ucLastModifiers = ucModifiers;

	// Released keys
	for (unsigned i = 0; i < 6; i++)
	{
		unsigned char k = s_LastKeys[i];
		if (k < 4)
			continue;
		bool bStill = false;
		for (unsigned j = 0; j < 6; j++)
			if (RawKeys[j] == k)
				bStill = true;
		if (!bStill)
		{
			input_event ev = {};
			ev.type = EV_KEY_UP;
			ev.code = k;
			input_push (ev);
		}
	}

	// Pressed keys
	for (unsigned i = 0; i < 6; i++)
	{
		unsigned char k = RawKeys[i];
		if (k < 4)		// 0 = none, 1..3 = error codes (rollover)
			continue;
		bool bWas = false;
		for (unsigned j = 0; j < 6; j++)
			if (s_LastKeys[j] == k)
				bWas = true;
		if (!bWas)
		{
			input_event ev = {};
			ev.type = EV_KEY_DOWN;
			ev.code = k;
			input_push (ev);
		}
	}

	// On rollover error keep the previous state
	if (RawKeys[0] != 1)
		memcpy (s_LastKeys, RawKeys, 6);
}

// Called in interrupt context
void CKernel::MouseStatusHandler (unsigned nButtons, int nDisplacementX, int nDisplacementY, int nWheelMove)
{
	input_event ev = {};
	ev.type = EV_MOUSE;
	ev.buttons = (uint8_t) nButtons;
	ev.dx = (int16_t) nDisplacementX;
	ev.dy = (int16_t) nDisplacementY;
	ev.wheel = (int8_t) nWheelMove;
	input_push (ev);
}

/*
 *  Housekeeping, called from the emulator on core 0
 */

void CKernel::Housekeeping (void)
{
	if (m_USBHCI.UpdatePlugAndPlay ())
		UpdateUSBDevices ();

	unsigned nNow = m_Timer.GetTicks ();
	if (nNow - m_nLastHousekeeping >= HZ)
	{
		m_nLastHousekeeping = nNow;
		m_CPUThrottle.SetOnTemperature ();
	}
}

/*
 *  Sound
 */

bool CKernel::SoundOpen (int nOutput, unsigned nRate)
{
	SoundClose ();

	if (nOutput == SOUND_OUT_JACK)
		m_pSound = new CPWMSoundBaseDevice (&m_Interrupt, nRate, 2048);
	else
		m_pSound = new CHDMISoundBaseDevice (&m_Interrupt, nRate, 384 * 10);

	if (m_pSound == 0)
		return false;

	// ~170 ms of queue: enough to ride over disk accesses
	if (!m_pSound->AllocateQueue (170))
	{
		delete m_pSound;
		m_pSound = 0;
		return false;
	}

	m_pSound->SetWriteFormat (SoundFormatSigned16, 2);

	if (!m_pSound->Start ())
	{
		m_Logger.Write (FromKernel, LogWarning, "Cannot start sound device");
		delete m_pSound;
		m_pSound = 0;
		return false;
	}

	m_nSoundRate = nRate;
	m_Logger.Write (FromKernel, LogNotice, "Sound started (%s, %u Hz)",
			nOutput == SOUND_OUT_JACK ? "jack" : "HDMI", nRate);
	return true;
}

void CKernel::SoundClose (void)
{
	if (m_pSound != 0)
	{
		m_pSound->Cancel ();
		while (m_pSound->IsActive ())
			m_Timer.MsDelay (1);
		delete m_pSound;
		m_pSound = 0;
	}
	m_nSoundRate = 0;
}

unsigned CKernel::SoundFramesFree (void)
{
	if (m_pSound == 0)
		return 0;
	return m_pSound->GetQueueFramesAvail ();
}

void CKernel::SoundWrite (const s16 *pStereo, unsigned nFrames)
{
	if (m_pSound != 0 && nFrames > 0)
		m_pSound->Write (pStereo, nFrames * 2 * sizeof (s16));
}

/*
 *  Error / halt screens
 */

void CKernel::DrawTextScreen (const char *pTitle, const char *pBody, u32 nColor)
{
	if (m_pFrameBuffer == 0)
		return;

	gfx_set_overlay_owner_core0 ();
	gfx_clear (0xFF000000);
	unsigned y = 40;
	gfx_text (40, y, pTitle, nColor, 2);
	y += 60;

	// Body, line by line
	char line[160];
	const char *p = pBody;
	while (*p && y + 20 < gfx_height ())
	{
		unsigned n = 0;
		while (*p && *p != '\n' && n < sizeof line - 1)
			line[n++] = *p++;
		line[n] = '\0';
		if (*p == '\n')
			p++;
		gfx_text (40, y, line, 0xFFC0C0C0, 1);
		y += 20;
	}
}

void CKernel::Fatal (const char *pMsg)
{
	m_Logger.Write (FromKernel, LogError, "FATAL: %s", pMsg);

	static char body[2048];
	static char tail[1200];
	m_LogTee.GetTail (tail, sizeof tail);

	// Keep only the last lines of the log
	const char *pTail = tail;
	int nLines = 0;
	for (const char *q = tail + strlen (tail); q > tail; q--)
		if (*q == '\n' && ++nLines > 14)
		{
			pTail = q + 1;
			break;
		}

	snprintf (body, sizeof body,
		  "%s\n\nControlla la cartella /quadra sulla scheda SD (vedi LEGGIMI.txt).\n"
		  "Premi il pulsante di reset o scollega l'alimentazione per riavviare.\n\n"
		  "--- ultime righe del log ---\n%s", pMsg, pTail);

	DrawTextScreen ("QuadraPi - errore", body, 0xFFFF6060);

	for (;;)
		m_Timer.MsDelay (1000);
}

void CKernel::Halt (void)
{
	SoundClose ();
	DrawTextScreen ("QuadraPi", "Il Macintosh è stato spento.\n\n"
			"Ora puoi scollegare l'alimentazione del Raspberry Pi in sicurezza.", 0xFFFFFFFF);
	m_Timer.MsDelay (200);
	halt ();
	for (;;);
}

/*
 *  platform.h C-style wrappers
 */

uint64_t plat_usec (void)
{
	return CTimer::GetClockTicks64 ();
}

void plat_housekeeping (void)
{
	CKernel::Get ()->Housekeeping ();
}

void plat_log (const char *fmt, ...)
{
	va_list var;
	va_start (var, fmt);
	CString Msg;
	Msg.FormatV (fmt, var);
	va_end (var);
	CLogger::Get ()->Write ("emu", LogNotice, "%s", (const char *) Msg);
}

void plat_fatal (const char *msg)
{
	CKernel::Get ()->Fatal (msg);
	for (;;);
}

void plat_reboot (void)
{
	CKernel::Get ()->SoundClose ();
	CTimer::Get ()->MsDelay (100);
	reboot ();
	for (;;);
}

void plat_halt (void)
{
	CKernel::Get ()->Halt ();
	for (;;);
}

unsigned plat_cpu_temp (void)
{
	return CKernel::Get ()->CPUTemp ();
}

unsigned plat_arm_clock_mhz (void)
{
	return CKernel::Get ()->ARMClockMHz ();
}

unsigned plat_ram_mb (void)
{
	return CMachineInfo::Get ()->GetRAMSize ();
}

size_t plat_heap_free (void)
{
	return CMemorySystem::Get ()->GetHeapFreeSpace (HEAP_LOW);
}

bool plat_usb_storage_present (void)
{
	return CKernel::Get ()->USBStoragePresent ();
}

bool plat_sound_open (int output, unsigned sample_rate)
{
	return CKernel::Get ()->SoundOpen (output, sample_rate);
}

void plat_sound_close (void)
{
	CKernel::Get ()->SoundClose ();
}

unsigned plat_sound_frames_free (void)
{
	return CKernel::Get ()->SoundFramesFree ();
}

void plat_sound_write (const int16_t *stereo, unsigned frames)
{
	CKernel::Get ()->SoundWrite (stereo, frames);
}

unsigned plat_sound_rate (void)
{
	return CKernel::Get ()->SoundRate ();
}

/*
 *  Input event ring (single producer: USB IRQ, single consumer: emulator)
 */

volatile uint8_t input_modifiers = 0;

static const unsigned INPUT_RING = 256;
static input_event s_InputRing[INPUT_RING];
static volatile unsigned s_nInputHead = 0;	// written by producer
static volatile unsigned s_nInputTail = 0;	// written by consumer

void input_push (const input_event &ev)
{
	unsigned nNext = (s_nInputHead + 1) % INPUT_RING;
	if (nNext == s_nInputTail)
		return;		// full, drop
	s_InputRing[s_nInputHead] = ev;
	DataMemBarrier ();
	s_nInputHead = nNext;
}

bool input_pop (input_event &ev)
{
	if (s_nInputTail == s_nInputHead)
		return false;
	DataMemBarrier ();
	ev = s_InputRing[s_nInputTail];
	DataMemBarrier ();
	s_nInputTail = (s_nInputTail + 1) % INPUT_RING;
	return true;
}
