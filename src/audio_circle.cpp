/*
 *  audio_circle.cpp - Sound output for QuadraPi (HDMI or 3.5 mm jack)
 *
 *  The Mac Sound Manager delivers blocks of samples through the emulated
 *  sound component (audio.cpp). Blocks are requested whenever the Circle
 *  sound queue has room, converted to 16 bit stereo, resampled to the
 *  output rate and queued.
 *
 *  Based on SDL/audio_sdl.cpp from Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "prefs.h"
#include "user_strings.h"
#include "audio.h"
#include "audio_defs.h"
#include "platform.h"
#include "qp.h"

#define DEBUG 0
#include "debug.h"

#define MAC_MAX_VOLUME 0x0100

// The currently selected audio parameters (indices in audio_sample_rates[] etc. vectors)
static int audio_sample_rate_index = 0;
static int audio_sample_size_index = 0;
static int audio_channel_count_index = 0;

static int main_volume = MAC_MAX_VOLUME;
static int speaker_volume = MAC_MAX_VOLUME;
static bool main_mute = false;
static bool speaker_mute = false;
static int pref_volume = 80;		// percent
static bool menu_muted = false;

static bool irq_pending = false;
static uint64 irq_time = 0;
static unsigned out_rate = 0;

// Resampler state (16.16 fixed point position)
static uint32 rs_pos = 0;
static int16 rs_prev_l = 0, rs_prev_r = 0;

static const int FRAMES_PER_BLOCK = 1024;
static int16 *conv_buf = NULL;		// Mac block as 16 bit stereo
static int16 *out_buf = NULL;		// resampled

static void set_audio_status_format(void)
{
	AudioStatus.sample_rate = audio_sample_rates[audio_sample_rate_index];
	AudioStatus.sample_size = audio_sample_sizes[audio_sample_size_index];
	AudioStatus.channels = audio_channel_counts[audio_channel_count_index];
}

static bool open_audio(void)
{
	if (audio_sample_sizes.empty()) {
		audio_sample_rates.push_back(11025 << 16);
		audio_sample_rates.push_back(22050 << 16);
		audio_sample_rates.push_back(44100 << 16);
		audio_sample_sizes.push_back(8);
		audio_sample_sizes.push_back(16);
		audio_channel_counts.push_back(1);
		audio_channel_counts.push_back(2);

		// Default to highest supported values
		audio_sample_rate_index = audio_sample_rates.size() - 1;
		audio_sample_size_index = audio_sample_sizes.size() - 1;
		audio_channel_count_index = audio_channel_counts.size() - 1;
	}

	int output = PrefsFindInt32("soundout");
	unsigned rate = (output == SOUND_OUT_JACK) ? 44100 : 48000;
	if (!plat_sound_open(output, rate)) {
		// Not an error worth a dialog: many HDMI monitors have no speakers
		plat_log("No sound output available (%s)", output == SOUND_OUT_JACK ? "jack" : "HDMI");
		return false;
	}
	out_rate = plat_sound_rate();

	if (!conv_buf)
		conv_buf = (int16 *)malloc(FRAMES_PER_BLOCK * 2 * sizeof(int16));
	if (!out_buf)
		out_buf = (int16 *)malloc((FRAMES_PER_BLOCK * 6 + 16) * 2 * sizeof(int16));

	audio_frames_per_block = FRAMES_PER_BLOCK;
	set_audio_status_format();
	irq_pending = false;
	audio_open = true;
	return true;
}

static void close_audio(void)
{
	plat_sound_close();
	audio_open = false;
	irq_pending = false;
}

void AudioInit(void)
{
	// Init audio status and feature flags
	AudioStatus.sample_rate = 44100 << 16;
	AudioStatus.sample_size = 16;
	AudioStatus.channels = 2;
	AudioStatus.mixer = 0;
	AudioStatus.num_sources = 0;
	audio_component_flags = cmpWantsRegisterMessage | kStereoOut | k16BitOut;

	pref_volume = PrefsFindInt32("volume");

	// Sound disabled in prefs? Then do nothing
	if (PrefsFindBool("nosound"))
		return;

	open_audio();
}

void AudioExit(void)
{
	close_audio();
}

// Called from the menu after the output or volume was changed
void audio_apply_prefs(void)
{
	pref_volume = PrefsFindInt32("volume");
	bool want = !PrefsFindBool("nosound");
	if (audio_open)
		close_audio();
	if (want)
		open_audio();
}

void audio_mute_for_menu(bool muted)
{
	menu_muted = muted;
}

void audio_enter_stream()
{
}

void audio_exit_stream()
{
}


/*
 *  Called often from the CPU loop: request a new block when there is room
 */

static unsigned out_frames_for_block(void)
{
	uint32 mac_rate = AudioStatus.sample_rate >> 16;
	if (mac_rate == 0)
		mac_rate = 44100;
	return (unsigned)((uint64)FRAMES_PER_BLOCK * out_rate / mac_rate) + 2;
}

void audio_service(void)
{
	if (!audio_open || !AudioStatus.num_sources)
		return;
	if (irq_pending) {
		// The Mac may have interrupts disabled for a while; retry later
		if (GetTicks_usec() - irq_time < 200000)
			return;
		irq_pending = false;
	}
	if (plat_sound_frames_free() < out_frames_for_block())
		return;
	irq_pending = true;
	irq_time = GetTicks_usec();
	SetInterruptFlag(INTFLAG_AUDIO);
	TriggerInterrupt();
}


/*
 *  MacOS audio interrupt, read next data block
 */

static int volume_factor(void)		// 0..256
{
	if (main_mute || speaker_mute || menu_muted)
		return 0;
	int v = main_volume * speaker_volume / MAC_MAX_VOLUME;	// 0..256
	return v * pref_volume / 100;
}

static void resample_and_write(int frames)
{
	uint32 mac_rate = AudioStatus.sample_rate >> 16;
	uint32 step = (uint32)(((uint64)mac_rate << 16) / out_rate);	// input frames per output frame
	int vol = volume_factor();

	int n = 0;
	int max_out = FRAMES_PER_BLOCK * 6 + 8;
	while (n < max_out) {
		uint32 idx = rs_pos >> 16;
		if ((int)idx >= frames)
			break;
		uint32 frac = rs_pos & 0xffff;
		int l0 = idx == 0 ? rs_prev_l : conv_buf[(idx - 1) * 2];
		int r0 = idx == 0 ? rs_prev_r : conv_buf[(idx - 1) * 2 + 1];
		int l1 = conv_buf[idx * 2], r1 = conv_buf[idx * 2 + 1];
		int l = l0 + (int)(((int64)(l1 - l0) * frac) >> 16);
		int r = r0 + (int)(((int64)(r1 - r0) * frac) >> 16);
		out_buf[n * 2] = (int16)((l * vol) >> 8);
		out_buf[n * 2 + 1] = (int16)((r * vol) >> 8);
		n++;
		rs_pos += step;
	}
	rs_pos -= (uint32)frames << 16;
	if (frames > 0) {
		rs_prev_l = conv_buf[(frames - 1) * 2];
		rs_prev_r = conv_buf[(frames - 1) * 2 + 1];
	}
	plat_sound_write(out_buf, n);
}

void AudioInterrupt(void)
{
	D(bug("AudioInterrupt\n"));

	// Get data from apple mixer
	if (AudioStatus.mixer) {
		M68kRegisters r;
		r.a[0] = audio_data + adatStreamInfo;
		r.a[1] = AudioStatus.mixer;
		Execute68k(audio_data + adatGetSourceData, &r);
		D(bug(" GetSourceData() returns %08lx\n", r.d[0]));
	} else
		WriteMacInt32(audio_data + adatStreamInfo, 0);

	irq_pending = false;
	if (!audio_open)
		return;

	uint32 apple_stream_info = ReadMacInt32(audio_data + adatStreamInfo);
	int frames = 0;
	if (apple_stream_info) {
		frames = ReadMacInt32(apple_stream_info + scd_sampleCount);
		if (frames > FRAMES_PER_BLOCK)
			frames = FRAMES_PER_BLOCK;
		int channels = ReadMacInt16(apple_stream_info + scd_numChannels);
		int size = ReadMacInt16(apple_stream_info + scd_sampleSize);
		uint8 *src = Mac2HostAddr(ReadMacInt32(apple_stream_info + scd_buffer));

		for (int i = 0; i < frames; i++) {
			int l, r;
			if (size == 8) {
				// 8 bit samples are unsigned (offset binary)
				if (channels == 2) {
					l = ((int)src[i * 2] - 128) << 8;
					r = ((int)src[i * 2 + 1] - 128) << 8;
				} else
					l = r = ((int)src[i] - 128) << 8;
			} else {
				if (channels == 2) {
					l = (int16)((src[i * 4] << 8) | src[i * 4 + 1]);
					r = (int16)((src[i * 4 + 2] << 8) | src[i * 4 + 3]);
				} else
					l = r = (int16)((src[i * 2] << 8) | src[i * 2 + 1]);
			}
			conv_buf[i * 2] = l;
			conv_buf[i * 2 + 1] = r;
		}
	}

	if (frames == 0) {
		// Keep the stream going with a short silence
		frames = 256;
		memset(conv_buf, 0, frames * 2 * sizeof(int16));
	}
	resample_and_write(frames);
}


/*
 *  Set sampling parameters
 *  "index" is an index into the audio_sample_rates[] etc. vectors
 *  It is guaranteed that AudioStatus.num_sources == 0
 */

bool audio_set_sample_rate(int index)
{
	audio_sample_rate_index = index;
	set_audio_status_format();
	rs_pos = 0;
	return true;
}

bool audio_set_sample_size(int index)
{
	audio_sample_size_index = index;
	set_audio_status_format();
	return true;
}

bool audio_set_channels(int index)
{
	audio_channel_count_index = index;
	set_audio_status_format();
	return true;
}


/*
 *  Get/set volume controls
 */

bool audio_get_main_mute(void)
{
	return main_mute;
}

uint32 audio_get_main_volume(void)
{
	uint32 chan = main_volume;
	return (chan << 16) + chan;
}

bool audio_get_speaker_mute(void)
{
	return speaker_mute;
}

uint32 audio_get_speaker_volume(void)
{
	uint32 chan = speaker_volume;
	return (chan << 16) + chan;
}

void audio_set_main_mute(bool mute)
{
	main_mute = mute;
}

void audio_set_main_volume(uint32 vol)
{
	main_volume = ((vol >> 16) + (vol & 0xffff)) / 2;
	if (main_volume > MAC_MAX_VOLUME)
		main_volume = MAC_MAX_VOLUME;
}

void audio_set_speaker_mute(bool mute)
{
	speaker_mute = mute;
}

void audio_set_speaker_volume(uint32 vol)
{
	speaker_volume = ((vol >> 16) + (vol & 0xffff)) / 2;
	if (speaker_volume > MAC_MAX_VOLUME)
		speaker_volume = MAC_MAX_VOLUME;
}
